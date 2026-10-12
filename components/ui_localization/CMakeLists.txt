# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

idf_component_register(SRCS "ui_localization.cpp" INCLUDE_DIRS "include"
    REQUIRES web_interface sd_card_manager espressif__cjson
    EMBED_FILES "assets/languages.html" "assets/languages.js")
