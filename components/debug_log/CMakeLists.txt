# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "debug_log.c" "debug_log_web.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__cjson esp_http_server log nvs_flash web_interface
    EMBED_FILES "assets/debug.html" "assets/debug.js"
                "assets/debug_settings.html" "assets/debug_settings.js"
)
