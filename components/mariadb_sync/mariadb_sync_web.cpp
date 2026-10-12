/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstdlib>
#include <cstring>
#include <string>

#include "battery_database.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mariadb_sync_internal.h"
extern "C" {
#include "network_manager.h"
}
#include "web_interface.h"

extern const unsigned char mariadb_html_start[] asm("_binary_mariadb_html_start");
extern const unsigned char mariadb_html_end[] asm("_binary_mariadb_html_end");
extern const unsigned char mariadb_js_start[] asm("_binary_mariadb_js_start");
extern const unsigned char mariadb_js_end[] asm("_binary_mariadb_js_end");

namespace mariadb_sync {
namespace {

const char *TAG = "mariadb_web";
constexpr size_t MAX_BODY = 1024;

void common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

esp_err_t send_text(httpd_req_t *req, const char *status, const char *message)
{
    common_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message);
}

const char *state_name(State state)
{
    switch (state) {
    case State::Testing:
        return "testing";
    case State::Syncing:
        return "syncing";
    case State::Ok:
        return "ok";
    case State::Error:
        return "error";
    default:
        return "idle";
    }
}

esp_err_t status_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    const Settings settings = get_settings();
    const Status status = get_status();
    network_status_t network = {};
    network_manager_get_status(&network);

    cJSON *root = cJSON_CreateObject();
    cJSON *config = cJSON_AddObjectToObject(root, "settings");
    bool ok = root != nullptr && config != nullptr &&
              cJSON_AddBoolToObject(config, "enabled", settings.enabled) != nullptr &&
              cJSON_AddStringToObject(config, "host", settings.host.c_str()) != nullptr &&
              cJSON_AddNumberToObject(config, "port", settings.port) != nullptr &&
              cJSON_AddStringToObject(config, "user", settings.user.c_str()) != nullptr &&
              cJSON_AddStringToObject(config, "database", settings.database.c_str()) != nullptr &&
              cJSON_AddNumberToObject(config, "intervalMinutes", settings.interval_minutes) != nullptr &&
              cJSON_AddBoolToObject(config, "passwordSet", !settings.password.empty()) != nullptr &&
              cJSON_AddStringToObject(root, "state", state_name(status.state)) != nullptr &&
              cJSON_AddBoolToObject(root, "busy", status.busy) != nullptr &&
              cJSON_AddStringToObject(root, "message", status.message.c_str()) != nullptr &&
              cJSON_AddNumberToObject(root, "lastSuccess", static_cast<double>(status.last_success)) != nullptr &&
              cJSON_AddNumberToObject(root, "lastAttempt", static_cast<double>(status.last_attempt)) != nullptr &&
              cJSON_AddNumberToObject(root, "lastRuns", status.last_runs) != nullptr &&
              cJSON_AddNumberToObject(root, "lastMeasurements", status.last_measurements) != nullptr &&
              cJSON_AddNumberToObject(root, "pendingRuns", status.pending_runs) != nullptr &&
              cJSON_AddStringToObject(root, "serverVersion", status.server_version.c_str()) != nullptr &&
              cJSON_AddBoolToObject(root, "networkConnected", network.connected) != nullptr &&
              cJSON_AddBoolToObject(root, "databaseReady", battery_database_is_ready()) != nullptr;
    char *json = ok ? cJSON_PrintUnformatted(root) : nullptr;
    cJSON_Delete(root);
    if (json == nullptr) {
        return send_text(req, "500 Internal Server Error", "Could not create status.");
    }
    common_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

bool read_body(httpd_req_t *req, std::string &body)
{
    if (req->content_len > MAX_BODY) {
        send_text(req, "413 Payload Too Large", "Request too large.");
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
            send_text(req, "400 Bad Request", "Incomplete request.");
            return false;
        }
        received += static_cast<size_t>(result);
    }
    return true;
}

bool read_string(cJSON *root, const char *name, size_t max_length, std::string &out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || item->valuestring == nullptr || strlen(item->valuestring) > max_length) {
        return false;
    }
    out = item->valuestring;
    return true;
}

bool printable(const std::string &value)
{
    for (unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7F) {
            return false;
        }
    }
    return true;
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
    Settings settings;
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "port");
    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(root, "intervalMinutes");
    std::string password;
    bool valid = cJSON_IsBool(enabled) && cJSON_IsNumber(port) && cJSON_IsNumber(interval) &&
                 read_string(root, "host", 63, settings.host) && read_string(root, "user", 64, settings.user) &&
                 read_string(root, "database", 64, settings.database) &&
                 read_string(root, "password", 64, password);
    if (valid) {
        settings.enabled = cJSON_IsTrue(enabled);
        valid = port->valuedouble >= 1 && port->valuedouble <= 65535 && interval->valuedouble >= 0 &&
                interval->valuedouble <= 1440;
        settings.port = static_cast<uint16_t>(port->valuedouble);
        settings.interval_minutes = static_cast<uint16_t>(interval->valuedouble);
    }
    cJSON_Delete(root);
    if (!valid || settings.host.empty() || settings.user.empty() || settings.database.empty() ||
        !printable(settings.host) || !printable(settings.user) || !printable(settings.database) ||
        !printable(password)) {
        return send_text(req, "400 Bad Request",
                         "Invalid settings (server, user, database, port 1–65535, interval 0–1440).");
    }
    settings.password = password;
    esp_err_t err = save_settings(settings, password.empty());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot save settings: %s", esp_err_to_name(err));
        return send_text(req, "500 Internal Server Error", "Could not save settings.");
    }
    return send_text(req, "200 OK", "MariaDB settings saved.");
}

esp_err_t start_command(httpd_req_t *req, uint32_t command, const char *accepted)
{
    if (!web_interface_authorize_request(req, true)) {
        return ESP_OK;
    }
    if (request(command) != ESP_OK) {
        return send_text(req, "409 Conflict", "A MariaDB operation is already running.");
    }
    return send_text(req, "202 Accepted", accepted);
}

esp_err_t test_handler(httpd_req_t *req)
{
    return start_command(req, COMMAND_TEST, "Connection test started.");
}

esp_err_t sync_handler(httpd_req_t *req)
{
    return start_command(req, COMMAND_SYNC, "Synchronization started.");
}

esp_err_t full_sync_handler(httpd_req_t *req)
{
    return start_command(req, COMMAND_SYNC | COMMAND_FULL_SYNC, "Full transfer started.");
}

const httpd_uri_t routes[] = {
    {.uri = "/api/mariadb/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = nullptr},
    {.uri = "/api/mariadb/settings", .method = HTTP_POST, .handler = settings_handler, .user_ctx = nullptr},
    {.uri = "/api/mariadb/test", .method = HTTP_POST, .handler = test_handler, .user_ctx = nullptr},
    {.uri = "/api/mariadb/sync", .method = HTTP_POST, .handler = sync_handler, .user_ctx = nullptr},
    {.uri = "/api/mariadb/full-sync", .method = HTTP_POST, .handler = full_sync_handler, .user_ctx = nullptr},
};

web_interface_extension_t extension = {};

} // namespace

esp_err_t register_web_extension()
{
    extension.id = "mariadb";
    extension.label = "MariaDB backup";
    extension.content = mariadb_html_start;
    extension.content_size = static_cast<size_t>(mariadb_html_end - mariadb_html_start);
    extension.script = mariadb_js_start;
    extension.script_size = static_cast<size_t>(mariadb_js_end - mariadb_js_start);
    extension.routes = routes;
    extension.route_count = sizeof(routes) / sizeof(routes[0]);
    extension.placement = "settings";
    esp_err_t err = web_interface_register_extension(&extension);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot register MariaDB UI: %s", esp_err_to_name(err));
    }
    return err;
}

} // namespace mariadb_sync
