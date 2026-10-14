# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "ha_mqtt.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__cjson espressif__mqtt esp_netif esp_timer
)
