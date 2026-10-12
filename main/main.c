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

static const char *TAG = "app_main";

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing SD card manager");
    ESP_ERROR_CHECK(sd_card_manager_init());
    ESP_LOGI(TAG, "Scheduling database initialization");
    ESP_ERROR_CHECK(battery_database_init());
    /* UI extensions must be registered before the web server starts. */
    ESP_ERROR_CHECK(battery_dashboard_init());
    ESP_ERROR_CHECK(mariadb_sync_init());
    ESP_ERROR_CHECK(test_control_init());
    ESP_ERROR_CHECK(ui_localization_init());
    ESP_LOGI(TAG, "Starting device services");
    ESP_ERROR_CHECK(device_app_start());
}