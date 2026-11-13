/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "local_discovery.h"

#include <stddef.h>
#include "esp_log.h"
#include "mdns.h"
#include "network_manager.h"

static const char *TAG = "local_discovery";

esp_err_t local_discovery_start(void)
{
    network_status_t network;
    network_manager_get_status(&network);
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        return err;
    }
    err = mdns_hostname_set(network.ap_ssid);
    if (err == ESP_OK) {
        err = mdns_service_add(network.ap_ssid, "_http", "_tcp", 80, NULL, 0);
    }
    if (err != ESP_OK) {
        mdns_free();
        return err;
    }
    ESP_LOGI(TAG, "Web interface: http://%s.local/", network.ap_ssid);
    return ESP_OK;
}
