/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ebc_model.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    const char *name;
    uint8_t ident;
    bool version_specific;
    uint16_t version;
    bool enabled;
    float max_charge_voltage_v;
    float max_discharge_voltage_v;
    float max_charge_current_a;
    float max_discharge_current_a;
    float max_power_w;
    float current_scale;
    float voltage_scale;
    uint32_t supported_modes;
} ebc_model_definition_t;

#define MODEL(name_, ident_, has_version_, version_, enabled_, charge_v_, discharge_v_, charge_a_, discharge_a_, power_) \
    {name_, ident_, has_version_, version_, enabled_, charge_v_, discharge_v_, charge_a_, discharge_a_, power_, 0.01f, 0.001f, 0}

static const ebc_model_definition_t models[] = {
    MODEL("EBC-A05+", 5, true, 302, false, 5, 5, 5, 20, 85),
    MODEL("EBC-A05", 5, false, 0, false, 5, 5, 5, 20, 85),
    MODEL("EBC-A10+", 6, true, 310, false, 5, 5, 5, 20, 85),
    MODEL("EBC-A10H", 6, false, 0, false, 5, 5, 5, 20, 85),
    MODEL("EBC-A20+", 9, true, 311, false, 18, 30, 5, 20, 85),
    MODEL("EBC-A20", 9, false, 0, true, 18, 30, 5, 20, 85),
    MODEL("EBC-A50L", 10, true, 311, true, 5, 5, 50, 50, 250),
    MODEL("EBC-A40L", 10, false, 0, true, 5, 5, 40, 40, 200),
    MODEL("EBD-M10", 20, true, 310, false, 5, 5, 5, 20, 85),
    MODEL("EBD-A10H", 20, false, 0, false, 5, 5, 5, 20, 85),
    MODEL("EBD-M03+", 25, true, 310, false, 5, 5, 5, 20, 85),
    MODEL("EBD-M05", 25, true, 315, false, 5, 5, 5, 20, 85),
    MODEL("EBD-M03", 25, false, 0, false, 5, 5, 5, 20, 85),
    MODEL("EBD-A100L", 38, true, 310, false, 5, 5, 5, 20, 85),
    MODEL("EBC-A100L", 38, true, 311, false, 5, 5, 5, 20, 85),
    MODEL("EBD-A100", 38, false, 0, false, 5, 5, 5, 20, 85),
};

static const ebc_model_definition_t *find_definition(uint8_t ident, bool version_known,
                                                     uint16_t version)
{
    const ebc_model_definition_t *fallback = NULL;
    for (size_t index = 0; index < sizeof(models) / sizeof(models[0]); ++index) {
        const ebc_model_definition_t *model = &models[index];
        if (model->ident != ident) {
            continue;
        }
        if (!model->version_specific) {
            fallback = model;
        } else if (version_known && model->version == version) {
            return model;
        }
    }
    return fallback;
}

static void limits_from_definition(const ebc_model_definition_t *definition,
                                   ebc_model_limits_t *limits)
{
    if (definition == NULL) {
        *limits = (ebc_model_limits_t){
            0, "Unknown", false, false, 5.0f, 5.0f, 5.0f, 20.0f, 85.0f};
        return;
    }
    *limits = (ebc_model_limits_t){
        definition->ident, definition->name, true, definition->enabled,
        definition->max_charge_voltage_v, definition->max_discharge_voltage_v,
        definition->max_charge_current_a, definition->max_discharge_current_a,
        definition->max_power_w};
}

void ebc_model_get_limits(uint8_t selection, ebc_model_limits_t *limits)
{
    ebc_model_detection_t detection;
    if (selection == 0x10) {
        /* TODO: 0x10 is the legacy A40L selection value (16 decimal); protocol ident is 0x0A. */
        ebc_model_detect(10, true, 302, &detection);
    } else if (selection == 10) {
        ebc_model_detect(10, true, 311, &detection);
    } else {
        ebc_model_detect(selection, false, 0, &detection);
    }
    *limits = detection.limits;
}

void ebc_model_detect(uint8_t raw_ident, bool version_known, uint16_t version,
                      ebc_model_detection_t *detection)
{
    const uint8_t ident = (uint8_t)(raw_ident % 100);
    const ebc_model_definition_t *definition = find_definition(ident, version_known, version);
    detection->ident = ident;
    detection->version_known = version_known;
    detection->version = version_known ? version : 0;
    detection->known = definition != NULL;
    detection->enabled = definition != NULL && definition->enabled;
    if (ident == 10 && !version_known) {
        detection->name = "EBC-A40L/A50L (Version unknown)";
        detection->status = EBC_MODEL_VERSION_PENDING;
    } else if (definition == NULL) {
        detection->name = "Unknown";
        detection->status = EBC_MODEL_UNKNOWN;
    } else {
        detection->name = definition->name;
        detection->status = definition->enabled ? EBC_MODEL_SUPPORTED : EBC_MODEL_UNSUPPORTED;
    }
    limits_from_definition(definition, &detection->limits);
    detection->limits.ident = ident;
}

bool ebc_model_selection_matches(uint8_t selection, uint8_t ident,
                                 bool version_known, uint16_t version)
{
    ebc_model_detection_t detection;
    ebc_model_detect(ident, version_known, version, &detection);
    if (detection.status == EBC_MODEL_VERSION_PENDING) {
        return selection == 10 || selection == 0x10;
    }
    if (!detection.known || !detection.enabled) {
        return false;
    }
    if (selection == 0x10) {
        return detection.ident == 10 && strcmp(detection.name, "EBC-A40L") == 0;
    }
    if (selection == 10) {
        return detection.ident == 10 && strcmp(detection.name, "EBC-A50L") == 0;
    }
    return selection == detection.ident && strcmp(detection.name, "EBC-A20") == 0;
}
