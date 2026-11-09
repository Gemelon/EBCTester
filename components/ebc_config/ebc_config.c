/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ebc_config.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "bootloader_random.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "http_parser.h"
#include "sdkconfig.h"

static const char *TAG = "ebc_config";
static const char *NAMESPACE = "ebc_cfg";
static ebc_config_t config;
static bool initialized;
static bool config_active;
static ebc_fw_mode_t firmware_mode;
_Static_assert(sizeof(ebc_config_t) <= UINT16_MAX, "Configuration payload exceeds blob length field");

static void copy_default(char *destination, size_t capacity, const char *source, const char *name)
{
    if (strlcpy(destination, source, capacity) >= capacity) {
        ESP_LOGW(TAG, "Default %s exceeds %u bytes and was truncated", name, (unsigned)capacity - 1);
    }
}

static void load_defaults(ebc_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->ota.session_days = 1;
    cfg->ota.login_enabled = 1;
    cfg->rtc.int_gpio = CONFIG_EBCTESTER_RTC_INT_GPIO;
    cfg->rtc.sda_gpio = CONFIG_EBCTESTER_I2C_SDA_GPIO;
    cfg->rtc.scl_gpio = CONFIG_EBCTESTER_I2C_SCL_GPIO;
    cfg->tester.tester_uart = (ebc_uart_config_t){
        CONFIG_EBC_TESTER_UART_TX_GPIO, CONFIG_EBC_TESTER_UART_RX_GPIO, CONFIG_EBC_TESTER_UART_BAUD};
    cfg->tester.dongle_uart = (ebc_uart_config_t){
        CONFIG_EBC_DONGLE_UART_TX_GPIO, CONFIG_EBC_DONGLE_UART_RX_GPIO, CONFIG_EBC_DONGLE_UART_BAUD};
    cfg->tester.averaging_length = CONFIG_TEST_CONTROL_AVERAGING_LENGTH;
    copy_default(cfg->tester.device_id, sizeof(cfg->tester.device_id), CONFIG_TEST_STORE_DEVICE_ID, "device ID");
#ifdef CONFIG_EBCTESTER_HA_MQTT_ENABLE
    cfg->tester.mqtt_enabled = 1;
#endif
    copy_default(cfg->ota.ap_password, sizeof(cfg->ota.ap_password), CONFIG_OTA_SUITE_AP_PASSWORD, "AP password");
    copy_default(cfg->ota.sntp_server, sizeof(cfg->ota.sntp_server), CONFIG_OTA_SUITE_SNTP_SERVER, "SNTP server");
    copy_default(cfg->ota.manifest_url, sizeof(cfg->ota.manifest_url), CONFIG_OTA_SUITE_UPDATE_MANIFEST_URL, "manifest URL");
    copy_default(cfg->ota.web_password, sizeof(cfg->ota.web_password), CONFIG_OTA_SUITE_WEB_PASSWORD, "web password");
    copy_default(cfg->ota.primary_tabs, sizeof(cfg->ota.primary_tabs), CONFIG_OTA_SUITE_WEB_PRIMARY_TABS, "primary tabs");
    copy_default(cfg->ota.tab_order, sizeof(cfg->ota.tab_order), CONFIG_OTA_SUITE_WEB_TAB_ORDER, "tab order");
#ifdef CONFIG_OTA_SUITE_OTA_ALLOW_HTTP
    cfg->ota.allow_http = 1;
#endif
#ifdef CONFIG_OTA_SUITE_STATUS_LED_ENABLE
    cfg->ota.status_led_enabled = 1;
#endif
#ifdef CONFIG_OTA_SUITE_WEB_SETTINGS_REQUIRE_PASSWORD
    cfg->ota.settings_require_password = 1;
#endif
#ifdef CONFIG_OTA_SUITE_WEB_SD_LANGUAGES
    cfg->ota.sd_languages = 1;
#endif
    cfg->ota.led_gpio = -1;
#ifdef CONFIG_OTA_SUITE_LED_GPIO
    cfg->ota.led_gpio = CONFIG_OTA_SUITE_LED_GPIO;
#endif
    cfg->sd.bus = SD_BUS_SDMMC_4BIT;
#ifdef CONFIG_SD_CARD_MANAGER_BUS_SPI
    cfg->sd.bus = SD_BUS_SPI;
#endif
    cfg->sd.sdmmc_freq_khz = 10000;
    cfg->sd.spi_freq_khz = 10000;
#ifdef CONFIG_SD_CARD_MANAGER_SDMMC_FREQ_KHZ
    cfg->sd.sdmmc_freq_khz = CONFIG_SD_CARD_MANAGER_SDMMC_FREQ_KHZ;
#endif
#ifdef CONFIG_SD_CARD_MANAGER_SPI_FREQ_KHZ
    cfg->sd.spi_freq_khz = CONFIG_SD_CARD_MANAGER_SPI_FREQ_KHZ;
#endif
    cfg->sd.card_detect_gpio = -1;
#ifdef CONFIG_SD_CARD_MANAGER_GPIO_CARD_DETECT
    cfg->sd.card_detect_gpio = CONFIG_SD_CARD_MANAGER_GPIO_CARD_DETECT;
#endif
    copy_default(cfg->sd.mount_point, sizeof(cfg->sd.mount_point), CONFIG_SD_CARD_MANAGER_MOUNT_POINT, "mount point");
    cfg->sd.clk_gpio = cfg->sd.cmd_gpio = -1;
    cfg->sd.d0_gpio = cfg->sd.d1_gpio = cfg->sd.d2_gpio = cfg->sd.d3_gpio = -1;
    cfg->sd.spi_miso_gpio = cfg->sd.spi_mosi_gpio = cfg->sd.spi_cs_gpio = cfg->sd.spi_clk_gpio = -1;
#ifdef CONFIG_SD_CARD_MANAGER_GPIO_CLK
    cfg->sd.clk_gpio = CONFIG_SD_CARD_MANAGER_GPIO_CLK;
#endif
#ifdef CONFIG_SD_CARD_MANAGER_GPIO_CMD
    cfg->sd.cmd_gpio = CONFIG_SD_CARD_MANAGER_GPIO_CMD;
#endif
#ifdef CONFIG_SD_CARD_MANAGER_GPIO_D0
    cfg->sd.d0_gpio = CONFIG_SD_CARD_MANAGER_GPIO_D0;
#endif
#ifdef CONFIG_SD_CARD_MANAGER_GPIO_D1
    cfg->sd.d1_gpio = CONFIG_SD_CARD_MANAGER_GPIO_D1;
#endif
#ifdef CONFIG_SD_CARD_MANAGER_GPIO_D2
    cfg->sd.d2_gpio = CONFIG_SD_CARD_MANAGER_GPIO_D2;
#endif
#ifdef CONFIG_SD_CARD_MANAGER_GPIO_D3
    cfg->sd.d3_gpio = CONFIG_SD_CARD_MANAGER_GPIO_D3;
#endif
#ifdef CONFIG_SD_CARD_MANAGER_GPIO_SPI_MISO
    cfg->sd.spi_miso_gpio = CONFIG_SD_CARD_MANAGER_GPIO_SPI_MISO;
#endif
#ifdef CONFIG_SD_CARD_MANAGER_GPIO_SPI_MOSI
    cfg->sd.spi_mosi_gpio = CONFIG_SD_CARD_MANAGER_GPIO_SPI_MOSI;
#endif
#ifdef CONFIG_SD_CARD_MANAGER_GPIO_SPI_CS
    cfg->sd.spi_cs_gpio = CONFIG_SD_CARD_MANAGER_GPIO_SPI_CS;
#endif
#ifdef CONFIG_SD_CARD_MANAGER_GPIO_SPI_CLK
    cfg->sd.spi_clk_gpio = CONFIG_SD_CARD_MANAGER_GPIO_SPI_CLK;
#endif
}

static bool valid_string(const char *text, size_t capacity)
{
    const char *end = memchr(text, '\0', capacity);
    if (end == NULL) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < 32 || *p == 127) return false;
    }
    return true;
}

static bool valid_tabs(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == ',')) return false;
    }
    return true;
}

static bool valid_manifest_url(const char *url, bool allow_http)
{
    if (*url == '\0') return true;
    if (strncmp(url, "https://", 8) != 0 && !(allow_http && strncmp(url, "http://", 7) == 0)) return false;
    struct http_parser_url parsed;
    if (http_parser_parse_url(url, strlen(url), 0, &parsed) != 0 ||
        !(parsed.field_set & (1 << UF_HOST)) || parsed.field_data[UF_HOST].len == 0 ||
        parsed.field_set & ((1 << UF_USERINFO) | (1 << UF_FRAGMENT))) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p) {
        if (*p <= 32 || *p == 127 || *p == '@' || *p == '\\') return false;
    }
    return true;
}

esp_err_t ebc_config_validate(const ebc_config_t *cfg, bool active)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
#define CHECK_STRING(field) \
    if (!valid_string(cfg->field, sizeof(cfg->field))) { \
        ESP_LOGW(TAG, "Invalid configuration string: " #field); return ESP_ERR_INVALID_ARG; \
    }
    CHECK_STRING(tester.device_id);
    CHECK_STRING(ota.ap_password);
    CHECK_STRING(ota.sntp_server);
    CHECK_STRING(ota.manifest_url);
    CHECK_STRING(ota.web_password);
    CHECK_STRING(ota.primary_tabs);
    CHECK_STRING(ota.tab_order);
    CHECK_STRING(sd.mount_point);
#undef CHECK_STRING
    const int8_t pins[] = {
        cfg->tester.tester_uart.tx_gpio, cfg->tester.tester_uart.rx_gpio,
        cfg->tester.dongle_uart.tx_gpio, cfg->tester.dongle_uart.rx_gpio,
        cfg->ota.led_gpio, cfg->sd.card_detect_gpio, cfg->sd.clk_gpio, cfg->sd.cmd_gpio,
        cfg->sd.d0_gpio, cfg->sd.d1_gpio, cfg->sd.d2_gpio, cfg->sd.d3_gpio,
        cfg->sd.spi_miso_gpio, cfg->sd.spi_mosi_gpio, cfg->sd.spi_cs_gpio, cfg->sd.spi_clk_gpio,
        cfg->rtc.int_gpio, cfg->rtc.sda_gpio, cfg->rtc.scl_gpio,
    };
    for (size_t i = 0; i < sizeof(pins); ++i) {
        if (pins[i] != -1 && !GPIO_IS_VALID_GPIO(pins[i])) {
            ESP_LOGW(TAG, "Invalid GPIO: %d", pins[i]);
            return ESP_ERR_INVALID_ARG;
        }
    }
    size_t ap_length = strlen(cfg->ota.ap_password);
    size_t web_length = strlen(cfg->ota.web_password);
    if ((ap_length != 0 && ap_length < 8) || web_length < 8 ||
        cfg->ota.web_password[0] == ' ' || cfg->ota.web_password[web_length - 1] == ' ' ||
        cfg->tester.averaging_length < 1 || cfg->tester.averaging_length > 120 ||
        cfg->tester.tester_uart.baud < 1200 || cfg->tester.tester_uart.baud > 115200 ||
        cfg->tester.dongle_uart.baud < 1200 || cfg->tester.dongle_uart.baud > 115200 ||
        !cfg->tester.device_id[0] || !cfg->ota.sntp_server[0] ||
        cfg->sd.mount_point[0] != '/' || !cfg->sd.mount_point[1] ||
        strchr(cfg->sd.mount_point + 1, '/') != NULL || strstr(cfg->sd.mount_point, "..") != NULL ||
        cfg->sd.sdmmc_freq_khz < 400 || cfg->sd.sdmmc_freq_khz > 20000 ||
        cfg->sd.spi_freq_khz < 400 || cfg->sd.spi_freq_khz > 20000 ||
        (cfg->sd.bus != SD_BUS_SDMMC_4BIT && cfg->sd.bus != SD_BUS_SPI) ||
        cfg->rtc.enabled > 1 || cfg->tester.mqtt_enabled > 1 || cfg->ota.allow_http > 1 ||
        cfg->ota.status_led_enabled > 1 || cfg->ota.settings_require_password > 1 ||
        cfg->ota.sd_languages > 1 || cfg->ota.login_enabled > 1 ||
        (cfg->ota.session_days != 1 && cfg->ota.session_days != 7) ||
        !valid_tabs(cfg->ota.primary_tabs) || !valid_tabs(cfg->ota.tab_order) ||
        !valid_manifest_url(cfg->ota.manifest_url, cfg->ota.allow_http)) {
        ESP_LOGW(TAG, "Configuration value outside allowed range");
        return ESP_ERR_INVALID_ARG;
    }
    for (const unsigned char *p = (const unsigned char *)cfg->ota.web_password; *p; ++p) {
        if (*p > 126) {
            ESP_LOGW(TAG, "Web password must contain printable ASCII characters");
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!active) return ESP_OK;
    const int8_t required[] = {
        cfg->tester.tester_uart.tx_gpio, cfg->tester.tester_uart.rx_gpio,
        cfg->tester.dongle_uart.tx_gpio, cfg->tester.dongle_uart.rx_gpio,
        cfg->sd.bus == SD_BUS_SPI ? cfg->sd.spi_miso_gpio : cfg->sd.clk_gpio,
        cfg->sd.bus == SD_BUS_SPI ? cfg->sd.spi_mosi_gpio : cfg->sd.cmd_gpio,
        cfg->sd.bus == SD_BUS_SPI ? cfg->sd.spi_cs_gpio : cfg->sd.d0_gpio,
        cfg->sd.bus == SD_BUS_SPI ? cfg->sd.spi_clk_gpio : cfg->sd.d1_gpio,
        cfg->sd.bus == SD_BUS_SPI ? -1 : cfg->sd.d2_gpio,
        cfg->sd.bus == SD_BUS_SPI ? -1 : cfg->sd.d3_gpio,
        cfg->ota.status_led_enabled ? cfg->ota.led_gpio : -1,
        cfg->sd.card_detect_gpio,
        cfg->rtc.enabled ? cfg->rtc.sda_gpio : -1,
        cfg->rtc.enabled ? cfg->rtc.scl_gpio : -1,
        cfg->rtc.enabled ? cfg->rtc.int_gpio : -1,
    };
    for (size_t i = 0; i < sizeof(required); ++i) {
        const int pin = required[i];
        const bool optional = (cfg->sd.bus == SD_BUS_SPI && (i == 8 || i == 9)) ||
                              (i == 10 && !cfg->ota.status_led_enabled) || i == 11 ||
                              (i >= 12 && !cfg->rtc.enabled) || i == 14;
        if (pin == -1 && optional) continue;
        if (!GPIO_IS_VALID_GPIO(pin) ||
            (i != 1 && i != 3 && i != 11 && i != 14 && !(i == 4 && cfg->sd.bus == SD_BUS_SPI) &&
             !GPIO_IS_VALID_OUTPUT_GPIO(pin)) ||
            (pin >= 26 && pin <= 37) || pin == 19 || pin == 20 || pin == 43 || pin == 44) {
            ESP_LOGW(TAG, "Required GPIO %d is unavailable or reserved on N16R8", pin);
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; ++j) {
            if (required[j] == pin) {
                ESP_LOGW(TAG, "GPIO %d assigned more than once", pin);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

static ebc_io47_state_t detect_io47(void)
{
    const gpio_config_t input = {
        .pin_bit_mask = UINT64_C(1) << GPIO_NUM_47,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&input);
    int up = 1, down = 0;
    for (unsigned i = 0; err == ESP_OK && i < 3; ++i) {
        err = gpio_set_pull_mode(GPIO_NUM_47, GPIO_PULLUP_ONLY);
        if (err == ESP_OK) {
            esp_rom_delay_us(200);
            up &= gpio_get_level(GPIO_NUM_47);
            err = gpio_set_pull_mode(GPIO_NUM_47, GPIO_PULLDOWN_ONLY);
        }
        if (err == ESP_OK) {
            esp_rom_delay_us(200);
            down |= gpio_get_level(GPIO_NUM_47);
        }
    }
    esp_err_t floating_err = gpio_set_pull_mode(GPIO_NUM_47, GPIO_FLOATING);
    esp_err_t disable_err = gpio_set_direction(GPIO_NUM_47, GPIO_MODE_DISABLE);
    if (err == ESP_OK) err = floating_err;
    if (err == ESP_OK) err = disable_err;
    if (err != ESP_OK) ESP_LOGW(TAG, "IO47 GPIO operation failed: %s", esp_err_to_name(err));
    return err != ESP_OK ? IO47_ERROR :
           up ? (down ? IO47_HIGH : IO47_OPEN) : (down ? IO47_ERROR : IO47_LOW);
}

static esp_err_t persist(const ebc_config_t *cfg, bool active)
{
    ebc_config_blob_t *blob = calloc(1, sizeof(*blob));
    if (blob == NULL) {
        ESP_LOGE(TAG, "Cannot allocate configuration for writing");
        return ESP_ERR_NO_MEM;
    }
    blob->magic = EBC_CONFIG_MAGIC;
    blob->version = EBC_CONFIG_VERSION;
    blob->length = sizeof(blob->payload);
    memcpy(&blob->payload, cfg, sizeof(*cfg));
    blob->crc = esp_crc32_le(0, (const uint8_t *)&blob->payload, sizeof(blob->payload));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open configuration for writing: %s", esp_err_to_name(err));
        memset(blob, 0, sizeof(*blob));
        free(blob);
        return err;
    }
    err = nvs_set_blob(handle, "config", blob, sizeof(*blob));
    if (err == ESP_OK) err = nvs_set_u8(handle, "FirmwareMode", firmware_mode);
    if (err == ESP_OK) err = nvs_set_u8(handle, "ConfigAktive", active ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    memset(blob, 0, sizeof(*blob));
    free(blob);
    if (err != ESP_OK) ESP_LOGE(TAG, "Cannot commit configuration: %s", esp_err_to_name(err));
    return err;
}

static bool enforce_factory_rtc(ebc_config_t *cfg)
{
    if (firmware_mode != EBC_FW_FACTORY) return false;
    bool changed = false;
    if (cfg->rtc.enabled != 1) {
        cfg->rtc.enabled = 1;
        changed = true;
    }
    if (cfg->rtc.int_gpio != CONFIG_EBCTESTER_RTC_INT_GPIO) {
        cfg->rtc.int_gpio = CONFIG_EBCTESTER_RTC_INT_GPIO;
        changed = true;
    }
    if (cfg->rtc.sda_gpio != CONFIG_EBCTESTER_I2C_SDA_GPIO) {
        cfg->rtc.sda_gpio = CONFIG_EBCTESTER_I2C_SDA_GPIO;
        changed = true;
    }
    if (cfg->rtc.scl_gpio != CONFIG_EBCTESTER_I2C_SCL_GPIO) {
        cfg->rtc.scl_gpio = CONFIG_EBCTESTER_I2C_SCL_GPIO;
        changed = true;
    }
    if (changed) {
        ESP_LOGI(TAG, "FactoryMode enforces RTC on GPIO%d/%d/%d",
                 cfg->rtc.int_gpio, cfg->rtc.sda_gpio, cfg->rtc.scl_gpio);
    }
    return changed;
}

static esp_err_t load(bool *valid)
{
    *valid = false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Configuration validation: new");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    ebc_config_blob_t *blob = calloc(1, sizeof(*blob));
    if (blob == NULL) {
        nvs_close(handle);
        ESP_LOGE(TAG, "Cannot allocate configuration for reading");
        return ESP_ERR_NO_MEM;
    }
    bool migrated = false;
    size_t size = sizeof(*blob);
    err = nvs_get_blob(handle, "config", blob, &size);
    uint8_t mode = 0, active = 0;
    const size_t legacy_length = offsetof(ebc_config_t, rtc);
    const bool legacy = blob->version == 1 && blob->length == legacy_length &&
                        size == offsetof(ebc_config_blob_t, payload) + legacy_length;
    if (err == ESP_OK && blob->magic == EBC_CONFIG_MAGIC &&
        (legacy || (size == sizeof(*blob) && blob->length == sizeof(blob->payload)))) {
        if (!legacy && blob->version != EBC_CONFIG_VERSION) {
            ESP_LOGI(TAG, "Configuration validation: version mismatch (%u)", blob->version);
        } else if (blob->crc != esp_crc32_le(0, (const uint8_t *)&blob->payload, blob->length)) {
            ESP_LOGI(TAG, "Configuration validation: CRC mismatch");
        } else {
            if (legacy) {
                blob->payload.rtc.enabled = 0;
                blob->payload.rtc.int_gpio = CONFIG_EBCTESTER_RTC_INT_GPIO;
                blob->payload.rtc.sda_gpio = CONFIG_EBCTESTER_I2C_SDA_GPIO;
                blob->payload.rtc.scl_gpio = CONFIG_EBCTESTER_I2C_SCL_GPIO;
                migrated = true;
            }
            err = nvs_get_u8(handle, "FirmwareMode", &mode);
            if (err == ESP_OK && mode <= EBC_FW_FACTORY &&
                ebc_config_validate(&blob->payload, false) == ESP_OK) {
                esp_err_t active_err = nvs_get_u8(handle, "ConfigAktive", &active);
                if (active_err == ESP_ERR_NVS_NOT_FOUND || active_err == ESP_ERR_NVS_TYPE_MISMATCH ||
                    (active_err == ESP_OK && active > 1)) {
                    active = 0;
                    ESP_LOGW(TAG, "Missing or invalid ConfigAktive; starting inactive");
                } else if (active_err != ESP_OK) {
                    err = active_err;
                }
                if (err == ESP_OK) {
                    memcpy(&config, &blob->payload, sizeof(config));
                    firmware_mode = (ebc_fw_mode_t)mode;
                    if (enforce_factory_rtc(&config)) migrated = true;
                    config_active = active == 1;
                    if (config_active && ebc_config_validate(&config, true) != ESP_OK) {
                        config_active = false;
                        ESP_LOGW(TAG, "Unsafe peripheral configuration; starting inactive");
                    }
                    *valid = true;
                    ESP_LOGI(TAG, "Configuration validation: valid");
                }
            }
        }
    } else {
        ESP_LOGI(TAG, "Configuration validation: new or invalid size/magic");
    }
    nvs_close(handle);
    memset(blob, 0, sizeof(*blob));
    free(blob);
    if (err == ESP_OK && *valid && migrated) {
        ESP_LOGI(TAG, "Persisting configuration migration or FactoryMode RTC activation");
        return persist(&config, config_active);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_INVALID_LENGTH ||
        err == ESP_ERR_NVS_TYPE_MISMATCH) return ESP_OK;
    return err;
}

esp_err_t ebc_config_init(void)
{
    if (initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS requires erase: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    bool valid;
    err = load(&valid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot load configuration: %s", esp_err_to_name(err));
        return err;
    }
    if (!valid) {
        load_defaults(&config);
        if (config.ota.ap_password[0] == '\0') {
            bootloader_random_enable();
            snprintf(config.ota.ap_password, sizeof(config.ota.ap_password), "%08lx%08lx",
                     (unsigned long)esp_random(), (unsigned long)esp_random());
            bootloader_random_disable();
            /* First-start serial output precedes registration of the web log buffer. */
            ESP_LOGI(TAG, "Generated setup AP password: %s", config.ota.ap_password);
        }
        ebc_io47_state_t state = detect_io47();
        static const char *states[] = {"open", "low", "high", "error"};
        ESP_LOGI(TAG, "IO47 state: %s", states[state]);
        if (state == IO47_ERROR) {
            ESP_LOGW(TAG, "IO47 detection failed; selecting MakerMode");
        }
        firmware_mode = state == IO47_HIGH ? EBC_FW_FACTORY : EBC_FW_MAKER;
        enforce_factory_rtc(&config);
        config_active = firmware_mode == EBC_FW_FACTORY;
        err = ebc_config_validate(&config, config_active);
        if (err == ESP_OK) err = persist(&config, config_active);
        if (err != ESP_OK) return err;
    }
    initialized = true;
    ESP_LOGI(TAG, "Firmware mode: %s; configuration active: %u",
             firmware_mode == EBC_FW_FACTORY ? "Factory" : "Maker", config_active);
    return ESP_OK;
}

const ebc_config_t *ebc_config_get(void) { return initialized ? &config : NULL; }
bool ebc_config_is_active(void) { return initialized && config_active; }
ebc_fw_mode_t ebc_config_get_mode(void) { return firmware_mode; }

esp_err_t ebc_config_save(const ebc_config_t *cfg)
{
    return ebc_config_save_with_active(cfg, config_active);
}

esp_err_t ebc_config_save_with_active(const ebc_config_t *cfg, bool active)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ebc_config_validate(cfg, false);
    if (err != ESP_OK) return err;
    ebc_config_t *next = malloc(sizeof(*next));
    if (next == NULL) {
        ESP_LOGE(TAG, "Cannot allocate configuration for saving");
        return ESP_ERR_NO_MEM;
    }
    memcpy(next, cfg, sizeof(*next));
    enforce_factory_rtc(next);
    err = ebc_config_validate(next, active);
    if (err == ESP_OK) err = persist(next, active);
    if (err == ESP_OK) {
        memcpy(&config, next, sizeof(config));
        config_active = active;
    }
    memset(next, 0, sizeof(*next));
    free(next);
    return err;
}

esp_err_t ebc_config_set_active(bool active)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    return ebc_config_save_with_active(&config, active);
}

esp_err_t ebc_config_factory_reset(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) esp_restart();
    ESP_LOGE(TAG, "Configuration reset failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t ebc_config_full_reset(void)
{
    esp_err_t err = nvs_flash_deinit();
    if (err == ESP_OK) err = nvs_flash_erase();
    if (err == ESP_OK) esp_restart();
    ESP_LOGE(TAG, "Full NVS reset failed: %s", esp_err_to_name(err));
    return err;
}
