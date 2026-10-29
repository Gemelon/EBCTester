/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ebc_link.h"
#include "ebc_model.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define TESTER_UART UART_NUM_1
#define DONGLE_UART UART_NUM_2
#define STATUS_FRAME_LENGTH 19
#define COMMAND_FRAME_LENGTH 10
#define FRAME_START 0xFA
#define FRAME_END 0xF8
#define CONNECTION_TIMEOUT_US (5 * 1000 * 1000)
#define CONNECT_RETRY_US (3 * 1000 * 1000)
#define TICK_US (500 * 1000)

static const char *TAG = "ebc_link";

static SemaphoreHandle_t lock;
static ebc_link_listener_t listener;
static bool connected;
static bool has_frame;
static bool auto_connect;
static bool pending_valid;
static uint8_t pending_command;
static uint16_t pending_params[3];
static ebc_frame_t last_frame;
static uint32_t frames_ok;
static uint32_t frames_bad;
static int64_t last_rx_us;
static int64_t last_connect_try_us;
static uint8_t active_model;
static bool active_model_valid;

const char *ebc_link_mode_name(uint8_t mode)
{
    switch (mode) {
    case 0x00:
        return "D-CC stopped";
    case 0x01:
        return "D-CP stopped";
    case 0x02:
        return "C-CV stopped";
    case 0x0A:
        return "D-CC aktiv";
    case 0x0B:
        return "D-CP aktiv";
    case 0x0C:
        return "C-CV aktiv";
    case 0x14:
        return "D-CC completed";
    case 0x15:
        return "D-CP completed";
    case 0x16:
        return "C-CV completed";
    default:
        return "unknown";
    }
}

bool ebc_link_mode_is_active(uint8_t mode)
{
    return mode == 0x0A || mode == 0x0B || mode == 0x0C;
}

bool ebc_link_mode_is_idle(uint8_t mode)
{
    return mode == 0x00 || mode == 0x01 || mode == 0x02 || mode == 0x14 || mode == 0x15 || mode == 0x16;
}

static void send_frame(uint8_t command, uint16_t p1, uint16_t p2, uint16_t p3)
{
    uint8_t frame[COMMAND_FRAME_LENGTH] = {
        FRAME_START, command,
        (uint8_t)(p1 / 240), (uint8_t)(p1 % 240),
        (uint8_t)(p2 / 240), (uint8_t)(p2 % 240),
        (uint8_t)(p3 / 240), (uint8_t)(p3 % 240),
        0, FRAME_END};
    uint8_t checksum = 0;
    for (int i = 1; i <= 7; ++i) {
        checksum ^= frame[i];
    }
    frame[8] = checksum;
    ESP_LOGI(TAG, "Sending command 0x%02X (%u, %u, %u)", command, p1, p2, p3);
    uart_write_bytes(TESTER_UART, frame, sizeof(frame));
}

static bool decode_frame(const uint8_t *buffer, ebc_frame_t *frame)
{
    uint8_t checksum = 0;
    for (int i = 1; i <= 16; ++i) {
        checksum ^= buffer[i];
    }
    /* The EBC never sends bytes >= 0xF0 inside a frame, so such checksums lose the high nibble. */
    const uint8_t received = buffer[17];
    if (!(received == checksum || ((checksum & 0xF0) == 0xF0 && received == (checksum & 0x0F)))) {
        ESP_LOGW(TAG, "Checksum mismatch: received 0x%02X, calculated 0x%02X", received, checksum);
        return false;
    }
#define VALUE(i) ((int)buffer[i] * 240 + buffer[(i) + 1])
    memset(frame, 0, sizeof(*frame));
    frame->mode = buffer[1];
    frame->model = buffer[16]; /* Final payload byte, immediately before checksum and frame end. */
    frame->auxiliary = (frame->mode >= 0x64 && frame->mode <= 0x66) ||
                       (frame->mode >= 0x6E && frame->mode <= 0x70);
    if (!frame->auxiliary) {
        const uint8_t high = buffer[6];
        const uint8_t low = buffer[7];
        if ((high & 0xE0) == 0xE0) {
            frame->capacity_ah = ((high & 0x3F) * 240 + low - 0x1C00) / 10.0f;
        } else if (high & 0x80) {
            frame->capacity_ah = ((high & 0x7F) * 240 + low - 0x800) / 100.0f;
        } else {
            frame->capacity_ah = (high * 240 + low) / 1000.0f;
        }
        frame->current_a = VALUE(2) * 0.01f;
        frame->voltage_v = VALUE(4) * 0.001f;
        frame->setpoint_1 = VALUE(10) * 0.01f;
        frame->setpoint_2 = VALUE(12) * 0.01f;
        frame->setpoint_3 = VALUE(14) * 0.01f;
    }
#undef VALUE
    return true;
}

static void handle_frame(const uint8_t *buffer, int64_t now)
{
    ebc_frame_t frame;
    if (!decode_frame(buffer, &frame)) {
        xSemaphoreTake(lock, portMAX_DELAY);
        frames_bad++;
        xSemaphoreGive(lock);
        return;
    }
    bool model_changed = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    frames_ok++;
    last_rx_us = now;
    if (!connected) {
        connected = true;
        ESP_LOGI(TAG, "Connected to EBC (model 0x%02X)", frame.model);
    }
    if (!active_model_valid || active_model != frame.model) {
        active_model = frame.model;
        active_model_valid = true;
        model_changed = true;
    }
    if (!frame.auxiliary) {
        last_frame = frame;
        has_frame = true;
    }
    xSemaphoreGive(lock);

    if (model_changed) {
        ebc_model_limits_t limits;
        ebc_model_get_limits(frame.model, &limits);
        if (limits.known) {
            ESP_LOGI(TAG, "Detected %s (ident 0x%02X)", limits.name, frame.model);
        } else {
            ESP_LOGW(TAG,
                     "Unknown EBC ident 0x%02X; using conservative limits "
                     "(charge 5 V/5 A, discharge 5 V/20 A/85 W)",
                     frame.model);
        }
    }

    if (listener.on_frame != NULL) {
        listener.on_frame(&frame, listener.context);
    }

    /* Commands are only sent directly after a valid frame to avoid collisions. */
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool send = pending_valid;
    const uint8_t command = pending_command;
    uint16_t params[3];
    memcpy(params, pending_params, sizeof(params));
    pending_valid = false;
    xSemaphoreGive(lock);
    if (send) {
        send_frame(command, params[0], params[1], params[2]);
    }
}

static void link_task(void *arg)
{
    uint8_t chunk[64];
    uint8_t frame[STATUS_FRAME_LENGTH];
    size_t length = 0;
    bool in_frame = false;
    int64_t last_tick = 0;
    for (;;) {
        const int received = uart_read_bytes(TESTER_UART, chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        const int64_t now = esp_timer_get_time();
        for (int i = 0; i < received; ++i) {
            const uint8_t byte = chunk[i];
            if (byte == FRAME_START) {
                frame[0] = byte;
                length = 1;
                in_frame = true;
                continue;
            }
            if (!in_frame) {
                continue;
            }
            if (length >= STATUS_FRAME_LENGTH) {
                in_frame = false;
                xSemaphoreTake(lock, portMAX_DELAY);
                frames_bad++;
                xSemaphoreGive(lock);
                ESP_LOGW(TAG, "Frame too long; discarded");
                continue;
            }
            frame[length++] = byte;
            if (byte != FRAME_END) {
                continue;
            }
            in_frame = false;
            if (length != STATUS_FRAME_LENGTH) {
                xSemaphoreTake(lock, portMAX_DELAY);
                frames_bad++;
                xSemaphoreGive(lock);
                ESP_LOGW(TAG, "Frame length %u is invalid; discarded", (unsigned)length);
                continue;
            }
            handle_frame(frame, now);
        }

        /* The dongle bridge is not implemented yet; keep its receive buffer empty. */
        uart_flush_input(DONGLE_UART);

        bool lost = false;
        bool try_connect = false;
        xSemaphoreTake(lock, portMAX_DELAY);
        if (connected && now - last_rx_us > CONNECTION_TIMEOUT_US) {
            connected = false;
            has_frame = false;
            if (pending_valid) {
                ESP_LOGW(TAG, "Discarding pending command 0x%02X after connection loss", pending_command);
                pending_valid = false;
            }
            lost = true;
        }
        if (!connected && auto_connect && now - last_connect_try_us >= CONNECT_RETRY_US) {
            last_connect_try_us = now;
            try_connect = true;
        }
        xSemaphoreGive(lock);
        if (lost) {
            ESP_LOGW(TAG, "No valid frames received for 5 seconds; connection lost");
            if (listener.on_connection_lost != NULL) {
                listener.on_connection_lost(listener.context);
            }
        }
        if (try_connect) {
            send_frame(EBC_CMD_CONNECT, 0, 0, 0);
        }
        if (now - last_tick >= TICK_US) {
            last_tick = now;
            if (listener.on_tick != NULL) {
                listener.on_tick(listener.context);
            }
        }
    }
}

/* Releases the EBC keypad lock before a planned restart (e.g. after OTA). */
static void shutdown_handler(void)
{
    if (connected) {
        send_frame(EBC_CMD_DISCONNECT, 0, 0, 0);
        uart_wait_tx_done(TESTER_UART, pdMS_TO_TICKS(50));
    }
}

static esp_err_t configure_uart(uart_port_t port, int tx, int rx, int baud)
{
    const uart_config_t config = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_ODD,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(port, 1024, 256, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(port, &config);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d setup failed: %s", (int)port, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "UART%d ready: TX=%d RX=%d %d baud 8O1", (int)port, tx, rx, baud);
    return ESP_OK;
}

esp_err_t ebc_link_init(const ebc_link_listener_t *link_listener)
{
    if (lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock = xSemaphoreCreateMutex();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (link_listener != NULL) {
        listener = *link_listener;
    }
    esp_err_t err = configure_uart(TESTER_UART, CONFIG_EBC_TESTER_UART_TX_GPIO,
                                   CONFIG_EBC_TESTER_UART_RX_GPIO, CONFIG_EBC_TESTER_UART_BAUD);
    if (err == ESP_OK) {
        err = configure_uart(DONGLE_UART, CONFIG_EBC_DONGLE_UART_TX_GPIO, CONFIG_EBC_DONGLE_UART_RX_GPIO,
                             CONFIG_EBC_DONGLE_UART_BAUD);
    }
    if (err != ESP_OK) {
        return err;
    }
    err = esp_register_shutdown_handler(shutdown_handler);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot register shutdown handler: %s", esp_err_to_name(err));
    }
    if (xTaskCreate(link_task, "ebc_link", 8192, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ebc_link_get_status(ebc_link_status_t *status)
{
    memset(status, 0, sizeof(*status));
    if (lock == NULL) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    status->connected = connected;
    status->has_frame = has_frame;
    status->has_model = active_model_valid;
    status->model = active_model;
    status->auto_connect = auto_connect;
    status->command_pending = pending_valid;
    status->frame = last_frame;
    status->frames_ok = frames_ok;
    status->frames_bad = frames_bad;
    status->last_frame_age_ms = last_rx_us > 0 ? (uint32_t)((now - last_rx_us) / 1000) : UINT32_MAX;
    xSemaphoreGive(lock);
}

esp_err_t ebc_link_queue_command(uint8_t command, uint16_t p1, uint16_t p2, uint16_t p3)
{
    if (lock == NULL || p1 > EBC_PARAM_MAX || p2 > EBC_PARAM_MAX || p3 > EBC_PARAM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Stop and disconnect are safety commands and replace any pending command. */
    const bool priority = command == EBC_CMD_STOP || command == EBC_CMD_DISCONNECT;
    esp_err_t err = ESP_OK;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!connected || (pending_valid && !priority)) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        pending_valid = true;
        pending_command = command;
        pending_params[0] = p1;
        pending_params[1] = p2;
        pending_params[2] = p3;
    }
    xSemaphoreGive(lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Command 0x%02X queued for the next valid EBC frame", command);
    }
    return err;
}

esp_err_t ebc_link_connect(void)
{
    if (lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool already = connected;
    last_connect_try_us = esp_timer_get_time();
    xSemaphoreGive(lock);
    if (!already) {
        send_frame(EBC_CMD_CONNECT, 0, 0, 0);
    }
    return ESP_OK;
}

esp_err_t ebc_link_disconnect(void)
{
    if (lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    auto_connect = false;
    const bool is_connected = connected;
    xSemaphoreGive(lock);
    if (is_connected) {
        return ebc_link_queue_command(EBC_CMD_DISCONNECT, 0, 0, 0);
    }
    send_frame(EBC_CMD_DISCONNECT, 0, 0, 0);
    return ESP_OK;
}

void ebc_link_set_auto_connect(bool enabled)
{
    if (lock == NULL) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    auto_connect = enabled;
    xSemaphoreGive(lock);
}
