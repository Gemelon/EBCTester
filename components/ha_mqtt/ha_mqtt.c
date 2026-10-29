/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ha_mqtt.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#if CONFIG_EBCTESTER_HA_MQTT_ENABLE
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "network_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

static const char *TAG = "ha_mqtt";

#if CONFIG_EBCTESTER_HA_MQTT_ENABLE

#define INITIAL_RETRY_US (1LL * 1000 * 1000)
#define MAX_RETRY_US (60LL * 1000 * 1000)
#define HA_STATUS_TOPIC "homeassistant/status"
#define HA_DISCOVERY_TOPIC_MAX 64
#define HA_STATE_TOPIC_MAX (HA_MQTT_TOPIC_PREFIX_MAX + sizeof("/state"))
#define HA_COMMAND_TOPIC_MAX (HA_MQTT_TOPIC_PREFIX_MAX + sizeof("/command"))
#define HA_COMMAND_PAYLOAD_MAX 256
#define HA_COMMAND_NAME_MAX 64
#define HA_COMMAND_QUEUE_LENGTH 8
#define HA_DEVICE_ID_MAX 23
#define HA_MEASUREMENT_EXPIRE_MULTIPLIER 3

static ha_mqtt_settings_t active_settings;
static esp_mqtt_client_handle_t mqtt_client;
static esp_timer_handle_t reconnect_timer;
static int64_t next_retry_us = INITIAL_RETRY_US;
static bool started;
static atomic_bool mqtt_client_started;
static atomic_bool mqtt_connected;
static atomic_bool station_connected;
static atomic_int mqtt_state;
static esp_event_handler_instance_t got_ip_handler;
static esp_event_handler_instance_t wifi_disconnected_handler;
static portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;
static char status_message[128] = "MQTT has not been configured";
static char availability_topic[HA_MQTT_TOPIC_PREFIX_MAX + sizeof("/availability")];
static char state_topic[HA_STATE_TOPIC_MAX];
static char command_topic[HA_COMMAND_TOPIC_MAX];
static char discovery_topic[HA_DISCOVERY_TOPIC_MAX];
static char device_id[HA_DEVICE_ID_MAX];
static QueueHandle_t command_queue;
static SemaphoreHandle_t command_task_done;
static TaskHandle_t command_task_handle;
static char live_measurement_payload[112];
static bool live_measurement_pending;
static uint16_t live_measurement_generation;

static void discard_live_measurement(void)
{
    portENTER_CRITICAL(&status_mux);
    live_measurement_pending = false;
    ++live_measurement_generation;
    portEXIT_CRITICAL(&status_mux);
}

static void publish_live_measurement(const esp_mqtt_event_t *event)
{
    char payload[sizeof(live_measurement_payload)];
    bool available = false;
    portENTER_CRITICAL(&status_mux);
    if (event != NULL && live_measurement_pending &&
        event->msg_id == live_measurement_generation) {
        memcpy(payload, live_measurement_payload, sizeof(payload));
        live_measurement_pending = false;
        available = true;
    }
    portEXIT_CRITICAL(&status_mux);
    if (available && atomic_load(&mqtt_connected) &&
        esp_mqtt_client_publish(mqtt_client, state_topic, payload, 0, 0, 0) < 0) {
        ESP_LOGD(TAG, "Skipped live measurement because MQTT publishing failed");
    }
}

typedef enum {
    HA_COMMAND_START_PLAN,
    HA_COMMAND_STOP,
    HA_COMMAND_ABORT,
    HA_COMMAND_SELECT_PLAN,
    HA_COMMAND_SELECT_BATTERY,
    HA_COMMAND_SHUTDOWN,
} ha_command_type_t;

typedef struct {
    ha_command_type_t type;
    char value[HA_COMMAND_NAME_MAX + 1];
} ha_command_t;

static const char *const command_names[] = {
    "start_plan",
    "stop",
    "abort",
    "select_plan",
    "select_battery",
};
static ha_mqtt_command_callback_t command_callback;

static void set_status(ha_mqtt_state_t state, const char *message)
{
    atomic_store(&mqtt_state, state);
    portENTER_CRITICAL(&status_mux);
    snprintf(status_message, sizeof(status_message), "%s", message);
    portEXIT_CRITICAL(&status_mux);
}

typedef struct {
    const char *key;
    const char *name;
    const char *value_template;
    const char *unit;
    const char *device_class;
    const char *state_class;
    bool measurement;
    bool binary_sensor;
} discovery_entity_t;

static const discovery_entity_t discovery_entities[] = {
    {"voltage", "Voltage", "{{ value_json.voltage }}", "V", "voltage", "measurement", true, false},
    {"current", "Current", "{{ value_json.current }}", "A", "current", "measurement", true, false},
    {"capacity", "Capacity", "{{ value_json.capacity }}", "Ah", NULL, "measurement", true, false},
    {"mode", "Mode", "{{ value_json.mode }}", NULL, NULL, NULL, false, false},
    {"plan_status", "Plan status", "{{ value_json.plan_status }}", NULL, NULL, NULL, false, false},
    {"connected", "Connected", "{{ value_json.connected }}", NULL, NULL, NULL, false, true},
    {"assigned_battery", "Assigned battery", "{{ value_json.assigned_battery }}", NULL, NULL, NULL, false, false},
    {"test_capacity", "Test capacity", "{{ value_json.test_capacity }}", "Ah", NULL, "measurement", false, false},
    {"test_energy", "Test energy", "{{ value_json.test_energy }}", "Wh", "energy", "measurement", false, false},
    {"resistance_1s", "Internal resistance at 1 s", "{{ value_json.resistance_1s }}", "Ω", "resistance", "measurement", false, false},
    {"resistance_5s", "Internal resistance at 5 s", "{{ value_json.resistance_5s }}", "Ω", "resistance", "measurement", false, false},
    {"resistance_10s", "Internal resistance at 10 s", "{{ value_json.resistance_10s }}", "Ω", "resistance", "measurement", false, false},
    {"test_timestamp", "Test timestamp", "{{ value_json.test_timestamp }}", NULL, "timestamp", NULL, false, false},
};

static bool command_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    size_t length = 0;
    while (name[length] != '\0') {
        const unsigned char ch = (unsigned char)name[length];
        if (ch < 0x20 || ch == 0x7f) {
            return false;
        }
        if (++length > HA_COMMAND_NAME_MAX) {
            return false;
        }
    }
    return true;
}

static bool contains_escaped_null(const char *payload, size_t length)
{
    for (size_t i = 0; i + 5 < length; ++i) {
        if (payload[i] == '\\' && (payload[i + 1] == 'u' || payload[i + 1] == 'U') &&
            payload[i + 2] == '0' && payload[i + 3] == '0' &&
            payload[i + 4] == '0' && payload[i + 5] == '0') {
            return true;
        }
    }
    return false;
}

static bool parse_command(const char *payload, size_t length, ha_command_t *command)
{
    if (payload == NULL || command == NULL || length == 0 ||
        length > HA_COMMAND_PAYLOAD_MAX || memchr(payload, '\0', length) != NULL ||
        contains_escaped_null(payload, length)) {
        return false;
    }

    char json[HA_COMMAND_PAYLOAD_MAX + 1];
    memcpy(json, payload, length);
    json[length] = '\0';
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length + 1, &parse_end, false);
    if (root == NULL) {
        return false;
    }
    while (parse_end != NULL && *parse_end != '\0' &&
           (*parse_end == ' ' || *parse_end == '\t' || *parse_end == '\r' ||
            *parse_end == '\n')) {
        ++parse_end;
    }
    if (!cJSON_IsObject(root) || parse_end == NULL || *parse_end != '\0') {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *command_item = cJSON_GetObjectItemCaseSensitive(root, "command");
    bool valid = cJSON_IsString(command_item) && command_item->valuestring != NULL;
    ha_command_type_t type = HA_COMMAND_SHUTDOWN;
    if (valid) {
        valid = false;
        for (size_t i = 0; i < sizeof(command_names) / sizeof(command_names[0]); ++i) {
            if (strcmp(command_item->valuestring, command_names[i]) == 0) {
                type = (ha_command_type_t)i;
                valid = true;
                break;
            }
        }
    }

    const cJSON *value_item = cJSON_GetObjectItemCaseSensitive(root, "name");
    const bool selection = type == HA_COMMAND_SELECT_PLAN ||
                           type == HA_COMMAND_SELECT_BATTERY;
    if (valid && selection) {
        valid = cJSON_GetArraySize(root) == 2 && cJSON_IsString(value_item) &&
                command_name_valid(value_item->valuestring);
    } else if (valid) {
        valid = cJSON_GetArraySize(root) == 1 && value_item == NULL;
    }

    if (valid) {
        command->type = type;
        command->value[0] = '\0';
        if (selection) {
            snprintf(command->value, sizeof(command->value), "%s", value_item->valuestring);
        }
    }
    cJSON_Delete(root);
    return valid;
}

static void command_task(void *arg)
{
    (void)arg;
    ha_command_t command;
    while (xQueueReceive(command_queue, &command, portMAX_DELAY) == pdTRUE) {
        if (command.type == HA_COMMAND_SHUTDOWN) {
            break;
        }
        command_callback(command_names[command.type],
                         command.value[0] != '\0' ? command.value : NULL);
    }
    xSemaphoreGive(command_task_done);
    vTaskDelete(NULL);
}

static void stop_command_task(void)
{
    if (command_task_handle == NULL) {
        return;
    }
    xQueueReset(command_queue);
    const ha_command_t shutdown = {.type = HA_COMMAND_SHUTDOWN};
    if (xQueueSend(command_queue, &shutdown, portMAX_DELAY) == pdTRUE) {
        xSemaphoreTake(command_task_done, portMAX_DELAY);
    }
    command_task_handle = NULL;
    vQueueDelete(command_queue);
    command_queue = NULL;
    vSemaphoreDelete(command_task_done);
    command_task_done = NULL;
}

static bool queue_command(const ha_command_t *command)
{
    if (command_queue == NULL) {
        return false;
    }
    if (xQueueSend(command_queue, command, 0) == pdTRUE) {
        return true;
    }
    if (command->type == HA_COMMAND_STOP || command->type == HA_COMMAND_ABORT) {
        xQueueReset(command_queue);
        return xQueueSendToFront(command_queue, command, 0) == pdTRUE;
    }
    return false;
}

static bool add_discovery_entity(cJSON *components, const discovery_entity_t *entity,
                                 uint32_t expire_after)
{
    cJSON *component = cJSON_CreateObject();
    if (component == NULL) {
        return false;
    }

    char unique_id[HA_DEVICE_ID_MAX + 32];
    int length = snprintf(unique_id, sizeof(unique_id), "%s_%s", device_id, entity->key);
    if (length < 0 || (size_t)length >= sizeof(unique_id)) {
        cJSON_Delete(component);
        return false;
    }

    bool ok = cJSON_AddStringToObject(component, "p",
                                      entity->binary_sensor ? "binary_sensor" : "sensor") != NULL &&
              cJSON_AddStringToObject(component, "name", entity->name) != NULL &&
              cJSON_AddStringToObject(component, "unique_id", unique_id) != NULL &&
              cJSON_AddStringToObject(component, "state_topic", state_topic) != NULL &&
              cJSON_AddStringToObject(component, "value_template",
                                      entity->value_template) != NULL;
    if (ok && entity->unit != NULL) {
        ok = cJSON_AddStringToObject(component, "unit_of_measurement", entity->unit) != NULL;
    }
    if (ok && entity->device_class != NULL) {
        ok = cJSON_AddStringToObject(component, "device_class", entity->device_class) != NULL;
    }
    if (ok && entity->state_class != NULL) {
        ok = cJSON_AddStringToObject(component, "state_class", entity->state_class) != NULL;
    }
    if (ok && entity->binary_sensor) {
        ok = cJSON_AddStringToObject(component, "payload_on", "true") != NULL &&
             cJSON_AddStringToObject(component, "payload_off", "false") != NULL;
    }
    if (ok && entity->measurement) {
        ok = cJSON_AddNumberToObject(component, "expire_after", expire_after) != NULL;
    }
    if (!ok || !cJSON_AddItemToObject(components, entity->key, component)) {
        cJSON_Delete(component);
        return false;
    }
    return true;
}

static void get_configuration_url(char *url, size_t size)
{
    static const char *const interface_keys[] = {"WIFI_STA_DEF", "WIFI_AP_DEF"};
    for (size_t i = 0; i < sizeof(interface_keys) / sizeof(interface_keys[0]); ++i) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(interface_keys[i]);
        esp_netif_ip_info_t ip_info;
        if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            char address[16];
            if (esp_ip4addr_ntoa(&ip_info.ip, address, sizeof(address)) != NULL) {
                snprintf(url, size, "http://%s/", address);
                return;
            }
        }
    }
    snprintf(url, size, "http://192.168.4.1/");
}

static esp_err_t publish_discovery(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *device = cJSON_CreateObject();
    cJSON *identifiers = cJSON_CreateArray();
    cJSON *components = cJSON_CreateObject();
    if (root == NULL || device == NULL || identifiers == NULL || components == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(device);
        cJSON_Delete(identifiers);
        cJSON_Delete(components);
        return ESP_ERR_NO_MEM;
    }

    char configuration_url[32];
    get_configuration_url(configuration_url, sizeof(configuration_url));
    cJSON *origin = cJSON_AddObjectToObject(root, "o");
    bool ok = origin != NULL &&
              cJSON_AddStringToObject(origin, "name", "EBCTester") != NULL &&
              cJSON_AddStringToObject(root, "avty_t", availability_topic) != NULL &&
              cJSON_AddStringToObject(root, "pl_avail", "online") != NULL &&
              cJSON_AddStringToObject(root, "pl_not_avail", "offline") != NULL &&
              cJSON_AddStringToObject(device, "name", "EBCTester") != NULL &&
              cJSON_AddStringToObject(device, "mf", "Gemelon") != NULL &&
              cJSON_AddStringToObject(device, "mdl", "EBCTester battery tester") != NULL &&
              cJSON_AddStringToObject(device, "cu", configuration_url) != NULL &&
              cJSON_AddItemToArray(identifiers, cJSON_CreateString(device_id));
    if (ok) {
        ok = cJSON_AddItemToObject(device, "ids", identifiers);
        if (ok) {
            identifiers = NULL;
        }
    }
    if (ok) {
        ok = cJSON_AddItemToObject(root, "dev", device);
        if (ok) {
            device = NULL;
        }
    }

    uint32_t expire_after = active_settings.measurement_interval_seconds *
                            HA_MEASUREMENT_EXPIRE_MULTIPLIER;
    for (size_t i = 0; ok && i < sizeof(discovery_entities) / sizeof(discovery_entities[0]); ++i) {
        ok = add_discovery_entity(components, &discovery_entities[i], expire_after);
    }
    if (ok) {
        ok = cJSON_AddItemToObject(root, "cmps", components);
        if (ok) {
            components = NULL;
        }
    }

    char *payload = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    cJSON_Delete(device);
    cJSON_Delete(identifiers);
    cJSON_Delete(components);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int message_id = esp_mqtt_client_enqueue(mqtt_client, discovery_topic, payload, 0, 1, 1, false);
    cJSON_free(payload);
    if (message_id < 0) {
        ESP_LOGW(TAG, "Failed to queue Home Assistant discovery");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool settings_valid(const ha_mqtt_settings_t *settings)
{
    return memchr(settings->host, '\0', sizeof(settings->host)) != NULL &&
           memchr(settings->username, '\0', sizeof(settings->username)) != NULL &&
           memchr(settings->password, '\0', sizeof(settings->password)) != NULL &&
           memchr(settings->topic_prefix, '\0', sizeof(settings->topic_prefix)) != NULL &&
           settings->host[0] != '\0' &&
           settings->port != 0 &&
           settings->topic_prefix[0] != '\0' &&
           settings->measurement_interval_seconds != 0 &&
           settings->measurement_interval_seconds <=
               UINT32_MAX / HA_MEASUREMENT_EXPIRE_MULTIPLIER &&
           strchr(settings->topic_prefix, '+') == NULL &&
           strchr(settings->topic_prefix, '#') == NULL;
}

static void schedule_reconnect(void)
{
    if (!started || reconnect_timer == NULL) {
        return;
    }
    if (!atomic_load(&station_connected)) {
        set_status(HA_MQTT_STATE_DISCONNECTED, "Waiting for Wi-Fi");
        return;
    }

    esp_timer_stop(reconnect_timer);
    esp_err_t err = esp_timer_start_once(reconnect_timer, next_retry_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to schedule MQTT retry: %s", esp_err_to_name(err));
        return;
    }
    if (next_retry_us < MAX_RETRY_US) {
        next_retry_us *= 2;
        if (next_retry_us > MAX_RETRY_US) {
            next_retry_us = MAX_RETRY_US;
        }
    }
}

static void reconnect_timer_callback(void *arg)
{
    (void)arg;
    if (!started || mqtt_client == NULL) {
        return;
    }
    if (!atomic_load(&station_connected)) {
        set_status(HA_MQTT_STATE_DISCONNECTED, "Waiting for Wi-Fi");
        return;
    }

    esp_err_t err;
    if (!atomic_load(&mqtt_client_started)) {
        atomic_store(&mqtt_client_started, true);
        err = esp_mqtt_client_start(mqtt_client);
        if (err != ESP_OK) {
            atomic_store(&mqtt_client_started, false);
        }
    } else {
        err = esp_mqtt_client_reconnect(mqtt_client);
    }
    if (err != ESP_OK) {
        schedule_reconnect();
    }
}

static void network_event_handler(void *handler_args, esp_event_base_t base,
                                  int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)event_data;
    if (!started) {
        return;
    }

    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        atomic_store(&station_connected, true);
        next_retry_us = INITIAL_RETRY_US;
        set_status(HA_MQTT_STATE_CONNECTING, "Connecting to the MQTT broker");
        schedule_reconnect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&station_connected, false);
        atomic_store(&mqtt_connected, false);
        esp_timer_stop(reconnect_timer);
        set_status(HA_MQTT_STATE_DISCONNECTED, "Waiting for Wi-Fi");
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    if (!started) {
        return;
    }

    if (event_id == MQTT_EVENT_CONNECTED) {
        atomic_store(&mqtt_connected, true);
        set_status(HA_MQTT_STATE_CONNECTED, "Connected to the MQTT broker");
        next_retry_us = INITIAL_RETRY_US;
        esp_timer_stop(reconnect_timer);
        if (esp_mqtt_client_subscribe(mqtt_client, HA_STATUS_TOPIC, 1) < 0) {
            ESP_LOGW(TAG, "Failed to subscribe to Home Assistant status");
        }
        if (command_callback != NULL &&
            esp_mqtt_client_subscribe(mqtt_client, command_topic, 1) < 0) {
            ESP_LOGW(TAG, "Failed to subscribe to MQTT command topic");
        }
        if (esp_mqtt_client_enqueue(mqtt_client, availability_topic, "online", 0, 1, 1,
                                    false) < 0) {
            ESP_LOGW(TAG, "Failed to queue MQTT availability");
        }
        if (publish_discovery() != ESP_OK) {
            ESP_LOGW(TAG, "Failed to publish Home Assistant discovery");
        }
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        atomic_store(&mqtt_connected, false);
        discard_live_measurement();
        set_status(HA_MQTT_STATE_DISCONNECTED,
                   atomic_load(&station_connected)
                       ? "Disconnected; retry scheduled"
                       : "Waiting for Wi-Fi");
        schedule_reconnect();
    } else if (event_id == MQTT_EVENT_ERROR) {
        const bool online = atomic_load(&station_connected);
        set_status(online ? HA_MQTT_STATE_ERROR : HA_MQTT_STATE_DISCONNECTED,
                   online ? "Broker connection failed; retry scheduled"
                          : "Waiting for Wi-Fi");
    } else if (event_id == MQTT_USER_EVENT) {
        publish_live_measurement(event_data);
    } else if (event_id == MQTT_EVENT_DATA) {
        const esp_mqtt_event_handle_t event = event_data;
        if (event == NULL || event->current_data_offset != 0 ||
            event->data_len != event->total_data_len || event->topic_len < 0 ||
            event->data_len < 0 ||
            (event->topic_len > 0 && event->topic == NULL) ||
            (event->data_len > 0 && event->data == NULL)) {
            return;
        }
        if (event->topic_len == (int)(sizeof(HA_STATUS_TOPIC) - 1) &&
            memcmp(event->topic, HA_STATUS_TOPIC, sizeof(HA_STATUS_TOPIC) - 1) == 0 &&
            event->data_len == (int)(sizeof("online") - 1) &&
            memcmp(event->data, "online", sizeof("online") - 1) == 0) {
            if (publish_discovery() != ESP_OK) {
                ESP_LOGW(TAG, "Failed to republish Home Assistant discovery");
            }
        } else if (command_callback != NULL &&
                   event->topic_len == (int)strlen(command_topic) &&
                   memcmp(event->topic, command_topic, strlen(command_topic)) == 0) {
            ha_command_t command;
            if (!parse_command(event->data, (size_t)event->data_len, &command)) {
                ESP_LOGD(TAG, "Ignored malformed or unsupported MQTT command");
            } else if (command.type == HA_COMMAND_START_PLAN &&
                       !active_settings.allow_remote_start) {
                ESP_LOGD(TAG, "Ignored MQTT plan start because remote start is disabled");
            } else if (!queue_command(&command)) {
                ESP_LOGD(TAG, "Dropped MQTT command because the command queue is full");
            }
        }
    }
}

void ha_mqtt_settings_defaults(ha_mqtt_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    memset(settings, 0, sizeof(*settings));
    settings->enabled = CONFIG_EBCTESTER_HA_MQTT_ENABLE;
    settings->port = 1883;
    settings->measurement_interval_seconds = 20;
    snprintf(settings->topic_prefix, sizeof(settings->topic_prefix), "ebctester");
}

esp_err_t ha_mqtt_start(const ha_mqtt_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!settings->enabled) {
        return ESP_OK;
    }
    if (!settings_valid(settings)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (started) {
        return ESP_ERR_INVALID_STATE;
    }

    active_settings = *settings;
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(device_id, sizeof(device_id), "ebctester_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    int topic_length = snprintf(availability_topic, sizeof(availability_topic),
                                "%s/availability", active_settings.topic_prefix);
    int state_length = snprintf(state_topic, sizeof(state_topic), "%s/state",
                                active_settings.topic_prefix);
    int command_length = snprintf(command_topic, sizeof(command_topic), "%s/command",
                                  active_settings.topic_prefix);
    int discovery_length = snprintf(discovery_topic, sizeof(discovery_topic),
                                    "homeassistant/device/%s/config", device_id);
    if (topic_length < 0 || (size_t)topic_length >= sizeof(availability_topic) ||
        state_length < 0 || (size_t)state_length >= sizeof(state_topic) ||
        command_length < 0 || (size_t)command_length >= sizeof(command_topic) ||
        discovery_length < 0 || (size_t)discovery_length >= sizeof(discovery_topic)) {
        return ESP_ERR_INVALID_ARG;
    }

    command_callback = active_settings.command_callback;
    if (command_callback != NULL) {
        command_queue = xQueueCreate(HA_COMMAND_QUEUE_LENGTH, sizeof(ha_command_t));
        command_task_done = xSemaphoreCreateBinary();
        if (command_queue == NULL || command_task_done == NULL ||
            xTaskCreate(command_task, "ha_mqtt_cmd", 6144, NULL, 4,
                        &command_task_handle) != pdPASS) {
            if (command_queue != NULL) {
                vQueueDelete(command_queue);
                command_queue = NULL;
            }
            if (command_task_done != NULL) {
                vSemaphoreDelete(command_task_done);
                command_task_done = NULL;
            }
            command_callback = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_callback,
        .name = "ha_mqtt_retry",
    };
    err = esp_timer_create(&timer_args, &reconnect_timer);
    if (err != ESP_OK) {
        stop_command_task();
        command_callback = NULL;
        return err;
    }

    esp_mqtt_client_config_t client_config = {
        .broker.address.hostname = active_settings.host,
        .broker.address.port = active_settings.port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.username = active_settings.username[0] != '\0'
                                   ? active_settings.username
                                   : NULL,
        .credentials.authentication.password = active_settings.password[0] != '\0'
                                                   ? active_settings.password
                                                   : NULL,
        .session.last_will.topic = availability_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
        .network.disable_auto_reconnect = true,
    };

    mqtt_client = esp_mqtt_client_init(&client_config);
    if (mqtt_client == NULL) {
        esp_timer_delete(reconnect_timer);
        reconnect_timer = NULL;
        stop_command_task();
        command_callback = NULL;
        return ESP_ERR_NO_MEM;
    }

    err = esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                         mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        esp_timer_delete(reconnect_timer);
        reconnect_timer = NULL;
        stop_command_task();
        command_callback = NULL;
        return err;
    }

    started = true;
    atomic_store(&mqtt_client_started, false);
    atomic_store(&mqtt_connected, false);
    atomic_store(&station_connected, false);
    next_retry_us = INITIAL_RETRY_US;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              network_event_handler, NULL,
                                              &got_ip_handler);
    if (err != ESP_OK) {
        started = false;
        esp_mqtt_client_unregister_event(mqtt_client, ESP_EVENT_ANY_ID,
                                         mqtt_event_handler);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        esp_timer_delete(reconnect_timer);
        reconnect_timer = NULL;
        stop_command_task();
        command_callback = NULL;
        return err;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                              network_event_handler, NULL,
                                              &wifi_disconnected_handler);
    if (err != ESP_OK) {
        started = false;
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              got_ip_handler);
        esp_mqtt_client_unregister_event(mqtt_client, ESP_EVENT_ANY_ID,
                                         mqtt_event_handler);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        esp_timer_delete(reconnect_timer);
        reconnect_timer = NULL;
        stop_command_task();
        command_callback = NULL;
        return err;
    }

    network_status_t network;
    network_manager_get_status(&network);
    atomic_store(&station_connected, network.connected);
    if (network.connected) {
        set_status(HA_MQTT_STATE_CONNECTING, "Connecting to the MQTT broker");
        schedule_reconnect();
    } else {
        set_status(HA_MQTT_STATE_DISCONNECTED, "Waiting for Wi-Fi");
    }
    return ESP_OK;
}

esp_err_t ha_mqtt_stop(void)
{
    if (!started) {
        return ESP_OK;
    }

    started = false;
    atomic_store(&mqtt_connected, false);
    discard_live_measurement();
    atomic_store(&station_connected, false);
    set_status(HA_MQTT_STATE_DISABLED, "MQTT is disabled");
    esp_timer_stop(reconnect_timer);
    esp_err_t wifi_handler_err = esp_event_handler_instance_unregister(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_disconnected_handler);
    esp_err_t ip_handler_err = esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_handler);
    esp_err_t err = ESP_OK;
    if (atomic_load(&mqtt_client_started)) {
        err = esp_mqtt_client_stop(mqtt_client);
    }
    atomic_store(&mqtt_client_started, false);
    esp_err_t unregister_err = esp_mqtt_client_unregister_event(
        mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler);
    esp_err_t destroy_err = esp_mqtt_client_destroy(mqtt_client);
    mqtt_client = NULL;
    esp_err_t timer_err = esp_timer_delete(reconnect_timer);
    reconnect_timer = NULL;
    stop_command_task();
    command_callback = NULL;

    if (err != ESP_OK) {
        return err;
    }
    if (wifi_handler_err != ESP_OK) {
        return wifi_handler_err;
    }
    if (ip_handler_err != ESP_OK) {
        return ip_handler_err;
    }
    if (unregister_err != ESP_OK) {
        return unregister_err;
    }
    if (destroy_err != ESP_OK) {
        return destroy_err;
    }
    return timer_err;
}

esp_err_t ha_mqtt_get_status(ha_mqtt_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    status->state = (ha_mqtt_state_t)atomic_load(&mqtt_state);
    portENTER_CRITICAL(&status_mux);
    snprintf(status->message, sizeof(status->message), "%s", status_message);
    portEXIT_CRITICAL(&status_mux);
    return ESP_OK;
}

esp_err_t ha_mqtt_publish_measurement(const ha_mqtt_measurement_t *measurement)
{
    if (measurement == NULL || !isfinite(measurement->voltage_v) ||
        !isfinite(measurement->current_a) || !isfinite(measurement->capacity_ah)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!started || mqtt_client == NULL || !atomic_load(&mqtt_connected)) {
        return ESP_ERR_INVALID_STATE;
    }

    const double voltage = round((double)measurement->voltage_v * 1000.0) / 1000.0;
    const double current = round((double)measurement->current_a * 1000.0) / 1000.0;
    const double capacity = round((double)measurement->capacity_ah * 10000.0) / 10000.0;
    char payload[112];
    const int length = snprintf(payload, sizeof(payload),
                                "{\"voltage\":%.3f,\"current\":%.3f,\"capacity\":%.4f}",
                                voltage, current, capacity);
    if (length < 0 || (size_t)length >= sizeof(payload)) {
        return ESP_FAIL;
    }

    esp_mqtt_event_t event = {.event_id = MQTT_USER_EVENT};
    portENTER_CRITICAL(&status_mux);
    if (live_measurement_pending || !atomic_load(&mqtt_connected)) {
        portEXIT_CRITICAL(&status_mux);
        ESP_LOGD(TAG, "Skipped live measurement because MQTT is busy or disconnected");
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(live_measurement_payload, payload, (size_t)length + 1);
    live_measurement_pending = true;
    event.msg_id = live_measurement_generation;
    portEXIT_CRITICAL(&status_mux);

    const esp_err_t err = esp_mqtt_dispatch_custom_event(mqtt_client, &event);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&status_mux);
        if (event.msg_id == live_measurement_generation) {
            live_measurement_pending = false;
        }
        portEXIT_CRITICAL(&status_mux);
        ESP_LOGD(TAG, "Skipped live measurement because MQTT dispatch failed: %s",
                 esp_err_to_name(err));
    }
    return err;
}

#else

void ha_mqtt_settings_defaults(ha_mqtt_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }
    memset(settings, 0, sizeof(*settings));
    settings->port = 1883;
    settings->measurement_interval_seconds = 20;
    snprintf(settings->topic_prefix, sizeof(settings->topic_prefix), "ebctester");
}

esp_err_t ha_mqtt_start(const ha_mqtt_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (settings->enabled) {
        ESP_LOGE(TAG, "MQTT is disabled in the firmware configuration");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

esp_err_t ha_mqtt_stop(void)
{
    return ESP_OK;
}

esp_err_t ha_mqtt_get_status(ha_mqtt_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    status->state = HA_MQTT_STATE_DISABLED;
    snprintf(status->message, sizeof(status->message), "MQTT support is not included in this firmware");
    return ESP_OK;
}

esp_err_t ha_mqtt_publish_measurement(const ha_mqtt_measurement_t *measurement)
{
    return measurement == NULL ? ESP_ERR_INVALID_ARG : ESP_ERR_NOT_SUPPORTED;
}

#endif
