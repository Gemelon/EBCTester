# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.22)

set(PROJECT_VER "1.3.51")
include("${CMAKE_CURRENT_LIST_DIR}/CMakeUserConfig.cmake" OPTIONAL)
set(OTA_SUITE_DIR "${CMAKE_CURRENT_LIST_DIR}/external/ota-suite" CACHE PATH
    "Path to the reusable ota-suite package")
if(NOT EXISTS "${OTA_SUITE_DIR}/components/device_app/CMakeLists.txt")
    message(FATAL_ERROR
        "ota-suite was not found at '${OTA_SUITE_DIR}'. Initialize Git submodules or set OTA_SUITE_DIR.")
endif()
set(SD_CARD_MANAGER_DIR "${CMAKE_CURRENT_LIST_DIR}/external/sd-card-manager" CACHE PATH
    "Path to the reusable sd-card-manager package")
if(NOT EXISTS "${SD_CARD_MANAGER_DIR}/components/sd_card_manager/CMakeLists.txt")
    message(FATAL_ERROR
        "sd-card-manager was not found at '${SD_CARD_MANAGER_DIR}'. Initialize Git submodules or set SD_CARD_MANAGER_DIR.")
endif()
set(EXTRA_COMPONENT_DIRS
    "${OTA_SUITE_DIR}/components"
    "${SD_CARD_MANAGER_DIR}/components")
set(HA_MQTT_ENABLED FALSE)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/sdkconfig" HA_MQTT_SDKCONFIG)
    string(FIND "${HA_MQTT_SDKCONFIG}" "CONFIG_EBCTESTER_HA_MQTT_ENABLE=y"
           HA_MQTT_CONFIG_POSITION)
    if(NOT HA_MQTT_CONFIG_POSITION EQUAL -1)
        set(HA_MQTT_ENABLED TRUE)
    endif()
endif()
if(NOT HA_MQTT_ENABLED)
    list(APPEND EXCLUDE_COMPONENTS ha_mqtt)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(EBCTester)

option(EBCTESTER_PUBLISH_FIRMWARE "Publish firmware after each successful build" OFF)
set(EBCTESTER_PUBLISH_DIR "" CACHE PATH "Existing firmware publication directory")
set(EBCTESTER_PUBLISH_ORIGIN "" CACHE STRING "Firmware server origin (scheme, host, optional port)")
set(EBCTESTER_PUBLISH_URL_PATH "" CACHE STRING "Firmware server directory, e.g. /BatteryTester")
if(EBCTESTER_PUBLISH_FIRMWARE)
    if(EBCTESTER_PUBLISH_DIR STREQUAL "" OR EBCTESTER_PUBLISH_ORIGIN STREQUAL "")
        message(FATAL_ERROR "Firmware publication requires EBCTESTER_PUBLISH_DIR and EBCTESTER_PUBLISH_ORIGIN.")
    endif()
    add_custom_target(publish_firmware ALL
        COMMAND ${CMAKE_COMMAND}
            "-DPROJECT_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}"
            "-DOTA_SUITE_DIR=${OTA_SUITE_DIR}"
            "-DSD_CARD_MANAGER_DIR=${SD_CARD_MANAGER_DIR}"
            "-DDESTINATION_DIR=${EBCTESTER_PUBLISH_DIR}"
            "-DFIRMWARE_VERSION=${PROJECT_VER}"
            -P "${CMAKE_CURRENT_LIST_DIR}/cmake/copy_license_bundle.cmake"
        COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_FILE=${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin"
            "-DDESTINATION_DIR=${EBCTESTER_PUBLISH_DIR}"
            "-DFIRMWARE_VERSION=${PROJECT_VER}"
            "-DFIRMWARE_PROJECT=${PROJECT_NAME}"
            "-DFIRMWARE_TARGET=${IDF_TARGET}"
            "-DFIRMWARE_ORIGIN=${EBCTESTER_PUBLISH_ORIGIN}"
            "-DFIRMWARE_URL_PATH=${EBCTESTER_PUBLISH_URL_PATH}"
            -P "${OTA_SUITE_DIR}/cmake/publish_firmware.cmake"
        DEPENDS app bootloader partition_table_bin
        COMMENT "Publishing immutable firmware and atomic manifest to ${EBCTESTER_PUBLISH_DIR}"
        VERBATIM
    )
endif()
