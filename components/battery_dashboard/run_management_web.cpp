/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "run_management.h"
#include "battery_database.h"
#include "test_store.h"
#include "web_interface.h"
#include "cJSON.h"
#include "esp_log.h"
#include <cstring>
#include <memory>

extern const unsigned char runs_html_start[] asm("_binary_test_runs_html_start");
extern const unsigned char runs_html_end[] asm("_binary_test_runs_html_end");
extern const unsigned char runs_js_start[] asm("_binary_test_runs_js_start");
extern const unsigned char runs_js_end[] asm("_binary_test_runs_js_end");

namespace {
constexpr const char *TAG = "test_runs";
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
constexpr TickType_t TIMEOUT = pdMS_TO_TICKS(10000);

esp_err_t reply(httpd_req_t *req, const char *status, const char *message)
{
    if (strcmp(status, "200 OK") != 0) ESP_LOGW(TAG, "%s %s: %s", status, req->uri, message);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message);
}

esp_err_t database_error(httpd_req_t *req, esp_err_t err)
{
    return reply(req, err == ESP_ERR_INVALID_STATE || err == ESP_ERR_TIMEOUT ?
                 "503 Service Unavailable" : "500 Internal Server Error",
                 err == ESP_ERR_INVALID_STATE ? "Database not ready. Check the SD card." :
                 err == ESP_ERR_TIMEOUT ? "SD card is busy. Please try again." :
                 "Test run operation failed. See the device log.");
}

int hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 :
           c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool parameter(const char *query, const char *key, std::string &value)
{
    char encoded[1201];
    const esp_err_t err = httpd_query_key_value(query, key, encoded, sizeof(encoded));
    if (err == ESP_ERR_NOT_FOUND) { value.clear(); return true; }
    if (err != ESP_OK) return false;
    value.clear();
    for (size_t i = 0; encoded[i]; ++i) {
        char c = encoded[i];
        if (c == '%') {
            if (!encoded[i + 1] || !encoded[i + 2]) return false;
            const int high = hex(encoded[i + 1]), low = hex(encoded[i + 2]);
            if (high < 0 || low < 0) return false;
            c = static_cast<char>(high * 16 + low);
            i += 2;
        } else if (c == '+') c = ' ';
        if (c == '\0') return false;
        value += c;
    }
    return true;
}

struct ListContext {
    run_management::Filter filter;
    run_management::Catalog catalog;
};
esp_err_t list_operation(sqlite3 *db, void *context)
{
    auto &args = *static_cast<ListContext *>(context);
    const int result = run_management::list(db, args.filter, args.catalog);
    if (result == SQLITE_OK) return ESP_OK;
    ESP_LOGE(TAG, "Run list failed (%d): %s", result, sqlite3_errmsg(db));
    return ESP_FAIL;
}

esp_err_t list_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) return ESP_OK;
    ListContext args;
    char query[1601] = {};
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return reply(req, "400 Bad Request", "Invalid test run filters.");
    std::string page;
    if (!parameter(query, "battery", args.filter.battery) || !parameter(query, "status", args.filter.status) ||
        !parameter(query, "from", args.filter.from) || !parameter(query, "until", args.filter.until) ||
        !parameter(query, "search", args.filter.search) || !parameter(query, "page", page))
        return reply(req, "400 Bad Request", "Invalid test run filters.");
    if (!page.empty()) {
        if (page.size() > 7) return reply(req, "400 Bad Request", "Invalid page number.");
        for (char c : page) {
            if (c < '0' || c > '9') return reply(req, "400 Bad Request", "Invalid page number.");
            args.filter.page = args.filter.page * 10 + c - '0';
        }
    }
    if (!run_management::valid_filter(args.filter))
        return reply(req, "400 Bad Request", "Invalid test run filters.");
    const esp_err_t err = battery_database_execute(list_operation, &args, TIMEOUT);
    if (err != ESP_OK) return database_error(req, err);
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root) return database_error(req, ESP_ERR_NO_MEM);
    auto *runs = cJSON_AddArrayToObject(root.get(), "runs");
    auto *batteries = cJSON_AddArrayToObject(root.get(), "batteries");
    const auto recorder = test_store::recorder_status();
    bool ok = runs && batteries && cJSON_AddNumberToObject(root.get(), "total", args.catalog.total) &&
        cJSON_AddNumberToObject(root.get(), "page", args.filter.page) &&
        cJSON_AddNumberToObject(root.get(), "pageSize", run_management::PAGE_SIZE);
    for (const auto &run : args.catalog.runs) {
        Json row(cJSON_CreateObject(), cJSON_Delete);
        ok = ok && row && cJSON_AddStringToObject(row.get(), "id", run.id.c_str()) &&
            cJSON_AddStringToObject(row.get(), "name", run.name.c_str()) &&
            cJSON_AddStringToObject(row.get(), "battery", run.battery.c_str()) &&
            cJSON_AddStringToObject(row.get(), "started", run.started.c_str()) &&
            cJSON_AddStringToObject(row.get(), "plan", run.plan.c_str()) &&
            cJSON_AddStringToObject(row.get(), "status", run.status.c_str()) &&
            cJSON_AddNumberToObject(row.get(), "batteryId", static_cast<double>(run.battery_id)) &&
            cJSON_AddBoolToObject(row.get(), "protected", run.status == "RUNNING" ||
                                  (recorder.active && recorder.run_id == run.id));
        if (!ok || !cJSON_AddItemToArray(runs, row.get())) { ok = false; break; }
        row.release();
    }
    for (const auto &battery : args.catalog.batteries) {
        Json row(cJSON_CreateObject(), cJSON_Delete);
        ok = ok && row && cJSON_AddNumberToObject(row.get(), "id", static_cast<double>(battery.id)) &&
             cJSON_AddStringToObject(row.get(), "label", battery.label.c_str());
        if (!ok || !cJSON_AddItemToArray(batteries, row.get())) { ok = false; break; }
        row.release();
    }
    std::unique_ptr<char, decltype(&cJSON_free)> body(ok ? cJSON_PrintUnformatted(root.get()) : nullptr, cJSON_free);
    if (!body) return database_error(req, ESP_ERR_NO_MEM);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body.get());
}

struct ChangeContext {
    std::string id, name, active_id;
    bool rename = false;
    int result = SQLITE_OK;
};
esp_err_t change_operation(sqlite3 *db, void *context)
{
    auto &args = *static_cast<ChangeContext *>(context);
    args.result = run_management::change(db, args.id, args.rename ? &args.name : nullptr, args.active_id);
    if (args.result == SQLITE_OK || args.result == run_management::PROTECTED || args.result == SQLITE_NOTFOUND) return ESP_OK;
    ESP_LOGE(TAG, "Run change failed (%d): %s", args.result, sqlite3_errmsg(db));
    return ESP_FAIL;
}

esp_err_t change_handler(httpd_req_t *req, bool rename)
{
    if (!web_interface_authorize_settings_request(req)) return ESP_OK;
    if (req->content_len < 1 || req->content_len > 2048)
        return reply(req, "400 Bad Request", "Invalid test run request.");
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int count = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (count <= 0) return reply(req, "400 Bad Request", "Incomplete test run request.");
        received += static_cast<size_t>(count);
    }
    bool null_escape = false;
    for (size_t i = 0; i + 1 < body.size(); ++i) {
        if (body[i] == '\\') {
            if (body.compare(i, 6, "\\u0000") == 0) null_escape = true;
            ++i;
        }
    }
    if (body.find('\0') != std::string::npos || null_escape)
        return reply(req, "400 Bad Request", "Null characters are not allowed.");
    Json root(cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, nullptr, true), cJSON_Delete);
    const cJSON *id = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "id") : nullptr;
    const cJSON *name = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "name") : nullptr;
    ChangeContext args;
    args.rename = rename;
    if (!cJSON_IsObject(root.get()) || !cJSON_IsString(id) || !id->valuestring ||
        cJSON_GetArraySize(root.get()) != (rename ? 2 : 1) ||
        (args.rename && (!cJSON_IsString(name) || !name->valuestring)))
        return reply(req, "400 Bad Request", "Invalid test run request.");
    args.id = id->valuestring;
    if (args.id.empty() || args.id.size() > 63) return reply(req, "400 Bad Request", "Invalid test run ID.");
    for (char c : args.id) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.' || c == ':'))
            return reply(req, "400 Bad Request", "Invalid test run ID.");
    }
    if (args.rename) {
        args.name = name->valuestring;
        const size_t first = args.name.find_first_not_of(" \t\r\n");
        args.name = first == std::string::npos ? "" :
            args.name.substr(first, args.name.find_last_not_of(" \t\r\n") - first + 1);
        if (!run_management::valid_name(args.name))
            return reply(req, "400 Bad Request", "Name must contain at most 100 characters without control characters.");
    }
    const auto recorder = test_store::recorder_status();
    if (recorder.active) args.active_id = recorder.run_id;
    const esp_err_t err = battery_database_execute(change_operation, &args, TIMEOUT);
    if (err != ESP_OK) return database_error(req, err);
    if (args.result == run_management::PROTECTED) return reply(req, "409 Conflict", "Running test runs cannot be renamed or deleted.");
    if (args.result == SQLITE_NOTFOUND) return reply(req, "404 Not Found", "Test run not found.");
    ESP_LOGI(TAG, "%s test run %s", args.rename ? "Renamed" : "Deleted", args.id.c_str());
    return reply(req, "200 OK", args.rename ? "Test run name saved." :
                 "Test run deleted from the SD card. Existing MariaDB backups are unchanged.");
}

esp_err_t rename_handler(httpd_req_t *req) { return change_handler(req, true); }
esp_err_t delete_handler(httpd_req_t *req) { return change_handler(req, false); }
const httpd_uri_t routes[] = {
    {.uri = "/api/test-runs", .method = HTTP_GET, .handler = list_handler, .user_ctx = nullptr},
    {.uri = "/api/test-runs/rename", .method = HTTP_POST, .handler = rename_handler, .user_ctx = nullptr},
    {.uri = "/api/test-runs/delete", .method = HTTP_POST, .handler = delete_handler, .user_ctx = nullptr},
};
web_interface_extension_t extension = {};
}

esp_err_t run_management_init()
{
    extension.id = "test-runs";
    extension.label = "Test run management";
    extension.content = runs_html_start;
    extension.content_size = runs_html_end - runs_html_start;
    extension.script = runs_js_start;
    extension.script_size = runs_js_end - runs_js_start;
    extension.routes = routes;
    extension.route_count = sizeof(routes) / sizeof(routes[0]);
    const esp_err_t err = web_interface_register_extension(&extension);
    if (err != ESP_OK) ESP_LOGE(TAG, "Cannot register test run management: %s", esp_err_to_name(err));
    return err;
}
