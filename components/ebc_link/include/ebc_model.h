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
    bool enabled;
    float max_charge_voltage_v;
    float max_discharge_voltage_v;
    float max_charge_current_a;
    float max_discharge_current_a;
    float max_power_w;
} ebc_model_limits_t;

typedef enum {
    EBC_MODEL_UNKNOWN,
    EBC_MODEL_SUPPORTED,
    EBC_MODEL_UNSUPPORTED,
    EBC_MODEL_VERSION_PENDING,
} ebc_model_status_t;

typedef struct {
    uint8_t ident;
    const char *name;
    ebc_model_status_t status;
    bool known;
    bool enabled;
    bool version_known;
    uint16_t version;
    ebc_model_limits_t limits;
} ebc_model_detection_t;

/* Resolves saved model selectors; 0x10 remains the legacy A40L selection value. */
void ebc_model_get_limits(uint8_t selection, ebc_model_limits_t *limits);
/* Resolves the protocol ident (raw byte modulo 100) and optional firmware version. */
void ebc_model_detect(uint8_t raw_ident, bool version_known, uint16_t version,
                      ebc_model_detection_t *detection);
bool ebc_model_selection_matches(uint8_t selection, uint8_t ident,
                                 bool version_known, uint16_t version);

#ifdef __cplusplus
}
#endif
