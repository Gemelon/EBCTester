/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "battery_dashboard.h"
#include "battery_database.h"
#include "device_app.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mariadb_sync.h"
#include "sd_card_manager.h"
#include "test_control.h"
#include "ui_localization.h"
#include "sdkconfig.h"
#if defined(CONFIG_EBCTESTER_HA_MQTT_ENABLE) && CONFIG_EBCTESTER_HA_MQTT_ENABLE
#include "ha_mqtt.h"
#include <stdio.h>
#endif

static const char *TAG = "app_main";

#if defined(CONFIG_EBCTESTER_HA_MQTT_ENABLE) && CONFIG_EBCTESTER_HA_MQTT_ENABLE
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

static void start_mqtt(void)
{
    if (CONFIG_EBCTESTER_HA_MQTT_HOST[0] == '\0') {
        ESP_LOGW(TAG, "MQTT is enabled but no broker host is configured");
        return;
    }
    ha_mqtt_settings_t settings;
    ha_mqtt_settings_defaults(&settings);
    settings.enabled = true;
    settings.port = CONFIG_EBCTESTER_HA_MQTT_PORT;
    settings.measurement_interval_seconds = CONFIG_EBCTESTER_HA_MQTT_INTERVAL_SECONDS;
#if defined(CONFIG_EBCTESTER_HA_MQTT_ALLOW_REMOTE_START) && \
    CONFIG_EBCTESTER_HA_MQTT_ALLOW_REMOTE_START
    settings.allow_remote_start = CONFIG_EBCTESTER_HA_MQTT_ALLOW_REMOTE_START;
#endif
    snprintf(settings.host, sizeof(settings.host), "%s", CONFIG_EBCTESTER_HA_MQTT_HOST);
    snprintf(settings.username, sizeof(settings.username), "%s", CONFIG_EBCTESTER_HA_MQTT_USERNAME);
    snprintf(settings.password, sizeof(settings.password), "%s", CONFIG_EBCTESTER_HA_MQTT_PASSWORD);
    snprintf(settings.topic_prefix, sizeof(settings.topic_prefix), "%s",
             CONFIG_EBCTESTER_HA_MQTT_TOPIC_PREFIX);
    const esp_err_t err = ha_mqtt_start(&settings);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start MQTT integration: %s", esp_err_to_name(err));
    }
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing SD card manager");
    ESP_ERROR_CHECK(sd_card_manager_init());
    ESP_LOGI(TAG, "Scheduling database initialization");
    ESP_ERROR_CHECK(battery_database_init());
    /* UI extensions must be registered before the web server starts. */
    ESP_ERROR_CHECK(battery_dashboard_init());
    ESP_ERROR_CHECK(mariadb_sync_init());
#if defined(CONFIG_EBCTESTER_HA_MQTT_ENABLE) && CONFIG_EBCTESTER_HA_MQTT_ENABLE
    test_control_set_persisted_sample_callback(publish_persisted_sample);
#endif
    ESP_ERROR_CHECK(test_control_init());
    ESP_ERROR_CHECK(ui_localization_init());
    ESP_LOGI(TAG, "Starting device services");
    ESP_ERROR_CHECK(device_app_start());
#if defined(CONFIG_EBCTESTER_HA_MQTT_ENABLE) && CONFIG_EBCTESTER_HA_MQTT_ENABLE
    start_mqtt();
#endif
}