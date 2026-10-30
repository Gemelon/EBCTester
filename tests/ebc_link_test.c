/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "ebc_link.h"
#include "ebc_model.h"

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(value) (value)
#define pdPASS 1
#define UART_NUM_1 1
#define UART_NUM_2 2
#define UART_DATA_8_BITS 8
#define UART_PARITY_ODD 1
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE -1
#define CONFIG_EBC_TESTER_UART_TX_GPIO 1
#define CONFIG_EBC_TESTER_UART_RX_GPIO 2
#define CONFIG_EBC_TESTER_UART_BAUD 9600
#define CONFIG_EBC_DONGLE_UART_TX_GPIO 3
#define CONFIG_EBC_DONGLE_UART_RX_GPIO 4
#define CONFIG_EBC_DONGLE_UART_BAUD 9600

typedef int uart_port_t;
typedef void *SemaphoreHandle_t;
typedef struct {
    int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk;
} uart_config_t;

static int64_t clock_us;
static unsigned sent_count, frame_count, lost_count, ticks;
static uint8_t sent_command;
static jmp_buf task_exit;
static int reads_remaining;

static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static void xSemaphoreTake(SemaphoreHandle_t mutex, int wait) { (void)mutex; (void)wait; }
static void xSemaphoreGive(SemaphoreHandle_t mutex) { (void)mutex; }
static int xTaskCreate(void (*task)(void *), const char *name, int size, void *arg, int priority, void *handle)
{
    (void)task; (void)name; (void)size; (void)arg; (void)priority; (void)handle;
    return pdPASS;
}
static int uart_write_bytes(int port, const void *data, size_t size)
{
    (void)port;
    assert(size == 10);
    sent_count++;
    sent_command = ((const uint8_t *)data)[1];
    return (int)size;
}
static int uart_read_bytes(int port, void *data, size_t size, int wait)
{
    (void)port; (void)data; (void)size; (void)wait;
    if (reads_remaining-- == 0) longjmp(task_exit, 1);
    return 0;
}
static void uart_flush_input(int port) { (void)port; }
static void uart_wait_tx_done(int port, int wait) { (void)port; (void)wait; }
static esp_err_t uart_driver_install(int port, int rx, int tx, int queue, void *handle, int flags)
{
    (void)port; (void)rx; (void)tx; (void)queue; (void)handle; (void)flags;
    return ESP_OK;
}
static esp_err_t uart_param_config(int port, const uart_config_t *config)
{
    (void)port; (void)config; return ESP_OK;
}
static esp_err_t uart_set_pin(int port, int tx, int rx, int rts, int cts)
{
    (void)port; (void)tx; (void)rx; (void)rts; (void)cts; return ESP_OK;
}
static esp_err_t esp_register_shutdown_handler(void (*handler)(void)) { (void)handler; return ESP_OK; }
static int64_t esp_timer_get_time(void) { return clock_us; }

#include "../components/ebc_link/ebc_link.c"

static void received_frame(const ebc_frame_t *frame, void *context)
{
    (void)context;
    assert(frame->model == expected_model);
    frame_count++;
}
static void connection_lost(void *context) { (void)context; lost_count++; }
static void tick(void *context) { (void)context; ticks++; }

static void receive(uint8_t model, uint8_t mode, bool valid_checksum)
{
    uint8_t frame[19] = {0xFA, 0};
    frame[1] = mode;
    frame[16] = model;
    for (int i = 1; i <= 16; ++i) frame[17] ^= frame[i];
    if (!valid_checksum) frame[17] ^= 1;
    frame[18] = 0xF8;
    handle_frame(frame, clock_us);
}

static ebc_link_status_t status(void)
{
    ebc_link_status_t result;
    ebc_link_get_status(&result);
    return result;
}

static void run_iteration(void)
{
    reads_remaining = 1;
    if (setjmp(task_exit) == 0) link_task(NULL);
}

int main(void)
{
    const ebc_link_listener_t callbacks = {received_frame, connection_lost, tick, NULL};
    assert(ebc_link_init(&callbacks) == ESP_OK);
    assert(ebc_link_connect() == ESP_ERR_INVALID_STATE);
    receive(0x09, 0, true);
    assert(!status().connected && frame_count == 0);

    ebc_model_limits_t limits;
    ebc_model_get_limits(0x09, &limits);
    assert(limits.known && limits.max_charge_current_a == 5 && limits.max_discharge_voltage_v == 30);
    ebc_model_get_limits(0x10, &limits);
    assert(limits.known && limits.max_charge_current_a == 40 && limits.max_discharge_current_a == 40);
    assert(limits.max_charge_voltage_v == 5 && limits.max_power_w == 200);
    ebc_model_get_limits(0x17, &limits);
    assert(!limits.known);

    ebc_link_set_expected_model(0x09);
    assert(ebc_link_connect() == ESP_OK && sent_command == EBC_CMD_CONNECT);
    receive(0x10, 0, false);
    assert(!status().connected && !status().model_mismatch && frame_count == 0);
    receive(0x09, 0, true);
    assert(status().connected && status().has_frame && frame_count == 1);
    assert(ebc_link_queue_command(EBC_CMD_CHARGE_CV, 50, 345, 10) == ESP_OK);
    receive(0x09, 0, true);
    assert(sent_command == EBC_CMD_CHARGE_CV && frame_count == 2);

    assert(ebc_link_queue_command(EBC_CMD_DISCHARGE_CC, 50, 250, 239) == ESP_OK);
    ebc_link_set_auto_connect(true);
    receive(0x10, 0x64, true);
    assert(status().model_mismatch && !status().connected && !status().has_frame);
    assert(status().model == 0x10 && status().expected_model == 0x09);
    assert(!status().command_pending && sent_command == EBC_CMD_DISCONNECT);
    assert(frame_count == 2 && lost_count == 1);
    assert(ebc_link_queue_command(EBC_CMD_CHARGE_CV, 50, 345, 10) == ESP_ERR_INVALID_STATE);
    const unsigned rejected_sent_count = sent_count;
    receive(0x09, 0, true);
    receive(0x10, 0, true);
    clock_us = 10000000;
    run_iteration();
    assert(!status().connected && status().model_mismatch && sent_count == rejected_sent_count);

    assert(ebc_link_connect() == ESP_OK);
    receive(0x09, 0, true);
    assert(status().connected && !status().model_mismatch && frame_count == 3);
    assert(ebc_link_disconnect() == ESP_OK);
    receive(0x09, 0, true);
    assert(!status().connected && !status().has_frame && !status().auto_connect);
    assert(sent_command == EBC_CMD_DISCONNECT && frame_count == 3);
    receive(0x09, 0, true);
    assert(!status().connected && frame_count == 3);

    assert(ebc_link_connect() == ESP_OK);
    receive(0x17, 0, true);
    assert(status().model_mismatch && !status().connected && frame_count == 3);
    ebc_link_set_expected_model(0x10);
    assert(!status().model_mismatch && !status().has_model);
    ebc_link_set_auto_connect(true);
    clock_us += 4000000;
    run_iteration();
    assert(sent_command == EBC_CMD_CONNECT);
    receive(0x10, 0x64, true);
    assert(status().connected && !status().has_frame && frame_count == 4);
    receive(0x10, 0, true);
    assert(status().has_frame && frame_count == 5);

    ebc_link_set_expected_model(0x09);
    assert(status().command_pending && !status().has_frame);
    receive(0x10, 0, true);
    assert(!status().connected && !status().model_mismatch && sent_command == EBC_CMD_DISCONNECT);
    clock_us += 4000000;
    run_iteration();
    receive(0x09, 0, true);
    assert(status().connected && frame_count == 6);
    clock_us += 6000000;
    run_iteration();
    assert(!status().connected && !status().has_frame);
    assert(sent_command == EBC_CMD_CONNECT);
    puts("EBC link tests passed: selection, model limits, checksum, match, mismatch, auxiliary frames, pending command discard, retry lockout, disconnect, model change and timeout.");
    return 0;
}
