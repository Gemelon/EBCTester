# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

set(MAIN_HA_MQTT_ENABLED FALSE)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig" HA_MQTT_SDKCONFIG)
    string(FIND "${HA_MQTT_SDKCONFIG}" "CONFIG_EBCTESTER_HA_MQTT_ENABLE=y"
           HA_MQTT_CONFIG_POSITION)
    if(NOT HA_MQTT_CONFIG_POSITION EQUAL -1)
        set(MAIN_HA_MQTT_ENABLED TRUE)
    endif()
endif()

if(MAIN_HA_MQTT_ENABLED)
    idf_component_register(
        SRCS "main.c"
        INCLUDE_DIRS "."
        REQUIRES device_app sd_card_manager battery_database battery_dashboard mariadb_sync
                 test_control ui_localization debug_log ha_mqtt
    )
else()
    idf_component_register(
        SRCS "main.c"
        INCLUDE_DIRS "."
        REQUIRES device_app sd_card_manager battery_database battery_dashboard mariadb_sync
                 test_control ui_localization debug_log
    )
endif()
