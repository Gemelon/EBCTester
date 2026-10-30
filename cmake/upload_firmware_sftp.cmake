# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

foreach(required_variable
        SFTP_EXECUTABLE SFTP_HOST SFTP_USER SFTP_REMOTE_DIR SFTP_PORT
        DESTINATION_DIR FIRMWARE_VERSION FIRMWARE_PROJECT CMAKE_CURRENT_BINARY_DIR)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "SFTP upload is missing ${required_variable}")
    endif()
endforeach()

if(NOT SFTP_HOST MATCHES "^[A-Za-z0-9.-]+$"
   OR NOT SFTP_USER MATCHES "^[A-Za-z0-9._-]+$"
   OR NOT SFTP_PORT MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Invalid SFTP host, user, or port")
endif()
if(SFTP_PORT LESS 1 OR SFTP_PORT GREATER 65535)
    message(FATAL_ERROR "SFTP port must be between 1 and 65535")
endif()
if(NOT SFTP_REMOTE_DIR MATCHES "^/?[A-Za-z0-9._/-]+$"
   OR SFTP_REMOTE_DIR MATCHES "(^|/)\\.\\.?(/|$)")
    message(FATAL_ERROR "Invalid SFTP remote directory")
endif()
if(NOT FIRMWARE_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"
   OR NOT FIRMWARE_PROJECT MATCHES "^[A-Za-z0-9_-]+$")
    message(FATAL_ERROR "Invalid firmware project or version")
endif()
if(NOT EXISTS "${SFTP_EXECUTABLE}")
    message(FATAL_ERROR "OpenSSH sftp client is unavailable: ${SFTP_EXECUTABLE}")
endif()

string(REGEX REPLACE "/+$" "" remote_directory "${SFTP_REMOTE_DIR}")
if(remote_directory STREQUAL "")
    message(FATAL_ERROR "SFTP remote directory must not be the server root")
endif()

set(versioned_name "${FIRMWARE_PROJECT}-${FIRMWARE_VERSION}.bin")
set(versioned_file "${DESTINATION_DIR}/${versioned_name}")
set(compatibility_file "${DESTINATION_DIR}/${FIRMWARE_PROJECT}.bin")
set(manifest_file "${DESTINATION_DIR}/firmware.json")
set(license_directory "${DESTINATION_DIR}/licenses/${FIRMWARE_PROJECT}-${FIRMWARE_VERSION}")
foreach(local_file "${versioned_file}" "${compatibility_file}" "${manifest_file}")
    if(NOT EXISTS "${local_file}" OR IS_DIRECTORY "${local_file}")
        message(FATAL_ERROR "Published file is missing: ${local_file}")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${license_directory}")
    message(FATAL_ERROR "Published license bundle is missing: ${license_directory}")
endif()

set(remote_versioned_file "${remote_directory}/${versioned_name}")
set(preflight_file "${CMAKE_CURRENT_BINARY_DIR}/sftp-existing-${versioned_name}")
set(preflight_batch "${CMAKE_CURRENT_BINARY_DIR}/sftp-preflight-${FIRMWARE_VERSION}.txt")
file(REMOVE "${preflight_file}")
file(WRITE "${preflight_batch}"
    "-get \"${remote_versioned_file}\" \"${preflight_file}\"\n")

function(run_sftp batch_file operation)
    message(STATUS "Starting SFTP ${operation}")
    execute_process(
        COMMAND "${SFTP_EXECUTABLE}"
            -oBatchMode=yes
            -oStrictHostKeyChecking=yes
            -oConnectTimeout=15
            -oServerAliveInterval=15
            -oServerAliveCountMax=3
            -P "${SFTP_PORT}"
            -b "${batch_file}"
            "${SFTP_USER}@${SFTP_HOST}"
        RESULT_VARIABLE result
        TIMEOUT 600
    )
    file(REMOVE "${batch_file}")
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "SFTP ${operation} failed (${result}); see the client output above.")
    endif()
endfunction()

run_sftp("${preflight_batch}" "version check")
set(version_already_published FALSE)
if(EXISTS "${preflight_file}")
    file(SHA256 "${versioned_file}" local_hash)
    file(SHA256 "${preflight_file}" remote_hash)
    file(SIZE "${versioned_file}" local_size)
    file(SIZE "${preflight_file}" remote_size)
    file(REMOVE "${preflight_file}")
    if(NOT local_hash STREQUAL remote_hash OR NOT local_size EQUAL remote_size)
        message(FATAL_ERROR
            "Refusing to overwrite immutable remote firmware with different bytes: ${remote_versioned_file}")
    endif()
    set(version_already_published TRUE)
endif()

set(upload_batch "${CMAKE_CURRENT_BINARY_DIR}/sftp-publish-${FIRMWARE_VERSION}.txt")
set(license_remote_directory "${remote_directory}/licenses")
file(WRITE "${upload_batch}" "-mkdir \"${license_remote_directory}\"\n")
if(NOT version_already_published)
    file(APPEND "${upload_batch}"
        "put \"${versioned_file}\" \"${remote_versioned_file}.uploading\"\n"
        "rename \"${remote_versioned_file}.uploading\" \"${remote_versioned_file}\"\n")
endif()
file(APPEND "${upload_batch}"
    "put \"${compatibility_file}\" \"${remote_directory}/${FIRMWARE_PROJECT}.bin.uploading\"\n"
    "rename \"${remote_directory}/${FIRMWARE_PROJECT}.bin.uploading\" \"${remote_directory}/${FIRMWARE_PROJECT}.bin\"\n"
    "put -r \"${license_directory}\" \"${license_remote_directory}\"\n"
    "put \"${manifest_file}\" \"${remote_directory}/firmware.json.uploading\"\n"
    "rename \"${remote_directory}/firmware.json.uploading\" \"${remote_directory}/firmware.json\"\n")

run_sftp("${upload_batch}" "publication")
message(STATUS "Uploaded ${versioned_name}, compatibility BIN, license bundle, and manifest to ${SFTP_HOST}:${remote_directory}")
