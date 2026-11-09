/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rtc_clock.h"
#include "ds3231_time.h"
#include "ebc_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/time.h>
#include <time.h>

static const char *TAG = "rtc_clock";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t rtc;
static TaskHandle_t worker;
static bool enabled;

static esp_err_t read_registers(uint8_t address, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(rtc, &address, 1, data, length, 100);
}

static esp_err_t write_register(uint8_t address, uint8_t value)
{
    const uint8_t data[] = {address, value};
    return i2c_master_transmit(rtc, data, sizeof(data), 100);
}

static esp_err_t sync_from_rtc(void)
{
    uint8_t registers[16];
    esp_err_t err = read_registers(0, registers, sizeof(registers));
    if (err == ESP_OK) err = write_register(0x0e, (registers[14] & ~0xc3) | 0x04);
    int64_t utc;
    if (err == ESP_OK && ((registers[15] & 0x80) || !ds3231_decode_time(registers, &utc))) {
        ESP_LOGW(TAG, "RTC time is invalid or oscillator stopped; waiting for NTP");
        return ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK) return err;
    /* A newly received NTP update wins over an in-flight RTC read. */
    if (worker != NULL && ulTaskNotifyTake(pdFALSE, 0) != 0) {
        xTaskNotifyGive(worker);
        return ESP_OK;
    }
    const struct timeval tv = {.tv_sec = (time_t)utc, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) return ESP_FAIL;
    ESP_LOGI(TAG, "Device time synchronized from RTC");
    return ESP_OK;
}

static esp_err_t sync_to_rtc(void)
{
    uint8_t data[8] = {0};
    if (!ds3231_encode_time((int64_t)time(NULL), data + 1)) return ESP_ERR_INVALID_STATE;
    uint8_t control, status;
    esp_err_t err = read_registers(0x0e, &control, 1);
    if (err == ESP_OK) err = write_register(0x0e, (control & ~0xc3) | 0x04);
    if (err == ESP_OK) err = i2c_master_transmit(rtc, data, sizeof(data), 100);
    if (err == ESP_OK) err = read_registers(0x0f, &status, 1);
    if (err == ESP_OK) err = write_register(0x0f, status & ~0x80);
    if (err == ESP_OK) ESP_LOGI(TAG, "RTC synchronized from NTP");
    return err;
}

static void clock_worker(void *arg)
{
    (void)arg;
    for (;;) {
        const bool ntp = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RTC_SYNC_INTERVAL_MS)) != 0;
        const esp_err_t err = ntp ? sync_to_rtc() : sync_from_rtc();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Cannot synchronize %s: %s", ntp ? "RTC from NTP" : "device from RTC",
                     esp_err_to_name(err));
        }
    }
}

void rtc_clock_ntp_synced(int64_t utc)
{
    if (utc >= RTC_VALID_TIME_MIN && worker != NULL) xTaskNotifyGive(worker);
}

bool rtc_clock_get_utc(int64_t *utc)
{
    const int64_t now = (int64_t)time(NULL);
    *utc = now >= RTC_VALID_TIME_MIN ? now : 0;
    return enabled || *utc != 0;
}

esp_err_t rtc_clock_start(void)
{
    const ebc_config_t *cfg = ebc_config_get();
    enabled = ebc_config_is_active() && cfg->rtc.enabled;
    if (!enabled) return ESP_OK;
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = -1,
        .sda_io_num = cfg->rtc.sda_gpio,
        .scl_io_num = cfg->rtc.scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) return err;
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x68,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &device, &rtc);
    if (err == ESP_OK && cfg->rtc.int_gpio >= 0) {
        const gpio_config_t input = {
            .pin_bit_mask = UINT64_C(1) << cfg->rtc.int_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&input);
    }
    if (err == ESP_OK) {
        const esp_err_t read_err = sync_from_rtc();
        if (read_err != ESP_OK) ESP_LOGW(TAG, "Startup RTC read failed: %s", esp_err_to_name(read_err));
        if (xTaskCreate(clock_worker, "rtc_clock", 3072, NULL, 2, &worker) != pdPASS) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) {
        if (rtc != NULL) {
            const esp_err_t cleanup = i2c_master_bus_rm_device(rtc);
            if (cleanup != ESP_OK) ESP_LOGE(TAG, "Cannot remove RTC device: %s", esp_err_to_name(cleanup));
            rtc = NULL;
        }
        const esp_err_t cleanup = i2c_del_master_bus(bus);
        if (cleanup != ESP_OK) ESP_LOGE(TAG, "Cannot remove RTC bus: %s", esp_err_to_name(cleanup));
        bus = NULL;
    }
    return err;
}
