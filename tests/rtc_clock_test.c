/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "ds3231_time.h"
#include "ebc_config.h"

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 0
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef void *TaskHandle_t;
typedef struct {
    int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
    struct { bool enable_internal_pullup; } flags;
} i2c_master_bus_config_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;

static ebc_config_t cfg;
static bool active = true, missing, fail_task, fail_bus;
static uint8_t registers[16];
static time_t now;
static unsigned reads, writes, set_times, notifications, bus_creations, gpio_calls, cleanup_calls;
static unsigned worker_waits;
static jmp_buf worker_exit;

const ebc_config_t *ebc_config_get(void) { return &cfg; }
bool ebc_config_is_active(void) { return active; }
static const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock"; }
static time_t mock_time(time_t *out) { if (out) *out = now; return now; }
static int mock_settimeofday(const struct timeval *tv, void *zone)
{
    assert(zone == NULL && tv->tv_usec == 0);
    now = tv->tv_sec;
    ++set_times;
    return 0;
}
static esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config, void **bus)
{
    assert(config->sda_io_num == 15 && config->scl_io_num == 16);
    assert(config->flags.enable_internal_pullup);
    ++bus_creations;
    *bus = (void *)1;
    return fail_bus ? ESP_FAIL : ESP_OK;
}
static esp_err_t i2c_master_bus_add_device(void *bus, const i2c_device_config_t *device, void **rtc)
{
    assert(bus != NULL && device->device_address == 0x68 && device->scl_speed_hz == 100000);
    *rtc = (void *)2;
    return ESP_OK;
}
static esp_err_t gpio_config(const gpio_config_t *input)
{
    assert(input->pin_bit_mask == (UINT64_C(1) << 14) && input->mode == GPIO_MODE_INPUT);
    ++gpio_calls;
    return ESP_OK;
}
static esp_err_t i2c_master_transmit_receive(void *rtc, const uint8_t *address, size_t size,
                                          uint8_t *out, size_t length, int timeout)
{
    assert(rtc && size == 1 && timeout == 100 && *address + length <= sizeof(registers));
    ++reads;
    if (missing) return ESP_FAIL;
    memcpy(out, registers + *address, length);
    return ESP_OK;
}
static esp_err_t i2c_master_transmit(void *rtc, const uint8_t *data, size_t size, int timeout)
{
    assert(rtc && timeout == 100 && size >= 2 && data[0] + size - 1 <= sizeof(registers));
    ++writes;
    if (missing) return ESP_FAIL;
    memcpy(registers + data[0], data + 1, size - 1);
    return ESP_OK;
}
static esp_err_t i2c_master_bus_rm_device(void *rtc) { assert(rtc); ++cleanup_calls; return ESP_OK; }
static esp_err_t i2c_del_master_bus(void *bus) { assert(bus); ++cleanup_calls; return ESP_OK; }
static void xTaskNotifyGive(void *task) { assert(task); ++notifications; }
static uint32_t ulTaskNotifyTake(int clear, uint32_t wait)
{
    if (wait != 0) {
        assert(wait == 21600000 && clear == pdTRUE);
        if (worker_waits++) longjmp(worker_exit, 1);
    }
    const unsigned pending = notifications;
    notifications = clear ? 0 : (notifications ? notifications - 1 : 0);
    return pending;
}
static int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, void **handle)
{
    (void)task;
    assert(strcmp(name, "rtc_clock") == 0 && stack >= 2048 && arg == NULL && priority == 2);
    if (fail_task) return 0;
    *handle = (void *)3;
    return pdPASS;
}
#define time mock_time
#define settimeofday mock_settimeofday
#include "../main/rtc_clock.c"

static void run_worker_once(void)
{
    worker_waits = 0;
    if (setjmp(worker_exit) == 0) clock_worker(NULL);
}
static void reset(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.rtc.enabled = 1;
    cfg.rtc.int_gpio = 14; cfg.rtc.sda_gpio = 15; cfg.rtc.scl_gpio = 16;
    active = true; missing = fail_task = fail_bus = false;
    enabled = false; worker = bus = rtc = NULL;
    reads = writes = set_times = notifications = bus_creations = gpio_calls = cleanup_calls = 0;
    memset(registers, 0, sizeof(registers));
    now = 0;
}
int main(void)
{
    uint8_t r[7];
    int64_t value;
    const int64_t timestamps[] = {1704067200, 1709251199, 1794142212, 4107542400, 7258118399};
    for (unsigned i = 0; i < sizeof(timestamps) / sizeof(timestamps[0]); ++i) {
        assert(ds3231_encode_time(timestamps[i], r));
        assert(ds3231_decode_time(r, &value) && value == timestamps[i]);
    }
    assert(!ds3231_encode_time(1704067199, r) && !ds3231_encode_time(7258118400, r));
    uint8_t noon[] = {0, 0, 0x72, 1, 1, 1, 0x26};
    assert(ds3231_decode_time(noon, &value));
    const int64_t noon_time = value;
    noon[2] = 0x52;
    assert(ds3231_decode_time(noon, &value) && value == noon_time - 43200);
    noon[2] = 0x71;
    assert(ds3231_decode_time(noon, &value) && value == noon_time + 39600);
    noon[2] = 0x24;
    assert(!ds3231_decode_time(noon, &value));
    noon[2] = 0; noon[4] = 0x29; noon[5] = 0x82; noon[6] = 0;
    assert(!ds3231_decode_time(noon, &value)); /* 2100 is not a leap year. */
    noon[5] = 2; noon[6] = 0x24;
    assert(ds3231_decode_time(noon, &value));
    noon[0] = 0x1a;
    assert(!ds3231_decode_time(noon, &value));

    reset(); cfg.rtc.enabled = 0;
    assert(rtc_clock_start() == ESP_OK && bus_creations == 0 && reads == 0);
    assert(!rtc_clock_get_utc(&value) && value == 0);
    now = 1794142212;
    assert(rtc_clock_get_utc(&value) && value == now); /* NTP-only display. */
    reset(); active = false;
    assert(rtc_clock_start() == ESP_OK && bus_creations == 0);
    reset();
    assert(ds3231_encode_time(1794142212, registers));
    registers[14] = 0xc3;
    assert(rtc_clock_start() == ESP_OK && now == 1794142212 && set_times == 1 && gpio_calls == 1);
    assert(registers[14] == 0x04); /* Battery oscillator enabled even without NTP. */
    assert(ds3231_encode_time(1794163812, registers));
    run_worker_once();
    assert(now == 1794163812 && set_times == 2); /* Six-hour offline fallback. */
    now = 1794185412;
    registers[14] = 0xff; registers[15] = 0x8b;
    rtc_clock_ntp_synced(now);
    run_worker_once();
    assert(set_times == 2 && ds3231_decode_time(registers, &value) && value == now);
    assert(registers[14] == 0x3c && registers[15] == 0x0b); /* EOSC/alarms off; OSF cleared only after NTP. */
    rtc_clock_ntp_synced(0);
    assert(notifications == 0);
    reset(); registers[15] = 0x80;
    assert(rtc_clock_start() == ESP_OK && now == 0 && set_times == 0);
    assert(registers[15] == 0x80); /* An untrusted RTC is never marked valid by a read. */
    assert(rtc_clock_get_utc(&value) && value == 0);
    reset(); missing = true;
    assert(rtc_clock_start() == ESP_OK && worker != NULL && now == 0);
    now = 1794142212; rtc_clock_ntp_synced(now); run_worker_once();
    assert(now == 1794142212 && set_times == 0); /* Missing hardware cannot block NTP. */
    reset(); fail_task = true;
    assert(rtc_clock_start() == ESP_ERR_NO_MEM && cleanup_calls == 2);
    reset(); fail_bus = true;
    assert(rtc_clock_start() == ESP_FAIL && worker == NULL);
    puts("RTC codec, startup, six-hour synchronization and failure tests passed");
    return 0;
}
