# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "led_settings.cpp" "led_policy.c" "main.c" "ebc_config_web.cpp"
    INCLUDE_DIRS "."
    REQUIRES device_app sd_card_manager battery_database battery_dashboard mariadb_sync
             test_control ui_localization debug_log ha_mqtt ebc_config status_led ebc_link nvs_flash esp_timer
    EMBED_FILES "assets/maker_settings.html" "assets/maker_settings.js"
                "assets/config_reset.html" "assets/config_reset.js"
                "assets/led_settings.html" "assets/led_settings.js"
)
