/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Registers the Debug tab and its settings card.
 *
 * Must be called before the web server starts.
 */
esp_err_t debug_log_web_init(void);

/**
 * @brief Applies the persisted enable flag.
 *
 * Call this after NVS is available. Capturing stays active until this runs so
 * that start-up messages are kept when the feature is enabled.
 */
esp_err_t debug_log_web_start(void);

#ifdef __cplusplus
}
#endif
