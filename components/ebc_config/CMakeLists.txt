# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

idf_component_register(SRCS "ebc_config.c" INCLUDE_DIRS "include"
                       REQUIRES nvs_flash esp_driver_gpio esp_rom esp_system bootloader_support http_parser)
