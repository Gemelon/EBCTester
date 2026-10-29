/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t ident;
    const char *name;
    bool known;
    float max_charge_voltage_v;
    float max_discharge_voltage_v;
    float max_charge_current_a;
    float max_discharge_current_a;
    float max_power_w;
} ebc_model_limits_t;

/* Unknown identifiers use conservative limits and do not prevent connection. */
void ebc_model_get_limits(uint8_t ident, ebc_model_limits_t *limits);

#ifdef __cplusplus
}
#endif
