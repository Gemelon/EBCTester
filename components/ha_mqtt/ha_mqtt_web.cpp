/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ha_mqtt_web.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "ha_mqtt.h"
#include "nvs.h"
#include "test_control.h"
#include "web_interface.h"

extern const unsigned char mqtt_html_start[] asm("_binary_mqtt_html_start");
extern const unsigned char mqtt_html_end[] asm("_binary_mqtt_html_end");
extern const unsigned char mqtt_js_start[] asm("_binary_mqtt_js_start");
extern const unsigned char mqtt_js_end[] asm("_binary_mqtt_js_end");

namespace {

constexpr char TAG[] = "ha_mqtt_web";
constexpr char NVS_NAMESPACE[] = "ha_mqtt";
constexpr size_t MAX_BODY = 1024;

std::mutex settings_mutex;
std::mutex client_mutex;
ha_mqtt_settings_t saved_settings;
bool settings_loaded;
bool initialized;

esp_err_t read_nvs_string(nvs_handle_t handle, const char *key, char *value, size_t capacity)
{
    size_t length = capacity;
    const esp_err_t err = nvs_get_str(handle, key, value, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

template <typename T, typename Getter>
esp_err_t read_nvs_value(nvs_handle_t handle, const char *key, T *value, Getter getter)
{
    const esp_err_t err = getter(handle, key, value);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t load_settings_locked()
{
    if (settings_loaded) {
        return ESP_OK;
    }
    ha_mqtt_settings_defaults(&saved_settings);

    nvs_handle_t handle;
    const esp_err_t open_err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (open_err == ESP_ERR_NVS_NOT_FOUND) {
        settings_loaded = true;
        return ESP_OK;
    }
    if (open_err != ESP_OK) {
        return open_err;
    }

    uint8_t enabled = 0;
    uint8_t remote_start = 0;
    uint16_t port = saved_settings.port;
    uint32_t interval = saved_settings.measurement_interval_seconds;
    esp_err_t err = read_nvs_value(handle, "enabled", &enabled, nvs_get_u8);
    if (err == ESP_OK) err = read_nvs_value(handle, "remote", &remote_start, nvs_get_u8);
    if (err == ESP_OK) err = read_nvs_value(handle, "port", &port, nvs_get_u16);
    if (err == ESP_OK) err = read_nvs_value(handle, "interval", &interval, nvs_get_u32);
    if (err == ESP_OK) err = read_nvs_string(handle, "host", saved_settings.host,
                                              sizeof(saved_settings.host));
    if (err == ESP_OK) err = read_nvs_string(handle, "user", saved_settings.username,
                                              sizeof(saved_settings.username));
    if (err == ESP_OK) err = read_nvs_string(handle, "password", saved_settings.password,
                                              sizeof(saved_settings.password));
    if (err == ESP_OK) err = read_nvs_string(handle, "prefix", saved_settings.topic_prefix,
                                              sizeof(saved_settings.topic_prefix));
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read MQTT settings: %s", esp_err_to_name(err));
        return err;
    }

    saved_settings.enabled = enabled != 0;
    saved_settings.allow_remote_start = remote_start != 0;
    saved_settings.port = port;
    saved_settings.measurement_interval_seconds = interval;
    saved_settings.command_callback = test_control_handle_mqtt_command;
    settings_loaded = true;
    return ESP_OK;
}

esp_err_t get_settings(ha_mqtt_settings_t &settings)
{
    std::lock_guard<std::mutex> lock(settings_mutex);
    const esp_err_t err = load_settings_locked();
    if (err != ESP_OK) {
        return err;
    }
    settings = saved_settings;
    return ESP_OK;
}

bool valid_string(const char *value, size_t capacity, bool allow_empty)
{
    const char *end = static_cast<const char *>(std::memchr(value, '\0', capacity));
    if (end == nullptr || (!allow_empty && end == value)) {
        return false;
    }
    for (const unsigned char ch : std::string(value)) {
        if (ch < 0x20 || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

bool read_body(httpd_req_t *req, std::string &body)
{
    if (req->content_len > MAX_BODY) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "Request too large.");
        return false;
    }
    body.resize(req->content_len);
    size_t received = 0;
    while (received < body.size()) {
        const int result = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (result <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "Incomplete request.");
            return false;
        }
        received += static_cast<size_t>(result);
    }
    return true;
}

esp_err_t send_text(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message);
}

bool save_settings(const ha_mqtt_settings_t &settings)
{
    std::lock_guard<std::mutex> lock(settings_mutex);
    const esp_err_t load_err = load_settings_locked();
    if (load_err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot load MQTT settings: %s", esp_err_to_name(load_err));
        return false;
    }

    ha_mqtt_settings_t updated = settings;
    if (updated.password[0] == '\0') {
        std::memcpy(updated.password, saved_settings.password, sizeof(updated.password));
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open MQTT settings: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_u8(handle, "enabled", updated.enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(handle, "remote", updated.allow_remote_start ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u16(handle, "port", updated.port);
    if (err == ESP_OK) err = nvs_set_u32(handle, "interval", updated.measurement_interval_seconds);
    if (err == ESP_OK) err = nvs_set_str(handle, "host", updated.host);
    if (err == ESP_OK) err = nvs_set_str(handle, "user", updated.username);
    if (err == ESP_OK) err = nvs_set_str(handle, "password", updated.password);
    if (err == ESP_OK) err = nvs_set_str(handle, "prefix", updated.topic_prefix);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot save MQTT settings: %s", esp_err_to_name(err));
        return false;
    }
    saved_settings = updated;
    return true;
}

esp_err_t apply_settings(const ha_mqtt_settings_t &settings)
{
    std::lock_guard<std::mutex> lock(client_mutex);
    esp_err_t err = ha_mqtt_stop();
    if (err != ESP_OK) {
        return err;
    }
    if (!settings.enabled || settings.host[0] == '\0') {
        return ESP_OK;
    }
    ha_mqtt_settings_t active = settings;
    active.command_callback = test_control_handle_mqtt_command;
    return ha_mqtt_start(&active);
}

esp_err_t status_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    ha_mqtt_settings_t settings;
    const esp_err_t settings_err = get_settings(settings);
    if (settings_err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot load MQTT settings: %s", esp_err_to_name(settings_err));
        return send_text(req, "500 Internal Server Error", "Could not load MQTT settings.");
    }
    ha_mqtt_status_t status = {};
    ha_mqtt_get_status(&status);
    if (!settings.enabled) {
        status.state = HA_MQTT_STATE_DISABLED;
        std::snprintf(status.message, sizeof(status.message), "MQTT is disabled in device settings");
    } else if (settings.host[0] == '\0') {
        status.state = HA_MQTT_STATE_NOT_CONFIGURED;
        std::snprintf(status.message, sizeof(status.message), "Enter a broker host and save settings");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *config = root != nullptr ? cJSON_AddObjectToObject(root, "settings") : nullptr;
    const bool ok = root != nullptr && config != nullptr &&
                    cJSON_AddBoolToObject(config, "enabled", settings.enabled) != nullptr &&
                    cJSON_AddStringToObject(config, "host", settings.host) != nullptr &&
                    cJSON_AddNumberToObject(config, "port", settings.port) != nullptr &&
                    cJSON_AddStringToObject(config, "username", settings.username) != nullptr &&
                    cJSON_AddBoolToObject(config, "passwordSet", settings.password[0] != '\0') != nullptr &&
                    cJSON_AddStringToObject(config, "topicPrefix", settings.topic_prefix) != nullptr &&
                    cJSON_AddNumberToObject(config, "intervalSeconds",
                                            settings.measurement_interval_seconds) != nullptr &&
                    cJSON_AddBoolToObject(config, "allowRemoteStart",
                                          settings.allow_remote_start) != nullptr &&
                    cJSON_AddStringToObject(root, "state",
                                            status.state == HA_MQTT_STATE_DISABLED ? "disabled" :
                                            status.state == HA_MQTT_STATE_NOT_CONFIGURED ? "not_configured" :
                                            status.state == HA_MQTT_STATE_CONNECTING ? "connecting" :
                                            status.state == HA_MQTT_STATE_CONNECTED ? "connected" :
                                            status.state == HA_MQTT_STATE_DISCONNECTED ? "disconnected" :
                                            "error") != nullptr &&
                    cJSON_AddStringToObject(root, "message", status.message) != nullptr;
    char *json = ok ? cJSON_PrintUnformatted(root) : nullptr;
    cJSON_Delete(root);
    if (json == nullptr) {
        return send_text(req, "500 Internal Server Error", "Could not create MQTT status.");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

esp_err_t settings_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, true)) {
        return ESP_OK;
    }
    std::string body;
    if (!read_body(req, body)) {
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (root == nullptr) {
        return send_text(req, "400 Bad Request", "Invalid JSON.");
    }

    ha_mqtt_settings_t settings;
    const esp_err_t settings_err = get_settings(settings);
    if (settings_err != ESP_OK) {
        return send_text(req, "500 Internal Server Error", "Could not load MQTT settings.");
    }
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "port");
    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(root, "intervalSeconds");
    const cJSON *remote_start = cJSON_GetObjectItemCaseSensitive(root, "allowRemoteStart");
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "host");
    const cJSON *username = cJSON_GetObjectItemCaseSensitive(root, "username");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    const cJSON *prefix = cJSON_GetObjectItemCaseSensitive(root, "topicPrefix");
    bool valid = cJSON_IsBool(enabled) && cJSON_IsNumber(port) &&
                 cJSON_IsNumber(interval) && cJSON_IsBool(remote_start) &&
                 cJSON_IsString(host) && cJSON_IsString(username) &&
                 cJSON_IsString(password) && cJSON_IsString(prefix);
    if (valid) {
        settings.enabled = cJSON_IsTrue(enabled);
        settings.allow_remote_start = cJSON_IsTrue(remote_start);
        const double port_value = port->valuedouble;
        const double interval_value = interval->valuedouble;
        valid = port_value >= 1 && port_value <= 65535 &&
                port_value == static_cast<uint16_t>(port_value) &&
                interval_value >= 1 && interval_value <= 3600 &&
                interval_value == static_cast<uint32_t>(interval_value) &&
                std::strlen(host->valuestring) < sizeof(settings.host) &&
                std::strlen(username->valuestring) < sizeof(settings.username) &&
                std::strlen(password->valuestring) < sizeof(settings.password) &&
                std::strlen(prefix->valuestring) < sizeof(settings.topic_prefix);
        if (valid) {
            settings.port = static_cast<uint16_t>(port_value);
            settings.measurement_interval_seconds = static_cast<uint32_t>(interval_value);
            std::snprintf(settings.host, sizeof(settings.host), "%s", host->valuestring);
            std::snprintf(settings.username, sizeof(settings.username), "%s", username->valuestring);
            std::snprintf(settings.password, sizeof(settings.password), "%s", password->valuestring);
            std::snprintf(settings.topic_prefix, sizeof(settings.topic_prefix), "%s", prefix->valuestring);
        }
    }
    cJSON_Delete(root);

    if (!valid || !valid_string(settings.host, sizeof(settings.host), !settings.enabled) ||
        !valid_string(settings.username, sizeof(settings.username), true) ||
        !valid_string(settings.password, sizeof(settings.password), true) ||
        !valid_string(settings.topic_prefix, sizeof(settings.topic_prefix), false) ||
        std::strchr(settings.topic_prefix, '+') != nullptr ||
        std::strchr(settings.topic_prefix, '#') != nullptr ||
        std::strchr(settings.host, '/') != nullptr ||
        std::strstr(settings.host, "://") != nullptr) {
        return send_text(req, "400 Bad Request", "Invalid MQTT settings (check host, port, lengths and topic prefix).");
    }
    if (!save_settings(settings)) {
        return send_text(req, "500 Internal Server Error", "Could not save MQTT settings.");
    }
    const esp_err_t apply_err = apply_settings(settings);
    if (apply_err != ESP_OK) {
        ESP_LOGE(TAG, "Settings saved, but MQTT could not be started: %s",
                 esp_err_to_name(apply_err));
        return send_text(req, "500 Internal Server Error",
                         "Settings saved, but the MQTT client could not be started.");
    }
    return send_text(req, "200 OK", "MQTT settings saved and applied.");
}

esp_err_t test_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, true)) {
        return ESP_OK;
    }
    ha_mqtt_settings_t settings;
    const esp_err_t settings_err = get_settings(settings);
    if (settings_err != ESP_OK) {
        return send_text(req, "500 Internal Server Error", "Could not load MQTT settings.");
    }
    if (!settings.enabled) {
        return send_text(req, "409 Conflict", "Enable MQTT before testing the connection.");
    }
    if (settings.host[0] == '\0') {
        return send_text(req, "400 Bad Request", "Enter an MQTT broker host first.");
    }
    settings.command_callback = test_control_handle_mqtt_command;
    const esp_err_t err = apply_settings(settings);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot run MQTT connection test: %s", esp_err_to_name(err));
        return send_text(req, "500 Internal Server Error", "Could not start MQTT connection test.");
    }
    return send_text(req, "202 Accepted", "Connection attempt started; status updates below.");
}

const httpd_uri_t routes[] = {
    {.uri = "/api/mqtt/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = nullptr},
    {.uri = "/api/mqtt/settings", .method = HTTP_POST, .handler = settings_handler, .user_ctx = nullptr},
    {.uri = "/api/mqtt/test", .method = HTTP_POST, .handler = test_handler, .user_ctx = nullptr},
};

web_interface_extension_t extension = {};

} // namespace

extern "C" esp_err_t ha_mqtt_web_init(void)
{
    extension.id = "mqtt";
    extension.label = "Home Assistant MQTT";
    extension.content = mqtt_html_start;
    extension.content_size = static_cast<size_t>(mqtt_html_end - mqtt_html_start);
    extension.script = mqtt_js_start;
    extension.script_size = static_cast<size_t>(mqtt_js_end - mqtt_js_start);
    extension.routes = routes;
    extension.route_count = sizeof(routes) / sizeof(routes[0]);
    extension.placement = "settings";
    const esp_err_t err = web_interface_register_extension(&extension);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot register MQTT settings UI: %s", esp_err_to_name(err));
        return err;
    }
    initialized = true;
    return ESP_OK;
}

extern "C" esp_err_t ha_mqtt_web_start(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ha_mqtt_settings_t settings;
    const esp_err_t err = get_settings(settings);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot load MQTT settings: %s", esp_err_to_name(err));
        return err;
    }
    if (!settings.enabled || settings.host[0] == '\0') {
        return ESP_OK;
    }
    settings.command_callback = test_control_handle_mqtt_command;
    const esp_err_t start_err = ha_mqtt_start(&settings);
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start MQTT with saved settings: %s", esp_err_to_name(start_err));
    }
    return start_err;
}
