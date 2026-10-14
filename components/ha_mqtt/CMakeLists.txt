# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "ha_mqtt.c" "ha_mqtt_web.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__cjson espressif__mqtt esp_netif esp_timer nvs_flash
                  network_manager web_interface esp_http_server test_control
    EMBED_FILES "assets/mqtt.html" "assets/mqtt.js"
)
