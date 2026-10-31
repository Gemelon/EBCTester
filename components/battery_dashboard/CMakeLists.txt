# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "battery_dashboard.cpp" "run_management.cpp" "run_management_web.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES battery_database web_interface esp_http_server espressif__cjson test_store
    EMBED_FILES "assets/dashboard.html" "assets/dashboard.js" "assets/chart.umd.js"
                "assets/brand_logo.png" "assets/brand_icon.png" "assets/test_runs.html" "assets/test_runs.js"
)
