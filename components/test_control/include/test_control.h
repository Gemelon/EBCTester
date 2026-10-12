/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the EBC link, the test recorder and the Batterie/Manuell/Automatik web tabs. */
esp_err_t test_control_init(void);

#ifdef __cplusplus
}
#endif
