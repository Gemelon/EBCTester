/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the MQTT settings page before the web server starts. */
esp_err_t ha_mqtt_web_init(void);

/* Loads saved settings and starts MQTT after network services are available. */
esp_err_t ha_mqtt_web_start(void);

#ifdef __cplusplus
}
#endif
