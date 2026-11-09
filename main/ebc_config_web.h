/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ebc_config_web_init(void);
esp_err_t ebc_config_web_register_reset(void);

#ifdef __cplusplus
}
#endif
