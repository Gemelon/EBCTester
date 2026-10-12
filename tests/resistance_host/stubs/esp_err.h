/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 1;
constexpr esp_err_t ESP_ERR_NVS_NOT_INITIALIZED = 2, ESP_ERR_NVS_NOT_FOUND = 3;
inline const char *esp_err_to_name(esp_err_t) { return "mock-error"; }
