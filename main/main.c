/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "battery_dashboard.h"
#include "battery_database.h"
#include "debug_log.h"
#include "debug_log_web.h"
#include "device_app.h"
#include "ebc_config.h"
#include "ebc_config_web.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mariadb_sync.h"
#include "sd_card_manager.h"
#include "test_control.h"
#include "ui_localization.h"
#include "network_manager.h"
#include "ota_manager.h"
#include "status_led.h"
#include "led_settings.h"
#include "update_check.h"
#include "web_interface.h"
#include <string.h>
#include "ha_mqtt.h"
#include "ha_mqtt_web.h"
#include "rtc_clock.h"
#include "ds3231_time.h"
#include "local_discovery.h"

static const char *TAG = "app_main";
static ebc_config_t boot_config;

static void publish_persisted_sample(float voltage_v, float current_a, float capacity_ah)
{
    const ha_mqtt_measurement_t measurement = {
        .voltage_v = voltage_v,
        .current_a = current_a,
        .capacity_ah = capacity_ah,
    };
    const esp_err_t err = ha_mqtt_publish_measurement(&measurement);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "Skipping live MQTT measurement: %s", esp_err_to_name(err));
    }
}

static esp_err_t save_login(const char *password, unsigned days, bool login_enabled)
{
    ebc_config_t cfg = *ebc_config_get();
    strlcpy(cfg.ota.web_password, password, sizeof(cfg.ota.web_password));
    cfg.ota.session_days = days;
    cfg.ota.login_enabled = login_enabled;
    return ebc_config_save(&cfg);
}

static void configure_services(bool active)
{
    const ebc_config_t *cfg = &boot_config;
    const network_runtime_config_t network = {
        .ap_password = cfg->ota.ap_password,
        .sntp_server = cfg->ota.sntp_server,
        .ap_only = !active,
        .time_synced = rtc_clock_ntp_synced,
        .sntp_sync_interval_ms = active && cfg->rtc.enabled ? RTC_SYNC_INTERVAL_MS : 0,
    };
    const ota_runtime_config_t ota = {
        .manifest_url = cfg->ota.manifest_url,
        .allow_http = cfg->ota.allow_http,
        .services_enabled = active,
    };
    const update_check_runtime_config_t update = {
        .manifest_url = cfg->ota.manifest_url,
        .allow_http = cfg->ota.allow_http,
        .services_enabled = active,
    };
    const web_interface_runtime_config_t web = {
        .password = cfg->ota.web_password,
        .tab_order = cfg->ota.tab_order,
        .primary_tabs = cfg->ota.primary_tabs,
        .settings_require_password = cfg->ota.settings_require_password,
        .sd_languages = cfg->ota.sd_languages,
        .peripherals_active = active,
        .maker_mode = ebc_config_get_mode() == EBC_FW_MAKER,
        .session_days = cfg->ota.session_days,
        .login_enabled = cfg->ota.login_enabled,
        .save_login = save_login,
        .get_clock_utc = rtc_clock_get_utc,
    };
    sd_card_manager_config_t sd = {
        .spi = cfg->sd.bus == SD_BUS_SPI,
        .enabled = active,
        .frequency_khz = cfg->sd.bus == SD_BUS_SPI ? cfg->sd.spi_freq_khz : cfg->sd.sdmmc_freq_khz,
        .card_detect_gpio = cfg->sd.card_detect_gpio,
        .card_detect_active_high = cfg->sd.card_detect_active_high != 0,
        .clk_gpio = cfg->sd.clk_gpio, .cmd_gpio = cfg->sd.cmd_gpio,
        .d0_gpio = cfg->sd.d0_gpio, .d1_gpio = cfg->sd.d1_gpio,
        .d2_gpio = cfg->sd.d2_gpio, .d3_gpio = cfg->sd.d3_gpio,
        .spi_miso_gpio = cfg->sd.spi_miso_gpio, .spi_mosi_gpio = cfg->sd.spi_mosi_gpio,
        .spi_cs_gpio = cfg->sd.spi_cs_gpio, .spi_clk_gpio = cfg->sd.spi_clk_gpio,
    };
    strlcpy(sd.mount_point, cfg->sd.mount_point, sizeof(sd.mount_point));
    ESP_ERROR_CHECK(network_manager_set_runtime_config(&network));
    ESP_ERROR_CHECK(ota_manager_set_runtime_config(&ota));
    ESP_ERROR_CHECK(update_check_set_runtime_config(&update));
    ESP_ERROR_CHECK(web_interface_set_runtime_config(&web));
    ESP_ERROR_CHECK(sd_card_manager_set_config(&sd));
    device_app_set_led_enabled(active && cfg->ota.status_led_enabled);
    if (active && cfg->ota.status_led_enabled) ESP_ERROR_CHECK(status_led_set_gpio(cfg->ota.led_gpio));
}

void app_main(void)
{
    ESP_ERROR_CHECK(ebc_config_init());
    boot_config = *ebc_config_get();
    const bool active = ebc_config_is_active();
    configure_services(active);
    const esp_err_t log_err = debug_log_init();
    if (log_err != ESP_OK) {
        ESP_LOGW(TAG, "Debug log buffer unavailable: %s", esp_err_to_name(log_err));
    }
    /* UI extensions must be registered before the web server starts. */
    ESP_ERROR_CHECK(debug_log_web_init());
    ESP_ERROR_CHECK(ebc_config_web_init());
    ESP_ERROR_CHECK(led_settings_init());
    const esp_err_t rtc_err = rtc_clock_start();
    if (rtc_err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start RTC service: %s; network time remains available", esp_err_to_name(rtc_err));
    }
    if (active) {
        ESP_ERROR_CHECK(sd_card_manager_init());
        ESP_ERROR_CHECK(battery_database_init());
        ESP_ERROR_CHECK(battery_dashboard_init());
        const esp_err_t mariadb_err = mariadb_sync_init();
        if (mariadb_err != ESP_OK) {
            ESP_LOGW(TAG, "MariaDB module unavailable: %s", esp_err_to_name(mariadb_err));
        }
        ESP_ERROR_CHECK(ha_mqtt_web_init());
        test_control_set_persisted_sample_callback(publish_persisted_sample);
        ESP_ERROR_CHECK(test_control_init());
        ESP_ERROR_CHECK(ui_localization_init());
    } else {
        ESP_LOGI(TAG, "Configuration inactive: all application peripheral services are disabled");
    }
    ESP_ERROR_CHECK(ebc_config_web_register_reset());
    ESP_LOGI(TAG, "Starting device services");
    ESP_ERROR_CHECK(device_app_start());
    const esp_err_t discovery_err = local_discovery_start();
    if (discovery_err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot start mDNS discovery: %s; use the device IP address",
                 esp_err_to_name(discovery_err));
    }
    const esp_err_t debug_err = debug_log_web_start();
    if (debug_err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot apply debug log setting: %s", esp_err_to_name(debug_err));
    }
    const esp_err_t mqtt_err = active ? ha_mqtt_web_start() : ESP_OK;
    if (mqtt_err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start MQTT integration: %s", esp_err_to_name(mqtt_err));
    }
}