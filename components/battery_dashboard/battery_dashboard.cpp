/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "battery_dashboard.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>

#include "battery_database.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "web_interface.h"

extern const unsigned char dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const unsigned char dashboard_html_end[] asm("_binary_dashboard_html_end");
extern const unsigned char dashboard_js_start[] asm("_binary_dashboard_js_start");
extern const unsigned char dashboard_js_end[] asm("_binary_dashboard_js_end");
extern const unsigned char chart_js_start[] asm("_binary_chart_umd_js_start");
extern const unsigned char chart_js_end[] asm("_binary_chart_umd_js_end");
extern const unsigned char brand_logo_start[] asm("_binary_brand_logo_png_start");
extern const unsigned char brand_logo_end[] asm("_binary_brand_logo_png_end");
extern const unsigned char brand_icon_start[] asm("_binary_brand_icon_png_start");
extern const unsigned char brand_icon_end[] asm("_binary_brand_icon_png_end");

static const char *TAG = "dashboard";
esp_err_t run_management_init();

namespace {

constexpr TickType_t DATABASE_TIMEOUT = pdMS_TO_TICKS(10000);
constexpr size_t RUN_ID_MAX = 64;
constexpr int DEFAULT_POINTS = 1500;
constexpr int MIN_POINTS = 200;
constexpr int MAX_POINTS = 6000;
constexpr size_t STREAM_BUFFER_SIZE = 2048;
constexpr int MAX_LISTED_RUNS = 300;

void common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    common_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message);
}

esp_err_t send_database_error(httpd_req_t *req, esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_STATE:
        return send_error(req, "503 Service Unavailable",
                          "Database not ready (check the SD card or wait for initialization).");
    case ESP_ERR_TIMEOUT:
        return send_error(req, "503 Service Unavailable", "SD card is busy. Please try again.");
    case ESP_ERR_NOT_FOUND:
        return send_error(req, "404 Not Found", "Test run not found.");
    case ESP_ERR_NO_MEM:
        return send_error(req, "500 Internal Server Error", "Not enough memory.");
    default:
        return send_error(req, "500 Internal Server Error", "Database error. See the device log for details.");
    }
}

esp_err_t send_json(httpd_req_t *req, cJSON *root, const char *status = "200 OK")
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return send_error(req, "500 Internal Server Error", "Could not create JSON.");
    }
    common_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

bool valid_run_id(const char *value)
{
    size_t length = strlen(value);
    if (length == 0 || length >= RUN_ID_MAX) {
        return false;
    }
    for (const char *p = value; *p != '\0'; ++p) {
        bool allowed = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' ||
                       *p == ':';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool query_value(httpd_req_t *req, const char *key, char *value, size_t size)
{
    char query[160];
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, key, value, size) == ESP_OK;
}

bool read_run_id(httpd_req_t *req, char *run_id)
{
    if (!query_value(req, "id", run_id, RUN_ID_MAX) || !valid_run_id(run_id)) {
        send_error(req, "400 Bad Request", "Invalid test run ID.");
        return false;
    }
    return true;
}

/* Adds a result column with its SQLite storage class mapped to JSON. */
bool add_column(cJSON *object, const char *name, sqlite3_stmt *statement, int column)
{
    switch (sqlite3_column_type(statement, column)) {
    case SQLITE_INTEGER:
        return cJSON_AddNumberToObject(object, name,
                                       (double)sqlite3_column_int64(statement, column)) != NULL;
    case SQLITE_FLOAT:
        return cJSON_AddNumberToObject(object, name, sqlite3_column_double(statement, column)) !=
               NULL;
    case SQLITE_TEXT:
        return cJSON_AddStringToObject(
                   object, name, reinterpret_cast<const char *>(sqlite3_column_text(statement, column))) !=
               NULL;
    default:
        return cJSON_AddNullToObject(object, name) != NULL;
    }
}

/* Appends one object per row; names map 1:1 to the selected columns. */
int add_rows(sqlite3 *database, const char *sql, const char *run_id, const char *const *names,
             int name_count, cJSON *array, bool single)
{
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (result == SQLITE_OK && run_id != NULL) {
        result = sqlite3_bind_text(statement, 1, run_id, -1, SQLITE_STATIC);
    }
    while (result == SQLITE_OK && (result = sqlite3_step(statement)) == SQLITE_ROW) {
        cJSON *row = single ? array : cJSON_CreateObject();
        bool ok = row != NULL;
        for (int column = 0; ok && column < name_count; ++column) {
            ok = add_column(row, names[column], statement, column);
        }
        if (!single && ok) {
            ok = cJSON_AddItemToArray(array, row);
        }
        if (!ok) {
            if (!single) {
                cJSON_Delete(row);
            }
            result = SQLITE_NOMEM;
            break;
        }
        result = SQLITE_OK;
        if (single) {
            result = SQLITE_DONE;
            break;
        }
    }
    sqlite3_finalize(statement);
    return result == SQLITE_DONE ? SQLITE_OK : result;
}

esp_err_t sqlite_to_esp(sqlite3 *database, int result, const char *operation)
{
    if (result == SQLITE_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s failed (%d): %s", operation, result, sqlite3_errmsg(database));
    return (result & 0xFF) == SQLITE_NOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
}

esp_err_t list_operation(sqlite3 *database, void *context)
{
    cJSON *root = static_cast<cJSON *>(context);
    cJSON *batteries = cJSON_AddArrayToObject(root, "batteries");
    cJSON *runs = cJSON_AddArrayToObject(root, "runs");
    if (batteries == NULL || runs == NULL) {
        return ESP_ERR_NO_MEM;
    }
    static const char *const battery_names[] = {"id", "label", "manufacturer", "serial",
                                                "nominalAh"};
    int result = add_rows(database,
                          "SELECT battery_id, label, manufacturer, serial_number, nominal_capacity_ah "
                          "FROM ebc_battery ORDER BY label COLLATE NOCASE;",
                          NULL, battery_names, 5, batteries, false);
    if (result == SQLITE_OK) {
        static const char *const run_names[] = {"id", "batteryId", "plan", "started", "ended",
                                                "status", "name"};
        char sql[360];
        snprintf(sql, sizeof(sql),
                 "SELECT r.run_id, r.battery_id, p.plan_name, r.started_at_utc, r.ended_at_utc, "
                 "r.status, r.display_name FROM ebc_test_run r LEFT JOIN ebc_test_plan p ON p.plan_id = r.plan_id "
                 "ORDER BY r.started_at_utc DESC LIMIT %d;",
                 MAX_LISTED_RUNS);
        result = add_rows(database, sql, NULL, run_names, 7, runs, false);
    }
    return sqlite_to_esp(database, result, "Run list");
}

esp_err_t run_operation(sqlite3 *database, void *context)
{
    auto *args = static_cast<std::pair<const char *, cJSON *> *>(context);
    const char *run_id = args->first;
    cJSON *root = args->second;
    cJSON *run = cJSON_AddObjectToObject(root, "run");
    cJSON *segments = cJSON_AddArrayToObject(root, "segments");
    cJSON *last = cJSON_AddObjectToObject(root, "last");
    if (run == NULL || segments == NULL || last == NULL) {
        return ESP_ERR_NO_MEM;
    }
    static const char *const run_names[] = {
        "id", "batteryId", "batteryLabel", "manufacturer", "serial", "nominalAh", "plan",
        "started", "ended", "status", "currentStep", "cyclePass", "firmware", "notes", "name"};
    int result = add_rows(
        database,
        "SELECT r.run_id, r.battery_id, b.label, b.manufacturer, b.serial_number, "
        "b.nominal_capacity_ah, p.plan_name, r.started_at_utc, r.ended_at_utc, r.status, "
        "r.current_step, r.cycle_pass, r.firmware_version, r.notes, r.display_name FROM ebc_test_run r "
        "LEFT JOIN ebc_battery b ON b.battery_id = r.battery_id "
        "LEFT JOIN ebc_test_plan p ON p.plan_id = r.plan_id WHERE r.run_id = ?1;",
        run_id, run_names, 15, run, true);
    if (result == SQLITE_OK && cJSON_GetObjectItemCaseSensitive(run, "id") == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (result == SQLITE_OK) {
        static const char *const segment_names[] = {
            "n", "step", "cycle", "sp1", "sp2", "sp3", "mode", "samples", "capacityAh",
            "startMs", "endMs", "startUptime", "endUptime"};
        result = add_rows(
            database,
            "SELECT s.segment_number, s.step_number, s.cycle_pass, s.setpoint_1, s.setpoint_2, "
            "s.setpoint_3, COALESCE(ps.mode, CASE a.mode_code % 10 WHEN 0 THEN 'D-CC' "
            "WHEN 1 THEN 'D-CP' WHEN 2 THEN 'C-CV' END), COALESCE(a.samples, 0), a.capacity, "
            "CAST(round((julianday(a.first_time) - 2440587.5) * 86400000.0) AS INTEGER), "
            "CAST(round((julianday(a.last_time) - 2440587.5) * 86400000.0) AS INTEGER), "
            "a.first_uptime, a.last_uptime FROM ebc_run_segment s "
            "JOIN ebc_test_run r ON r.run_id = s.run_id "
            "LEFT JOIN ebc_test_plan_step ps ON ps.plan_id = r.plan_id "
            "AND ps.step_number = s.step_number "
            "LEFT JOIN (SELECT segment_number, COUNT(*) AS samples, MAX(capacity_ah) AS capacity, "
            "MAX(mode_code) AS mode_code, MIN(captured_at_utc) AS first_time, "
            "MAX(captured_at_utc) AS last_time, MIN(device_uptime_ms) AS first_uptime, "
            "MAX(device_uptime_ms) AS last_uptime FROM ebc_measurement WHERE run_id = ?1 "
            "GROUP BY segment_number) a ON a.segment_number = s.segment_number "
            "WHERE s.run_id = ?1 ORDER BY s.segment_number;",
            run_id, segment_names, 13, segments, false);
    }
    if (result == SQLITE_OK) {
        static const char *const last_names[] = {"segment", "current", "voltage", "capacity",
                                                 "modeCode", "time"};
        result = add_rows(database,
                          "SELECT segment_number, current_a, voltage_v, capacity_ah, mode_code, "
                          "captured_at_utc FROM ebc_measurement WHERE run_id = ?1 "
                          "ORDER BY sequence_number DESC LIMIT 1;",
                          run_id, last_names, 6, last, true);
    }
    return sqlite_to_esp(database, result, "Run details");
}

struct StreamContext {
    httpd_req_t *req;
    const char *run_id;
    int max_points;
    char *buffer;
    size_t used;
    bool started;
    esp_err_t send_error;
};

bool flush(StreamContext &stream)
{
    if (stream.used > 0 && stream.send_error == ESP_OK) {
        stream.send_error = httpd_resp_send_chunk(stream.req, stream.buffer, stream.used);
    }
    stream.used = 0;
    return stream.send_error == ESP_OK;
}

bool append(StreamContext &stream, const char *format, ...)
{
    if (STREAM_BUFFER_SIZE - stream.used < 128 && !flush(stream)) {
        return false;
    }
    va_list args;
    va_start(args, format);
    int written = vsnprintf(stream.buffer + stream.used, STREAM_BUFFER_SIZE - stream.used, format,
                            args);
    va_end(args);
    if (written < 0 || (size_t)written >= STREAM_BUFFER_SIZE - stream.used) {
        stream.send_error = ESP_ERR_INVALID_SIZE;
        return false;
    }
    stream.used += (size_t)written;
    return true;
}

bool append_number(StreamContext &stream, sqlite3_stmt *statement, int column, const char *format)
{
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return append(stream, ",null");
    }
    return append(stream, format, sqlite3_column_double(statement, column));
}

/* Streams a stride-sampled series that always keeps the first and last point of each segment. */
esp_err_t measurement_operation(sqlite3 *database, void *context)
{
    StreamContext &stream = *static_cast<StreamContext *>(context);
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(
        database,
        "SELECT (SELECT COUNT(*) FROM ebc_test_run WHERE run_id = ?1), COUNT(*), "
        "MIN(sequence_number), MAX(sequence_number) FROM ebc_measurement WHERE run_id = ?1;",
        -1, &statement, NULL);
    sqlite3_int64 total = 0;
    sqlite3_int64 first = 0;
    sqlite3_int64 last = 0;
    bool run_exists = false;
    if (result == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, stream.run_id, -1, SQLITE_STATIC);
        result = sqlite3_step(statement);
        if (result == SQLITE_ROW) {
            run_exists = sqlite3_column_int(statement, 0) > 0;
            total = sqlite3_column_int64(statement, 1);
            first = sqlite3_column_int64(statement, 2);
            last = sqlite3_column_int64(statement, 3);
            result = SQLITE_OK;
        }
    }
    sqlite3_finalize(statement);
    statement = NULL;
    if (result != SQLITE_OK) {
        return sqlite_to_esp(database, result, "Measurement summary");
    }
    if (!run_exists) {
        return ESP_ERR_NOT_FOUND;
    }
    sqlite3_int64 stride = total > stream.max_points
                               ? (total + stream.max_points - 1) / stream.max_points
                               : 1;
    result = sqlite3_prepare_v2(
        database,
        "SELECT segment_number, device_uptime_ms, "
        "CAST(round((julianday(captured_at_utc) - 2440587.5) * 86400000.0) AS INTEGER), "
        "current_a, voltage_v, capacity_ah FROM ebc_measurement WHERE run_id = ?1 AND ("
        "(sequence_number - ?2) % ?3 = 0 OR sequence_number = ?4 OR sequence_number IN "
        "(SELECT first_sequence FROM ebc_run_segment WHERE run_id = ?1) OR sequence_number + 1 IN "
        "(SELECT first_sequence FROM ebc_run_segment WHERE run_id = ?1)) "
        "ORDER BY sequence_number;",
        -1, &statement, NULL);
    if (result != SQLITE_OK) {
        sqlite3_finalize(statement);
        return sqlite_to_esp(database, result, "Measurement query");
    }
    sqlite3_bind_text(statement, 1, stream.run_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(statement, 2, first);
    sqlite3_bind_int64(statement, 3, stride);
    sqlite3_bind_int64(statement, 4, last);

    common_headers(stream.req);
    httpd_resp_set_type(stream.req, "application/json");
    stream.started = true;
    bool ok = append(stream, "{\"total\":%" PRId64 ",\"stride\":%" PRId64 ",\"points\":[",
                     (int64_t)total, (int64_t)stride);
    bool first_row = true;
    while (ok && (result = sqlite3_step(statement)) == SQLITE_ROW) {
        ok = append(stream, "%s[%d,%" PRId64, first_row ? "" : ",",
                    sqlite3_column_int(statement, 0), (int64_t)sqlite3_column_int64(statement, 1));
        if (ok) {
            ok = sqlite3_column_type(statement, 2) == SQLITE_NULL
                     ? append(stream, ",null")
                     : append(stream, ",%" PRId64, (int64_t)sqlite3_column_int64(statement, 2));
        }
        ok = ok && append_number(stream, statement, 3, ",%.3f") &&
             append_number(stream, statement, 4, ",%.3f") &&
             append_number(stream, statement, 5, ",%.4f") && append(stream, "]");
        first_row = false;
    }
    sqlite3_finalize(statement);
    if (ok && result != SQLITE_DONE) {
        ESP_LOGE(TAG, "Measurement stream failed (%d): %s", result, sqlite3_errmsg(database));
        ok = false;
    }
    if (ok) {
        ok = append(stream, "]}") && flush(stream);
    }
    if (ok) {
        stream.send_error = httpd_resp_send_chunk(stream.req, NULL, 0);
    }
    return ok && stream.send_error == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t chart_library_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=86400");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    return httpd_resp_send(req, reinterpret_cast<const char *>(chart_js_start),
                           chart_js_end - chart_js_start);
}

esp_err_t send_branding_image(httpd_req_t *req, const unsigned char *start,
                              const unsigned char *end)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=86400");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_type(req, "image/png");
    return httpd_resp_send(req, reinterpret_cast<const char *>(start), end - start);
}

esp_err_t brand_logo_handler(httpd_req_t *req)
{
    return send_branding_image(req, brand_logo_start, brand_logo_end);
}

esp_err_t brand_icon_handler(httpd_req_t *req)
{
    return send_branding_image(req, brand_icon_start, brand_icon_end);
}

esp_err_t runs_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return send_database_error(req, ESP_ERR_NO_MEM);
    }
    esp_err_t err = battery_database_execute(list_operation, root, DATABASE_TIMEOUT);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_database_error(req, err);
    }
    return send_json(req, root);
}

esp_err_t run_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    char run_id[RUN_ID_MAX];
    if (!read_run_id(req, run_id)) {
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return send_database_error(req, ESP_ERR_NO_MEM);
    }
    std::pair<const char *, cJSON *> args(run_id, root);
    esp_err_t err = battery_database_execute(run_operation, &args, DATABASE_TIMEOUT);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_database_error(req, err);
    }
    return send_json(req, root);
}

esp_err_t measurements_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    char run_id[RUN_ID_MAX];
    if (!read_run_id(req, run_id)) {
        return ESP_OK;
    }
    char max_text[8];
    int max_points = DEFAULT_POINTS;
    if (query_value(req, "max", max_text, sizeof(max_text))) {
        max_points = atoi(max_text);
        max_points = max_points < MIN_POINTS ? MIN_POINTS
                     : max_points > MAX_POINTS ? MAX_POINTS
                                               : max_points;
    }
    StreamContext stream = {};
    stream.req = req;
    stream.run_id = run_id;
    stream.max_points = max_points;
    stream.buffer = static_cast<char *>(malloc(STREAM_BUFFER_SIZE));
    if (stream.buffer == NULL) {
        return send_database_error(req, ESP_ERR_NO_MEM);
    }
    esp_err_t err = battery_database_execute(measurement_operation, &stream, DATABASE_TIMEOUT);
    free(stream.buffer);
    if (stream.started) {
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Measurement stream for %s aborted", run_id);
        }
        return err == ESP_OK ? ESP_OK : ESP_FAIL;
    }
    return err == ESP_OK ? ESP_OK : send_database_error(req, err);
}

const httpd_uri_t routes[] = {
    {.uri = "/ui/dashboard/chart.js", .method = HTTP_GET, .handler = chart_library_handler, .user_ctx = NULL},
    {.uri = "/ui/dashboard/brand-logo.png", .method = HTTP_GET, .handler = brand_logo_handler, .user_ctx = NULL},
    {.uri = "/ui/dashboard/brand-icon.png", .method = HTTP_GET, .handler = brand_icon_handler, .user_ctx = NULL},
    {.uri = "/api/db/runs", .method = HTTP_GET, .handler = runs_handler, .user_ctx = NULL},
    {.uri = "/api/db/run", .method = HTTP_GET, .handler = run_handler, .user_ctx = NULL},
    {.uri = "/api/db/measurements", .method = HTTP_GET, .handler = measurements_handler, .user_ctx = NULL},
};

web_interface_extension_t extension = {};

} // namespace

esp_err_t battery_dashboard_init(void)
{
    extension.id = "dashboard";
    extension.label = "Measurements";
    extension.content = dashboard_html_start;
    extension.content_size = (size_t)(dashboard_html_end - dashboard_html_start);
    extension.script = dashboard_js_start;
    extension.script_size = (size_t)(dashboard_js_end - dashboard_js_start);
    extension.routes = routes;
    extension.route_count = sizeof(routes) / sizeof(routes[0]);
    esp_err_t err = web_interface_register_extension(&extension);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot register dashboard UI: %s", esp_err_to_name(err));
    }
    return err == ESP_OK ? run_management_init() : err;
}
