/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ebc_config.h"

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERR_NVS_NOT_FOUND 10
#define ESP_ERR_NVS_INVALID_LENGTH 11
#define ESP_ERR_NVS_TYPE_MISMATCH 12
#define ESP_ERR_NVS_NO_FREE_PAGES 13
#define ESP_ERR_NVS_NEW_VERSION_FOUND 14
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define GPIO_NUM_47 47
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_DISABLE 0
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define GPIO_PULLUP_ONLY 1
#define GPIO_PULLDOWN_ONLY 2
#define GPIO_FLOATING 0
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0 && (pin) <= 48 && ((pin) <= 21 || (pin) >= 26))
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) (GPIO_IS_VALID_GPIO(pin) && (pin) != 46)

typedef int nvs_handle_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
static ebc_config_blob_t stored_blob;
static size_t stored_size = sizeof(stored_blob);
static bool has_blob, has_mode, has_active, fail_commit, fail_open, fail_gpio;
static uint8_t stored_mode, stored_active;
static int up_level, down_level, current_pull, init_result;
static unsigned gpio_calls, reads, delays, erases, restarts, deinitializations;
static char write_order[32];
static unsigned allocations, open_handles, allocation_calls, fail_allocation_at;
static void *allocated[4];
static size_t allocated_size[4];

static void *mock_malloc(size_t size)
{
    if (++allocation_calls == fail_allocation_at) return NULL;
    void *result = malloc(size);
    assert(result != NULL && allocations < 4);
    allocated[allocations] = result;
    allocated_size[allocations++] = size;
    return result;
}
static void *mock_calloc(size_t count, size_t size)
{
    void *result = mock_malloc(count * size);
    if (result != NULL) memset(result, 0, count * size);
    return result;
}
static void mock_free(void *pointer)
{
    unsigned index = 0;
    while (index < allocations && allocated[index] != pointer) ++index;
    assert(index < allocations);
    for (size_t i = 0; i < allocated_size[index]; ++i) {
        assert(((const unsigned char *)pointer)[i] == 0);
    }
    free(pointer);
    --allocations;
    allocated[index] = allocated[allocations];
    allocated_size[index] = allocated_size[allocations];
}

static size_t strlcpy(char *out, const char *in, size_t capacity)
{
    const size_t length = strlen(in);
    if (capacity) {
        size_t count = length < capacity ? length : capacity - 1;
        memcpy(out, in, count);
        out[count] = '\0';
    }
    return length;
}
static uint32_t esp_crc32_le(uint32_t crc, const uint8_t *data, size_t size)
{
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock"; }
static esp_err_t nvs_flash_init(void) { int err = init_result; init_result = ESP_OK; return err; }
static esp_err_t nvs_flash_erase(void) { erases++; has_blob = has_mode = has_active = false; return ESP_OK; }
static esp_err_t nvs_flash_deinit(void) { deinitializations++; return ESP_OK; }
static void esp_restart(void) { restarts++; }
static void bootloader_random_enable(void) {}
static void bootloader_random_disable(void) {}
static uint32_t esp_random(void) { return 0x12345678; }
static esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    (void)mode;
    assert(strcmp(name, "ebc_cfg") == 0);
    *handle = 1;
    if (!fail_open) ++open_handles;
    return fail_open ? ESP_ERR_NO_MEM : ESP_OK;
}
static void nvs_close(nvs_handle_t handle) { (void)handle; assert(open_handles > 0); --open_handles; }
static esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size)
{
    (void)h;
    assert(strcmp(key, "config") == 0);
    if (!has_blob) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size >= stored_size);
    memcpy(out, &stored_blob, stored_size);
    *size = stored_size;
    return ESP_OK;
}
static esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    (void)h;
    bool mode = strcmp(key, "FirmwareMode") == 0;
    if (mode ? !has_mode : !has_active) return ESP_ERR_NVS_NOT_FOUND;
    *out = mode ? stored_mode : stored_active;
    return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t size)
{
    (void)h;
    assert(strcmp(key, "config") == 0 && size == sizeof(stored_blob));
    strcat(write_order, "B");
    stored_blob = *(const ebc_config_blob_t *)value;
    stored_size = size;
    has_blob = true;
    return ESP_OK;
}
static esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{
    (void)h;
    if (strcmp(key, "FirmwareMode") == 0) {
        strcat(write_order, "M"); stored_mode = value; has_mode = true;
    } else {
        assert(strcmp(key, "ConfigAktive") == 0);
        strcat(write_order, "A"); stored_active = value; has_active = true;
    }
    return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    strcat(write_order, "C");
    return fail_commit ? ESP_ERR_NO_MEM : ESP_OK;
}
static esp_err_t nvs_erase_all(nvs_handle_t h)
{
    (void)h;
    has_blob = has_mode = has_active = false;
    return ESP_OK;
}
static esp_err_t gpio_config(const gpio_config_t *cfg)
{
    assert(cfg->pin_bit_mask == (UINT64_C(1) << 47) && cfg->mode == GPIO_MODE_INPUT);
    gpio_calls++;
    return fail_gpio ? ESP_ERR_INVALID_STATE : ESP_OK;
}
static esp_err_t gpio_set_pull_mode(int pin, int mode)
{
    assert(pin == 47);
    gpio_calls++;
    current_pull = mode;
    return ESP_OK;
}
static esp_err_t gpio_set_direction(int pin, int mode)
{
    assert(pin == 47 && mode == GPIO_MODE_DISABLE && current_pull == GPIO_FLOATING);
    gpio_calls++;
    return ESP_OK;
}
static int gpio_get_level(int pin)
{
    assert(pin == 47);
    reads++;
    return current_pull == GPIO_PULLUP_ONLY ? up_level : down_level;
}
static void esp_rom_delay_us(unsigned us) { assert(us == 200); delays++; }

#define malloc mock_malloc
#define calloc mock_calloc
#define free mock_free
#include "../components/ebc_config/ebc_config.c"
#undef malloc
#undef calloc
#undef free

static void reboot(void)
{
    initialized = false;
    assert(allocations == 0 && open_handles == 0);
    allocation_calls = fail_allocation_at = 0;
    gpio_calls = reads = delays = 0;
    write_order[0] = '\0';
}
static void fresh(int up, int down)
{
    reboot();
    has_blob = has_mode = has_active = false;
    up_level = up;
    down_level = down;
    fail_commit = fail_open = fail_gpio = false;
}
int main(void)
{
    const int levels[][2] = {{1, 0}, {0, 0}, {1, 1}, {0, 1}};
    for (unsigned i = 0; i < 4; ++i) {
        fresh(levels[i][0], levels[i][1]);
        assert(ebc_config_init() == ESP_OK);
        assert(ebc_config_get_mode() == (i == 2 ? EBC_FW_FACTORY : EBC_FW_MAKER));
        assert(ebc_config_is_active() == (i == 2));
        const ebc_config_t *cfg = ebc_config_get();
#ifdef CONFIG_SD_CARD_MANAGER_BUS_SPI
        assert(cfg->sd.bus == SD_BUS_SPI);
#else
        assert(cfg->sd.bus == SD_BUS_SDMMC_4BIT);
#endif
        assert(cfg->sd.sdmmc_freq_khz == 12000 && cfg->sd.spi_freq_khz == 8000);
        assert(cfg->sd.card_detect_gpio == 13 && strcmp(cfg->sd.mount_point, "/sdcard") == 0);
        assert(cfg->sd.clk_gpio == 10 && cfg->sd.cmd_gpio == 11);
        assert(cfg->sd.d0_gpio == 12 && cfg->sd.d1_gpio == 4);
        assert(cfg->sd.d2_gpio == 5 && cfg->sd.d3_gpio == 6);
        assert(cfg->sd.spi_miso_gpio == 2 && cfg->sd.spi_mosi_gpio == 7);
        assert(cfg->sd.spi_cs_gpio == 3 && cfg->sd.spi_clk_gpio == 10);
        assert(cfg->rtc.enabled == (i == 2) && cfg->rtc.int_gpio == 14 &&
               cfg->rtc.sda_gpio == 15 && cfg->rtc.scl_gpio == 16);
        assert(reads == 6 && delays == 6 && current_pull == GPIO_FLOATING);
        assert(strcmp(write_order, "BMAC") == 0);
        assert(stored_blob.magic == EBC_CONFIG_MAGIC);
        assert(stored_blob.length == sizeof(ebc_config_t));
        reboot();
        assert(ebc_config_init() == ESP_OK && gpio_calls == 0 && reads == 0);
    }
    has_active = false;
    reboot();
    assert(ebc_config_init() == ESP_OK && !ebc_config_is_active() && gpio_calls == 0);
    has_active = true;
    stored_active = 255;
    reboot();
    assert(ebc_config_init() == ESP_OK && !ebc_config_is_active() && gpio_calls == 0);
    stored_blob.crc ^= 1;
    reboot();
    assert(ebc_config_init() == ESP_OK && reads == 6);
    stored_blob.version++;
    reboot();
    assert(ebc_config_init() == ESP_OK && reads == 6);
    stored_blob.magic = 0;
    reboot();
    assert(ebc_config_init() == ESP_OK && reads == 6);
    has_mode = false;
    reboot();
    assert(ebc_config_init() == ESP_OK && reads == 6);
    ebc_config_t next = *ebc_config_get();
    next.rtc.enabled = 1;
    next.rtc.sda_gpio = next.sd.bus == SD_BUS_SPI ? next.sd.spi_mosi_gpio : next.sd.cmd_gpio;
    assert(ebc_config_validate(&next, true) != ESP_OK);
    next.rtc.sda_gpio = 1;
    next.rtc.scl_gpio = 0;
    next.rtc.int_gpio = 21;
    assert(ebc_config_validate(&next, true) == ESP_OK);
    next.rtc.scl_gpio = next.rtc.sda_gpio;
    assert(ebc_config_validate(&next, true) != ESP_OK);
    next.rtc.scl_gpio = 46;
    assert(ebc_config_validate(&next, true) != ESP_OK);
    next.rtc.scl_gpio = 0;
    next.rtc.int_gpio = -1;
    assert(ebc_config_validate(&next, true) == ESP_OK);
    next.rtc.sda_gpio = -1;
    assert(ebc_config_validate(&next, true) != ESP_OK);
    next.rtc.enabled = 2;
    assert(ebc_config_validate(&next, false) != ESP_OK);
    next = *ebc_config_get();
    next.tester.tester_uart.tx_gpio = 22;
    assert(ebc_config_validate(&next, false) != ESP_OK);
    fresh(1, 1);
    assert(ebc_config_init() == ESP_OK);
    strcpy(stored_blob.payload.tester.device_id, "preserve-on-upgrade");
    stored_blob.version = 1;
    stored_blob.length = (uint16_t)offsetof(ebc_config_t, rtc);
    stored_blob.crc = esp_crc32_le(0, (const uint8_t *)&stored_blob.payload, stored_blob.length);
    stored_size = offsetof(ebc_config_blob_t, payload) + stored_blob.length;
    reboot();
    assert(ebc_config_init() == ESP_OK && reads == 0 && gpio_calls == 0);
    assert(ebc_config_is_active() && ebc_config_get_mode() == EBC_FW_FACTORY);
    assert(strcmp(ebc_config_get()->tester.device_id, "preserve-on-upgrade") == 0);
    assert(ebc_config_get()->rtc.enabled && ebc_config_get()->rtc.sda_gpio == 15);
    assert(stored_blob.version == EBC_CONFIG_VERSION && strcmp(write_order, "BMAC") == 0);
    stored_blob.payload.rtc.enabled = 0;
    stored_blob.payload.rtc.int_gpio = 1;
    stored_blob.payload.rtc.sda_gpio = 2;
    stored_blob.payload.rtc.scl_gpio = 3;
    stored_blob.crc = esp_crc32_le(0, (const uint8_t *)&stored_blob.payload, stored_blob.length);
    reboot();
    assert(ebc_config_init() == ESP_OK && reads == 0 && ebc_config_is_active());
    assert(ebc_config_get()->rtc.enabled && stored_blob.payload.rtc.enabled);
    assert(ebc_config_get()->rtc.int_gpio == 14 && ebc_config_get()->rtc.sda_gpio == 15 &&
           ebc_config_get()->rtc.scl_gpio == 16);
    assert(stored_blob.payload.rtc.int_gpio == 14 && stored_blob.payload.rtc.sda_gpio == 15 &&
           stored_blob.payload.rtc.scl_gpio == 16);
    assert(strcmp(write_order, "BMAC") == 0);
    next = *ebc_config_get();
    next.rtc.enabled = 0;
    next.rtc.int_gpio = 9;
    next.rtc.sda_gpio = 10;
    next.rtc.scl_gpio = 11;
    assert(ebc_config_save(&next) == ESP_OK && ebc_config_get()->rtc.enabled);
    assert(stored_blob.payload.rtc.enabled &&
           stored_blob.payload.rtc.int_gpio == 14 &&
           stored_blob.payload.rtc.sda_gpio == 15 &&
           stored_blob.payload.rtc.scl_gpio == 16);
    next.rtc.sda_gpio = next.sd.bus == SD_BUS_SPI ? next.sd.spi_mosi_gpio : next.sd.cmd_gpio;
    assert(ebc_config_save(&next) == ESP_OK);
    assert(ebc_config_get()->rtc.enabled && ebc_config_is_active() &&
           ebc_config_get()->rtc.sda_gpio == 15 &&
           ebc_config_get()->rtc.scl_gpio == 16 &&
           ebc_config_get()->rtc.int_gpio == 14);
    fresh(1, 0);
    assert(ebc_config_init() == ESP_OK && !ebc_config_get()->rtc.enabled);
    next = *ebc_config_get();
    next.rtc.enabled = 1;
    assert(ebc_config_save_with_active(&next, true) == ESP_OK);
    reboot();
    assert(ebc_config_init() == ESP_OK && ebc_config_is_active() && ebc_config_get()->rtc.enabled);
    next = *ebc_config_get();
    next.rtc.enabled = 0;
    assert(ebc_config_save(&next) == ESP_OK && !ebc_config_get()->rtc.enabled);
    reboot();
    assert(ebc_config_init() == ESP_OK && !ebc_config_get()->rtc.enabled && ebc_config_is_active());
    stored_blob.version = 1;
    stored_blob.length = (uint16_t)offsetof(ebc_config_t, rtc);
    stored_blob.crc = esp_crc32_le(0, (const uint8_t *)&stored_blob.payload, stored_blob.length);
    stored_size = offsetof(ebc_config_blob_t, payload) + stored_blob.length;
    reboot();
    assert(ebc_config_init() == ESP_OK && reads == 0 && ebc_config_is_active());
    assert(ebc_config_get_mode() == EBC_FW_MAKER && !ebc_config_get()->rtc.enabled);
    const ebc_config_t before_failure = *ebc_config_get();
    next = before_failure;
    next.rtc.enabled = 1;
    for (unsigned attempt = 1; attempt <= 2; ++attempt) {
        allocation_calls = 0;
        fail_allocation_at = attempt;
        assert(ebc_config_save(&next) == ESP_ERR_NO_MEM);
        assert(memcmp(ebc_config_get(), &before_failure, sizeof(before_failure)) == 0);
        assert(allocations == 0 && open_handles == 0);
    }
    fail_allocation_at = 0;
    fail_commit = true;
    assert(ebc_config_save(&next) == ESP_ERR_NO_MEM);
    assert(memcmp(ebc_config_get(), &before_failure, sizeof(before_failure)) == 0);
    assert(allocations == 0 && open_handles == 0);
    fail_commit = false;
    fail_open = true;
    assert(persist(&before_failure, true) == ESP_ERR_NO_MEM);
    assert(allocations == 0 && open_handles == 0);
    fail_open = false;
    for (unsigned attempt = 1; attempt <= 2; ++attempt) {
        stored_blob.version = 1;
        stored_blob.length = (uint16_t)offsetof(ebc_config_t, rtc);
        stored_blob.crc = esp_crc32_le(0, (const uint8_t *)&stored_blob.payload, stored_blob.length);
        stored_size = offsetof(ebc_config_blob_t, payload) + stored_blob.length;
        reboot();
        fail_allocation_at = attempt;
        assert(ebc_config_init() == ESP_ERR_NO_MEM && !initialized);
        assert(allocations == 0 && open_handles == 0 && erases == 0);
    }
    reboot();
    assert(ebc_config_init() == ESP_OK && ebc_config_is_active());
    next = *ebc_config_get();
    memset(next.ota.web_password, 'a', sizeof(next.ota.web_password));
    assert(ebc_config_validate(&next, false) != ESP_OK);
    fresh(1, 1);
    fail_commit = true;
    assert(ebc_config_init() == ESP_ERR_NO_MEM && !initialized);
    fresh(1, 0);
    fail_open = true;
    assert(ebc_config_init() == ESP_ERR_NO_MEM && gpio_calls == 0);
    fresh(1, 0);
    init_result = ESP_ERR_NVS_NO_FREE_PAGES;
    assert(ebc_config_init() == ESP_OK && erases == 1);
    fresh(1, 0);
    fail_gpio = true;
    assert(ebc_config_init() == ESP_OK && !ebc_config_is_active());
    assert(current_pull == GPIO_FLOATING);
    assert(ebc_config_factory_reset() == ESP_OK && restarts == 1 && !has_blob);
    assert(ebc_config_full_reset() == ESP_OK && restarts == 2 && deinitializations == 1);
    puts("NVS configuration and IO47 tests passed.");
    return 0;
}
