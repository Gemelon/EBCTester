/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "debug_log_web.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "cJSON.h"
#include "debug_log.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"
#include "web_interface.h"

extern const unsigned char debug_html_start[] asm("_binary_debug_html_start");
extern const unsigned char debug_html_end[] asm("_binary_debug_html_end");
extern const unsigned char debug_js_start[] asm("_binary_debug_js_start");
extern const unsigned char debug_js_end[] asm("_binary_debug_js_end");
extern const unsigned char debug_settings_html_start[] asm("_binary_debug_settings_html_start");
extern const unsigned char debug_settings_html_end[] asm("_binary_debug_settings_html_end");
extern const unsigned char debug_settings_js_start[] asm("_binary_debug_settings_js_start");
extern const unsigned char debug_settings_js_end[] asm("_binary_debug_settings_js_end");

namespace {

constexpr char TAG[] = "debug_log_web";
constexpr char NVS_NAMESPACE[] = "debug_log";
constexpr char NVS_KEY_ENABLED[] = "enabled";
constexpr size_t MAX_BODY = 128;
constexpr size_t MAX_LINES_PER_REQUEST = 120;

std::mutex settings_mutex;
bool settings_enabled;
bool initialized;

void response_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

esp_err_t send_text(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    response_headers(req);
    return httpd_resp_sendstr(req, message);
}

bool load_enabled()
{
    std::lock_guard<std::mutex> lock(settings_mutex);
    return settings_enabled;
}

esp_err_t read_persisted_enabled(bool &enabled)
{
    enabled = false;
    nvs_handle_t handle;
    const esp_err_t open_err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (open_err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (open_err != ESP_OK) {
        return open_err;
    }
    uint8_t value = 0;
    esp_err_t err = nvs_get_u8(handle, NVS_KEY_ENABLED, &value);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    enabled = value != 0;
    return err;
}

esp_err_t write_persisted_enabled(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, NVS_KEY_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/* Appends text as a JSON string body, dropping malformed UTF-8 so that the
 * browser can always parse the response. */
void append_json_string(std::string &out, const char *text)
{
    const unsigned char *cursor = reinterpret_cast<const unsigned char *>(text);
    while (*cursor != '\0') {
        const unsigned char ch = *cursor;
        if (ch < 0x80) {
            switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default:
                if (ch < 0x20) {
                    char escape[7];
                    std::snprintf(escape, sizeof(escape), "\\u%04x", ch);
                    out += escape;
                } else {
                    out += static_cast<char>(ch);
                }
                break;
            }
            ++cursor;
            continue;
        }
        size_t extra = 0;
        if ((ch & 0xe0) == 0xc0) extra = 1;
        else if ((ch & 0xf0) == 0xe0) extra = 2;
        else if ((ch & 0xf8) == 0xf0) extra = 3;
        bool valid = extra != 0;
        for (size_t i = 1; valid && i <= extra; ++i) {
            valid = (cursor[i] & 0xc0) == 0x80;
        }
        if (!valid) {
            out += '?';
            ++cursor;
            continue;
        }
        out.append(reinterpret_cast<const char *>(cursor), extra + 1);
        cursor += extra + 1;
    }
}

uint32_t query_number(httpd_req_t *req, const char *key, uint32_t fallback)
{
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return fallback;
    }
    char value[24];
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return fallback;
    }
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || parsed > UINT32_MAX) {
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

void append_cursor(std::string &out)
{
    uint32_t first_seq = 0;
    uint32_t next_seq = 0;
    size_t count = 0;
    debug_log_get_cursor(&first_seq, &next_seq, &count);
    char numbers[208];
    std::snprintf(numbers, sizeof(numbers),
                  "\"enabled\":%s,\"capacity\":%d,\"count\":%u,\"firstSeq\":%u,\"nextSeq\":%u,"
                  "\"dropped\":%u",
                  debug_log_is_enabled() ? "true" : "false", DEBUG_LOG_LINE_COUNT,
                  static_cast<unsigned>(count), static_cast<unsigned>(first_seq),
                  static_cast<unsigned>(next_seq),
                  static_cast<unsigned>(debug_log_get_dropped()));
    out += numbers;
}

esp_err_t send_json(httpd_req_t *req, const std::string &json)
{
    httpd_resp_set_type(req, "application/json");
    response_headers(req);
    return httpd_resp_send(req, json.c_str(), json.size());
}

esp_err_t status_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    std::string json = "{";
    append_cursor(json);
    json += ",\"setting\":";
    json += load_enabled() ? "true" : "false";
    json += '}';
    return send_json(req, json);
}

esp_err_t log_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    if (!debug_log_is_enabled()) {
        return send_json(req, "{\"enabled\":false,\"capacity\":0,\"count\":0,"
                              "\"firstSeq\":0,\"nextSeq\":0,\"lines\":[]}");
    }
    const uint32_t after = query_number(req, "after", 0);
    uint32_t limit = query_number(req, "max", MAX_LINES_PER_REQUEST);
    if (limit == 0 || limit > MAX_LINES_PER_REQUEST) {
        limit = MAX_LINES_PER_REQUEST;
    }
    std::string json = "{";
    append_cursor(json);
    json += ",\"lines\":[";
    char text[DEBUG_LOG_LINE_MAX + 1];
    uint32_t seq = 0;
    uint32_t wanted = after + 1;
    for (uint32_t sent = 0; sent < limit; ++sent) {
        if (!debug_log_read_line(wanted, &seq, text, sizeof(text))) {
            break;
        }
        if (sent != 0) {
            json += ',';
        }
        char prefix[32];
        std::snprintf(prefix, sizeof(prefix), "{\"seq\":%u,\"text\":\"", static_cast<unsigned>(seq));
        json += prefix;
        append_json_string(json, text);
        json += "\"}";
        wanted = seq + 1;
    }
    json += "]}";
    return send_json(req, json);
}

esp_err_t read_body(httpd_req_t *req, char *buffer, size_t capacity)
{
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t received = 0;
    while (received < static_cast<size_t>(req->content_len)) {
        const int chunk = httpd_req_recv(req, buffer + received,
                                         static_cast<size_t>(req->content_len) - received);
        if (chunk <= 0) {
            return ESP_FAIL;
        }
        received += static_cast<size_t>(chunk);
    }
    buffer[received] = '\0';
    return ESP_OK;
}

esp_err_t settings_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, true)) {
        return ESP_OK;
    }
    char body[MAX_BODY];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return send_text(req, "400 Bad Request", "Invalid request body.");
    }
    cJSON *root = cJSON_Parse(body);
    const cJSON *flag = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (!cJSON_IsBool(flag)) {
        cJSON_Delete(root);
        return send_text(req, "400 Bad Request", "Expected a boolean \"enabled\" flag.");
    }
    const bool enabled = cJSON_IsTrue(flag);
    cJSON_Delete(root);

    const esp_err_t nvs_err = write_persisted_enabled(enabled);
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot save debug log setting: %s", esp_err_to_name(nvs_err));
        return send_text(req, "500 Internal Server Error", "Could not save the debug log setting.");
    }
    {
        std::lock_guard<std::mutex> lock(settings_mutex);
        settings_enabled = enabled;
    }
    const esp_err_t apply_err = debug_log_set_enabled(enabled);
    if (apply_err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot apply debug log setting: %s", esp_err_to_name(apply_err));
        return send_text(req, "500 Internal Server Error",
                         "Setting saved, but the log buffer could not be allocated.");
    }
    return send_text(req, "200 OK",
                     enabled ? "Debug log enabled; the Debug tab is now available."
                             : "Debug log disabled and buffer released.");
}

esp_err_t clear_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, true)) {
        return ESP_OK;
    }
    debug_log_clear();
    uint32_t next_seq = 0;
    debug_log_get_cursor(nullptr, &next_seq, nullptr);
    char json[48];
    std::snprintf(json, sizeof(json), "{\"nextSeq\":%u}", static_cast<unsigned>(next_seq));
    return send_json(req, json);
}

bool debug_tab_visible()
{
    return load_enabled() && debug_log_is_enabled();
}

const httpd_uri_t tab_routes[] = {
    {.uri = "/api/debug/log", .method = HTTP_GET, .handler = log_handler, .user_ctx = nullptr},
    {.uri = "/api/debug/clear", .method = HTTP_POST, .handler = clear_handler, .user_ctx = nullptr},
};

const httpd_uri_t settings_routes[] = {
    {.uri = "/api/debug/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = nullptr},
    {.uri = "/api/debug/settings", .method = HTTP_POST, .handler = settings_handler, .user_ctx = nullptr},
};

web_interface_extension_t tab_extension = {};
web_interface_extension_t settings_extension = {};

} // namespace

extern "C" esp_err_t debug_log_web_init(void)
{
    settings_extension.id = "debug-settings";
    settings_extension.label = "Debug log";
    settings_extension.content = debug_settings_html_start;
    settings_extension.content_size =
        static_cast<size_t>(debug_settings_html_end - debug_settings_html_start);
    settings_extension.script = debug_settings_js_start;
    settings_extension.script_size =
        static_cast<size_t>(debug_settings_js_end - debug_settings_js_start);
    settings_extension.routes = settings_routes;
    settings_extension.route_count = sizeof(settings_routes) / sizeof(settings_routes[0]);
    settings_extension.placement = "settings";

    tab_extension.id = "debug";
    tab_extension.label = "Debug";
    tab_extension.content = debug_html_start;
    tab_extension.content_size = static_cast<size_t>(debug_html_end - debug_html_start);
    tab_extension.script = debug_js_start;
    tab_extension.script_size = static_cast<size_t>(debug_js_end - debug_js_start);
    tab_extension.routes = tab_routes;
    tab_extension.route_count = sizeof(tab_routes) / sizeof(tab_routes[0]);
    tab_extension.placement = "tab";
    tab_extension.visible = debug_tab_visible;

    esp_err_t err = web_interface_register_extension(&settings_extension);
    if (err == ESP_OK) {
        err = web_interface_register_extension(&tab_extension);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot register debug log UI: %s", esp_err_to_name(err));
        return err;
    }
    initialized = true;
    return ESP_OK;
}

extern "C" esp_err_t debug_log_web_start(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    bool enabled = false;
    const esp_err_t err = read_persisted_enabled(enabled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read debug log setting: %s", esp_err_to_name(err));
    }
    {
        std::lock_guard<std::mutex> lock(settings_mutex);
        settings_enabled = enabled;
    }
    return debug_log_set_enabled(enabled);
}
