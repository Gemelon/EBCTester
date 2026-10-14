/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define HA_MQTT_HOST_MAX 128
#define HA_MQTT_USERNAME_MAX 64
#define HA_MQTT_PASSWORD_MAX 128
#define HA_MQTT_TOPIC_PREFIX_MAX 64

typedef enum {
    HA_MQTT_STATE_DISABLED,
    HA_MQTT_STATE_NOT_CONFIGURED,
    HA_MQTT_STATE_CONNECTING,
    HA_MQTT_STATE_CONNECTED,
    HA_MQTT_STATE_DISCONNECTED,
    HA_MQTT_STATE_ERROR,
} ha_mqtt_state_t;

typedef struct {
    ha_mqtt_state_t state;
    char message[128];
} ha_mqtt_status_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ha_mqtt_command_callback_t)(const char *command, const char *value);

typedef struct {
    bool enabled;
    char host[HA_MQTT_HOST_MAX];
    uint16_t port;
    char username[HA_MQTT_USERNAME_MAX];
    char password[HA_MQTT_PASSWORD_MAX];
    char topic_prefix[HA_MQTT_TOPIC_PREFIX_MAX];
    uint32_t measurement_interval_seconds;
    bool allow_remote_start;
    ha_mqtt_command_callback_t command_callback;
} ha_mqtt_settings_t;

typedef struct {
    float voltage_v;
    float current_a;
    float capacity_ah;
} ha_mqtt_measurement_t;

void ha_mqtt_settings_defaults(ha_mqtt_settings_t *settings);
esp_err_t ha_mqtt_start(const ha_mqtt_settings_t *settings);
esp_err_t ha_mqtt_stop(void);
esp_err_t ha_mqtt_get_status(ha_mqtt_status_t *status);
esp_err_t ha_mqtt_publish_measurement(const ha_mqtt_measurement_t *measurement);

#ifdef __cplusplus
}
#endif
