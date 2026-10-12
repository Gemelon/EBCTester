# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

idf_component_register(SRCS "resistance_test.cpp" INCLUDE_DIRS "include"
    REQUIRES ebc_link test_store
    PRIV_REQUIRES battery_database nvs_flash esp_timer)
