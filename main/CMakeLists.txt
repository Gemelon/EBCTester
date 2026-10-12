# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
    REQUIRES device_app sd_card_manager battery_database battery_dashboard mariadb_sync test_control ui_localization)
