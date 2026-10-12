# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

if(NOT IS_DIRECTORY "${DESTINATION_DIR}")
    message(FATAL_ERROR "License destination is unavailable: ${DESTINATION_DIR}")
endif()
if(NOT FIRMWARE_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "Invalid license bundle firmware version")
endif()

set(bundle "${DESTINATION_DIR}/licenses/EBCTester-${FIRMWARE_VERSION}")

function(copy_package source relative_destination)
    foreach(document LICENSE NOTICE README.md THIRD_PARTY_NOTICES.md)
        if(NOT EXISTS "${source}/${document}")
            message(FATAL_ERROR "Required license document is missing: ${source}/${document}")
        endif()
    endforeach()
    if(NOT IS_DIRECTORY "${source}/third_party/licenses")
        message(FATAL_ERROR "Third-party license texts are missing: ${source}")
    endif()
    set(destination "${bundle}/${relative_destination}")
    file(MAKE_DIRECTORY "${destination}")
    file(COPY "${source}/LICENSE" "${source}/NOTICE"
        "${source}/README.md" "${source}/THIRD_PARTY_NOTICES.md"
        DESTINATION "${destination}")
    file(COPY "${source}/third_party" DESTINATION "${destination}")
endfunction()

copy_package("${PROJECT_SOURCE_DIR}" ".")
copy_package("${OTA_SUITE_DIR}" "external/ota-suite")
copy_package("${SD_CARD_MANAGER_DIR}" "external/sd-card-manager")

set(bootstrap_license "components/web_interface/assets/BOOTSTRAP-LICENSE")
set(chart_license "components/battery_dashboard/assets/CHARTJS-LICENSE.md")
set(color_license "components/battery_dashboard/assets/KURKLE-COLOR-LICENSE.md")
foreach(license_file "${chart_license}" "${color_license}")
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${license_file}")
        message(FATAL_ERROR "Required asset license is missing: ${license_file}")
    endif()
    file(COPY "${PROJECT_SOURCE_DIR}/${license_file}"
        DESTINATION "${bundle}/components/battery_dashboard/assets")
endforeach()
if(NOT EXISTS "${OTA_SUITE_DIR}/${bootstrap_license}")
    message(FATAL_ERROR "Required Bootstrap license is missing")
endif()
file(COPY "${OTA_SUITE_DIR}/${bootstrap_license}"
    DESTINATION "${bundle}/external/ota-suite/components/web_interface/assets")
message(STATUS "License documentation copied to ${bundle}; distribute it with the firmware")
