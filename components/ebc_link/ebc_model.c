/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ebc_model.h"

#include <stddef.h>
void ebc_model_get_limits(uint8_t ident, ebc_model_limits_t *limits)
{
    static const ebc_model_limits_t models[] = {
        {0x09, "EBC-A20", true, 18.0f, 30.0f, 5.0f, 20.0f, 85.0f},
        {0x10, "EBC-A40L", true, 5.0f, 5.0f, 40.0f, 40.0f, 200.0f},
    };
    static const ebc_model_limits_t unknown = {
        0, "Unknown", false, 5.0f, 5.0f, 5.0f, 20.0f, 85.0f};

    for (size_t index = 0; index < sizeof(models) / sizeof(models[0]); ++index) {
        if (models[index].ident == ident) {
            *limits = models[index];
            return;
        }
    }
    *limits = unknown;
    limits->ident = ident;
}
