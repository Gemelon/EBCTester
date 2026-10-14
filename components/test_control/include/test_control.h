/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*test_control_persisted_sample_callback_t)(float voltage_v, float current_a,
                                                          float capacity_ah);

/* Starts the EBC link, the test recorder and the Battery/Manual/Automatic web tabs. */
esp_err_t test_control_init(void);
void test_control_set_persisted_sample_callback(
    test_control_persisted_sample_callback_t callback);

#ifdef __cplusplus
}
#endif
