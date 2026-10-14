/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "battery_database.h"
#include "cJSON.h"
#include "ebc_link.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "test_control_internal.h"
#include "web_interface.h"

extern const unsigned char battery_html_start[] asm("_binary_battery_html_start");
extern const unsigned char battery_html_end[] asm("_binary_battery_html_end");
extern const unsigned char battery_js_start[] asm("_binary_battery_js_start");
extern const unsigned char battery_js_end[] asm("_binary_battery_js_end");
extern const unsigned char manual_html_start[] asm("_binary_manual_html_start");
extern const unsigned char manual_html_end[] asm("_binary_manual_html_end");
extern const unsigned char manual_js_start[] asm("_binary_manual_js_start");
extern const unsigned char manual_js_end[] asm("_binary_manual_js_end");
extern const unsigned char auto_html_start[] asm("_binary_auto_html_start");
extern const unsigned char auto_html_end[] asm("_binary_auto_html_end");
extern const unsigned char auto_js_start[] asm("_binary_auto_js_start");
extern const unsigned char auto_js_end[] asm("_binary_auto_js_end");
extern const unsigned char resistance_html_start[] asm("_binary_resistance_html_start");
extern const unsigned char resistance_html_end[] asm("_binary_resistance_html_end");
extern const unsigned char resistance_js_start[] asm("_binary_resistance_js_start");
extern const unsigned char resistance_js_end[] asm("_binary_resistance_js_end");

namespace test_control {
namespace {

const char *TAG = "tester_web";
constexpr size_t MAX_BODY = 2048;

void common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

esp_err_t send_text(httpd_req_t *req, const char *status, const std::string &message)
{
    common_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message.c_str());
}

/* Sends an empty error as success text, otherwise 409 with the German error message. */
esp_err_t send_result(httpd_req_t *req, const std::string &error, const char *success)
{
    return error.empty() ? send_text(req, "200 OK", success) : send_text(req, "409 Conflict", error);
}

esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json = root != nullptr ? cJSON_PrintUnformatted(root) : nullptr;
    cJSON_Delete(root);
    if (json == nullptr) {
        return send_text(req, "500 Internal Server Error", "Could not create response.");
    }
    common_headers(req);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

/* Parses the JSON request body; sends an error response and returns nullptr on failure. */
cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        send_text(req, "413 Payload Too Large", "Request empty or too large.");
        return nullptr;
    }
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int result = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (result <= 0) {
            send_text(req, "400 Bad Request", "Incomplete request.");
            return nullptr;
        }
        received += static_cast<size_t>(result);
    }
    cJSON *root = cJSON_Parse(body.c_str());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        send_text(req, "400 Bad Request", "Invalid JSON.");
        return nullptr;
    }
    return root;
}

std::string get_string(const cJSON *root, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

double get_number(const cJSON *root, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(item) ? item->valuedouble : NAN;
}

bool valid_text(const std::string &value, size_t max_length)
{
    if (value.size() > max_length) {
        return false;
    }
    for (unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7F) {
            return false;
        }
    }
    return true;
}

bool valid_date(const std::string &value)
{
    if (value.empty()) {
        return true;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    char tail = 0;
    return value.size() == 10 && sscanf(value.c_str(), "%4d-%2d-%2d%c", &year, &month, &day, &tail) == 3 &&
           year >= 1990 && year <= 2100 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

cJSON *battery_json(const test_store::Battery &battery)
{
    cJSON *object = cJSON_CreateObject();
    cJSON_AddStringToObject(object, "label", battery.label.c_str());
    cJSON_AddStringToObject(object, "manufacturer", battery.manufacturer.c_str());
    cJSON_AddStringToObject(object, "serial", battery.serial_number.c_str());
    cJSON_AddStringToObject(object, "date", battery.manufactured_on.c_str());
    cJSON_AddStringToObject(object, "notes", battery.notes.c_str());
    cJSON_AddNumberToObject(object, "capacity", battery.nominal_capacity_ah);
    return object;
}

cJSON *steps_json(const std::vector<test_store::PlanStep> &steps)
{
    cJSON *array = cJSON_CreateArray();
    for (const test_store::PlanStep &step : steps) {
        const int values[4] = {step.mode, step.p1, step.p2, step.p3};
        cJSON_AddItemToArray(array, cJSON_CreateIntArray(values, 4));
    }
    return array;
}

esp_err_t status_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    ebc_link_status_t link;
    ebc_link_get_status(&link);
    const PlanStatus plan = get_plan_status();
    const Settings settings = get_settings();
    const test_store::RecorderStatus recorder = test_store::recorder_status();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "controlsBusy", controls_busy());
    cJSON *ebc = cJSON_AddObjectToObject(root, "link");
    cJSON_AddBoolToObject(ebc, "connected", link.connected);
    cJSON_AddBoolToObject(ebc, "hasFrame", link.has_frame);
    cJSON_AddBoolToObject(ebc, "autoConnect", link.auto_connect);
    cJSON_AddBoolToObject(ebc, "pending", link.command_pending);
    cJSON_AddNumberToObject(ebc, "mode", link.frame.mode);
    cJSON_AddStringToObject(ebc, "modeName", link.has_frame ? ebc_link_mode_name(link.frame.mode) : "-");
    cJSON_AddBoolToObject(ebc, "active", link.has_frame && ebc_link_mode_is_active(link.frame.mode));
    cJSON_AddNumberToObject(ebc, "model", link.frame.model);
    cJSON_AddNumberToObject(ebc, "current", link.frame.current_a);
    cJSON_AddNumberToObject(ebc, "voltage", link.frame.voltage_v);
    cJSON_AddNumberToObject(ebc, "capacity", link.frame.capacity_ah);
    cJSON_AddNumberToObject(ebc, "power", link.frame.current_a * link.frame.voltage_v);
    cJSON_AddNumberToObject(ebc, "setpoint1", link.frame.setpoint_1);
    cJSON_AddNumberToObject(ebc, "setpoint2", link.frame.setpoint_2);
    cJSON_AddNumberToObject(ebc, "setpoint3", link.frame.setpoint_3);
    cJSON_AddNumberToObject(ebc, "framesOk", link.frames_ok);
    cJSON_AddNumberToObject(ebc, "framesBad", link.frames_bad);
    cJSON_AddNumberToObject(ebc, "ageMs", link.last_frame_age_ms == UINT32_MAX ? -1 : link.last_frame_age_ms);

    cJSON *run = cJSON_AddObjectToObject(root, "plan");
    cJSON_AddStringToObject(run, "state", run_state_name(plan.state));
    cJSON_AddNumberToObject(run, "step", plan.step_index + 1);
    cJSON_AddNumberToObject(run, "steps", settings.plan.steps.size());
    cJSON_AddNumberToObject(run, "cycle", plan.cycle_pass);
    cJSON_AddNumberToObject(run, "stepElapsed", plan.step_elapsed_s);
    cJSON_AddStringToObject(run, "message", plan.message.c_str());
    cJSON_AddStringToObject(run, "name", settings.plan.name.c_str());

    cJSON *record = cJSON_AddObjectToObject(root, "recorder");
    cJSON_AddBoolToObject(record, "active", recorder.active);
    cJSON_AddBoolToObject(record, "failed", recorder.failed);
    cJSON_AddStringToObject(record, "runId", recorder.run_id.c_str());
    cJSON_AddNumberToObject(record, "written", recorder.samples_written);
    cJSON_AddNumberToObject(record, "pending", recorder.samples_pending);
    cJSON_AddNumberToObject(record, "dropped", recorder.samples_dropped);
    cJSON_AddStringToObject(record, "error", recorder.error.c_str());

    cJSON_AddStringToObject(root, "battery", settings.battery.label.c_str());
    cJSON_AddBoolToObject(root, "databaseReady", battery_database_is_ready());
    return send_json(req, root);
}

esp_err_t settings_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    const Settings settings = get_settings();
    cJSON *root = cJSON_CreateObject();
    cJSON *manual = cJSON_AddObjectToObject(root, "manual");
    cJSON_AddNumberToObject(manual, "chargeCurrent", settings.manual.charge_current_a);
    cJSON_AddNumberToObject(manual, "chargeVoltage", settings.manual.charge_voltage_v);
    cJSON_AddNumberToObject(manual, "chargeCutoff", settings.manual.charge_cutoff_a);
    cJSON_AddNumberToObject(manual, "dischargeCurrent", settings.manual.discharge_current_a);
    cJSON_AddNumberToObject(manual, "dischargeCutoff", settings.manual.discharge_cutoff_v);
    cJSON_AddNumberToObject(manual, "dischargeMinutes", settings.manual.discharge_minutes);
    cJSON_AddNumberToObject(manual, "power", settings.manual.power_w);
    cJSON_AddNumberToObject(manual, "powerCutoff", settings.manual.power_cutoff_v);
    cJSON_AddNumberToObject(manual, "powerMinutes", settings.manual.power_minutes);
    cJSON_AddBoolToObject(root, "autoConnect", settings.auto_connect);
    cJSON_AddItemToObject(root, "battery", battery_json(settings.battery));
    cJSON *plan = cJSON_AddObjectToObject(root, "plan");
    cJSON_AddStringToObject(plan, "name", settings.plan.name.c_str());
    cJSON_AddItemToObject(plan, "steps", steps_json(settings.plan.steps));
    return send_json(req, root);
}

esp_err_t command_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, true)) {
        return ESP_OK;
    }
    cJSON *root = read_json(req);
    if (root == nullptr) {
        return ESP_OK;
    }
    const std::string action = get_string(root, "action");
    const float current = static_cast<float>(get_number(root, "current"));
    const float voltage = static_cast<float>(get_number(root, "voltage"));
    const float cutoff = static_cast<float>(get_number(root, "cutoff"));
    const float power = static_cast<float>(get_number(root, "power"));
    const double minutes = get_number(root, "minutes");
    const bool enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "enabled"));
    cJSON_Delete(root);
    const bool minutes_ok = std::isfinite(minutes) && minutes >= 0 && minutes <= 239;
    const uint16_t whole_minutes = minutes_ok ? static_cast<uint16_t>(minutes) : 0;

    if (action == "connect") {
        return send_result(req, connect(), "Connection request sent.");
    }
    if (action == "disconnect") {
        return send_result(req, disconnect(), "Disconnect sent; keypad lock will be released.");
    }
    if (action == "stop") {
        return send_result(req, stop(), "Stop sent.");
    }
    if (action == "auto_connect") {
        return send_result(req, set_auto_connect(enabled),
                           enabled ? "Automatic connection enabled." : "Automatic connection disabled.");
    }
    if (action == "charge") {
        return send_result(req, start_charge(current, voltage, cutoff), "Starting charge (C-CV).");
    }
    if (action == "discharge_cc") {
        return send_result(req, minutes_ok ? start_discharge_cc(current, cutoff, whole_minutes) : "Time 0â€“239 min.",
                           "Starting discharge (D-CC).");
    }
    if (action == "discharge_cp") {
        return send_result(req, minutes_ok ? start_discharge_cp(power, cutoff, whole_minutes) : "Time 0â€“239 min.",
                           "Starting discharge (D-CP).");
    }
    return send_text(req, "400 Bad Request", "Unknown action.");
}

esp_err_t battery_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, true)) {
        return ESP_OK;
    }
    cJSON *root = read_json(req);
    if (root == nullptr) {
        return ESP_OK;
    }
    const std::string action = get_string(root, "action");
    const std::string label = get_string(root, "label");
    test_store::Battery battery;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "battery");
    if (cJSON_IsObject(data)) {
        battery.label = get_string(data, "label");
        battery.manufacturer = get_string(data, "manufacturer");
        battery.serial_number = get_string(data, "serial");
        battery.manufactured_on = get_string(data, "date");
        battery.notes = get_string(data, "notes");
        const double capacity = get_number(data, "capacity");
        battery.nominal_capacity_ah = std::isfinite(capacity) ? capacity : 0.0;
    }
    cJSON_Delete(root);

    if (action == "load_db") {
        if (!valid_text(label, 64) || label.empty()) {
            return send_text(req, "400 Bad Request", "Please select a battery.");
        }
        test_store::Battery loaded;
        const esp_err_t err = test_store::load_battery(label, loaded);
        if (err == ESP_ERR_NOT_FOUND) {
            return send_text(req, "404 Not Found", "Battery not found.");
        }
        if (err != ESP_OK) {
            return send_text(req, "503 Service Unavailable", "SD database unavailable.");
        }
        return send_result(req, set_battery(loaded), "Battery loaded from database.");
    }
    if (action != "save" && action != "save_db") {
        return send_text(req, "400 Bad Request", "Unknown action.");
    }
    if (!valid_text(battery.label, 64) || !valid_text(battery.manufacturer, 64) ||
        !valid_text(battery.serial_number, 64) || !valid_text(battery.notes, 255) ||
        !valid_date(battery.manufactured_on) || battery.nominal_capacity_ah < 0 ||
        battery.nominal_capacity_ah > 10000) {
        return send_text(req, "400 Bad Request", "Invalid battery data (lengths, date or capacity).");
    }
    if (action == "save_db") {
        if (battery.label.empty()) {
            return send_text(req, "400 Bad Request", "A label is required for the database.");
        }
        if (test_store::save_battery(battery) != ESP_OK) {
            return send_text(req, "503 Service Unavailable", "Could not save to SD database.");
        }
    }
    return send_result(req, set_battery(battery),
                       action == "save_db" ? "Battery saved and stored in database."
                                           : "Battery applied for next test.");
}

esp_err_t catalog_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) {
        return ESP_OK;
    }
    std::vector<std::string> batteries;
    std::vector<std::string> plans;
    const bool ready = battery_database_is_ready();
    bool ok = ready && test_store::list_batteries(batteries) == ESP_OK && test_store::list_plans(plans) == ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "databaseReady", ok);
    cJSON *battery_array = cJSON_AddArrayToObject(root, "batteries");
    for (const std::string &label : batteries) {
        cJSON_AddItemToArray(battery_array, cJSON_CreateString(label.c_str()));
    }
    cJSON *plan_array = cJSON_AddArrayToObject(root, "plans");
    for (const std::string &name : plans) {
        cJSON_AddItemToArray(plan_array, cJSON_CreateString(name.c_str()));
    }
    return send_json(req, root);
}

esp_err_t plan_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, true)) {
        return ESP_OK;
    }
    cJSON *root = read_json(req);
    if (root == nullptr) {
        return ESP_OK;
    }
    test_store::Plan plan;
    plan.name = get_string(root, "name");
    bool valid = valid_text(plan.name, 64);
    const cJSON *steps = cJSON_GetObjectItemCaseSensitive(root, "steps");
    valid = valid && cJSON_IsArray(steps) && cJSON_GetArraySize(steps) <= static_cast<int>(test_store::MAX_PLAN_STEPS);
    const cJSON *row = nullptr;
    if (valid) {
        cJSON_ArrayForEach(row, steps)
        {
            double values[4] = {};
            valid = valid && cJSON_IsArray(row) && cJSON_GetArraySize(row) == 4;
            for (int index = 0; valid && index < 4; ++index) {
                const cJSON *item = cJSON_GetArrayItem(row, index);
                valid = cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= 65535 &&
                        item->valuedouble == std::floor(item->valuedouble);
                values[index] = valid ? item->valuedouble : 0;
            }
            if (valid) {
                plan.steps.push_back({static_cast<uint8_t>(values[0]), static_cast<uint16_t>(values[1]),
                                      static_cast<uint16_t>(values[2]), static_cast<uint16_t>(values[3])});
            }
        }
    }
    cJSON_Delete(root);
    if (!valid) {
        return send_text(req, "400 Bad Request", "Invalid plan (name up to 64 characters, up to 10 steps).");
    }
    return send_result(req, set_plan(plan), "Plan saved.");
}

esp_err_t plan_action_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, true)) {
        return ESP_OK;
    }
    cJSON *root = read_json(req);
    if (root == nullptr) {
        return ESP_OK;
    }
    const std::string action = get_string(root, "action");
    const std::string name = get_string(root, "name");
    cJSON_Delete(root);
    if (action == "start") {
        return send_result(req, start_plan(), "Plan started.");
    }
    if (action == "abort") {
        return send_result(req, abort_plan(), "Plan aborted; stop sent.");
    }
    if (action == "reset") {
        return send_result(req, reset_plan(), "Planstatus zurueckgesetzt.");
    }
    if (action == "load_db") {
        if (name.empty() || !valid_text(name, 64)) {
            return send_text(req, "400 Bad Request", "Please select a plan.");
        }
        test_store::Plan plan;
        const esp_err_t err = test_store::load_plan(name, plan);
        if (err == ESP_ERR_NOT_FOUND) {
            return send_text(req, "404 Not Found", "Plan not found.");
        }
        if (err != ESP_OK) {
            return send_text(req, "503 Service Unavailable", "SD database unavailable.");
        }
        return send_result(req, set_plan(plan), "Plan loaded from database.");
    }
    if (action == "save_db") {
        const test_store::Plan plan = get_settings().plan;
        if (plan.name.empty()) {
            return send_text(req, "400 Bad Request", "Please give the plan a name.");
        }
        const std::string error = test_store::validate_plan(plan.steps);
        if (!error.empty()) {
            return send_text(req, "400 Bad Request", error);
        }
        if (test_store::save_plan(plan) != ESP_OK) {
            return send_text(req, "503 Service Unavailable", "Could not save to SD database.");
        }
        return send_text(req, "200 OK", "Plan saved to database.");
    }
    return send_text(req, "400 Bad Request", "Unknown action.");
}

esp_err_t resistance_status_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) return ESP_OK;
    const auto config = resistance_test::get_config();
    const auto status = resistance_test::get_status();
    cJSON *root = cJSON_CreateObject();
    cJSON *settings_json = cJSON_AddObjectToObject(root, "config");
    cJSON_AddBoolToObject(settings_json, "automatic", config.automatic);
    for (const auto &field : resistance_test::fields)
        cJSON_AddNumberToObject(settings_json, field.name, config.*field.member);
    cJSON_AddBoolToObject(root, "busy", status.busy);
    cJSON_AddStringToObject(root, "phase", status.phase.c_str());
    cJSON_AddStringToObject(root, "message", status.message.c_str());
    cJSON_AddStringToObject(root, "runId", status.runId.c_str());
    cJSON_AddNumberToObject(root, "elapsed", status.elapsed);
    cJSON_AddNumberToObject(root, "removedAh", status.removedAh);
    cJSON_AddNumberToObject(root, "targetAh", status.targetAh);
    cJSON_AddNumberToObject(root, "baselineVoltage", status.baselineVoltage);
    cJSON_AddNumberToObject(root, "baselineCurrent", status.baselineCurrent);
    cJSON_AddNumberToObject(root, "onsetUncertainty", status.onsetUncertainty);
    cJSON *results = cJSON_AddArrayToObject(root, "results");
    for (const auto &result : status.results) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToArray(results, r);
        cJSON_AddBoolToObject(r, "captured", result.captured);
        cJSON_AddBoolToObject(r, "valid", result.valid);
        cJSON_AddNumberToObject(r, "requested", result.requested);
        cJSON_AddNumberToObject(r, "actual", result.actual);
        if (result.epoch_ms > 0) cJSON_AddNumberToObject(r, "epochMs", static_cast<double>(result.epoch_ms));
        else cJSON_AddNullToObject(r, "epochMs");
        cJSON_AddNumberToObject(r, "voltage", result.voltage);
        cJSON_AddNumberToObject(r, "current", result.current);
        if (result.valid) cJSON_AddNumberToObject(r, "milliohms", result.milliohms);
        else cJSON_AddNullToObject(r, "milliohms");
        cJSON_AddStringToObject(r, "error", result.error.c_str());
    }
    return send_json(req, root);
}

esp_err_t resistance_action_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, true)) return ESP_OK;
    cJSON *root = read_json(req);
    if (root == nullptr) return ESP_OK;
    const std::string action = get_string(root, "action");
    if (action == "abort") {
        cJSON_Delete(root);
        return send_result(req, abort_resistance(), "Abbruch angefordert; Stoppbestaetigung abwarten.");
    }
    const cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "config");
    const cJSON *automatic = cJSON_GetObjectItemCaseSensitive(input, "automatic");
    bool valid = cJSON_IsObject(input) && cJSON_IsBool(automatic);
    resistance_test::Config config;
    config.automatic = cJSON_IsTrue(automatic);
    for (const auto &field : resistance_test::fields) {
        const double value = get_number(input, field.name);
        valid = valid && std::isfinite(value);
        config.*field.member = value;
    }
    cJSON_Delete(root);
    if (!valid) return send_text(req, "400 Bad Request", "All measurement parameters are required.");
    if (action == "save") return send_result(req, resistance_test::save_config(config), "Measurement parameters saved.");
    if (action == "start") return send_result(req, start_resistance(config), "Measurement started.");
    return send_text(req, "400 Bad Request", "Unknown measurement action.");
}

esp_err_t resistance_history_handler(httpd_req_t *req)
{
    if (!web_interface_authorize_request(req, false)) return ESP_OK;
    cJSON *root = cJSON_CreateArray();
    if (root == nullptr) return send_text(req, "500 Internal Server Error", "Not enough memory for history.");
    const esp_err_t err = battery_database_execute([](sqlite3 *db, void *context) {
        auto *array = static_cast<cJSON *>(context);
        sqlite3_stmt *statement = nullptr;
        int result = sqlite3_prepare_v2(db,
            "SELECT r.run_id, COALESCE(b.label, ''), r.status, r.started_at_utc, r.notes "
            "FROM ebc_test_run r LEFT JOIN ebc_battery b ON b.battery_id = r.battery_id "
            "WHERE r.notes LIKE 'DCIR v1;%' ORDER BY r.rowid DESC LIMIT 20;",
            -1, &statement, nullptr);
        if (result == SQLITE_OK) {
            while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
                cJSON *row = cJSON_CreateObject();
                const char *names[] = {"runId", "battery", "status", "started", "notes"};
                for (int i = 0; i < 5; ++i) {
                    const auto *text = sqlite3_column_text(statement, i);
                    cJSON_AddStringToObject(row, names[i], text != nullptr ? reinterpret_cast<const char *>(text) : "");
                }
                sqlite3_stmt *results = nullptr;
                int result_status = sqlite3_prepare_v2(db,
                    "SELECT result_number, captured_at_utc, requested_seconds, actual_seconds, "
                    "baseline_voltage_v, baseline_current_a, load_voltage_v, load_current_a, "
                    "resistance_mohm, valid, error FROM ebc_resistance_result WHERE run_id = ?1 ORDER BY result_number;",
                    -1, &results, nullptr);
                if (result_status == SQLITE_OK) {
                    sqlite3_bind_text(results, 1, reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)),
                                      -1, SQLITE_TRANSIENT);
                    cJSON *items = cJSON_AddArrayToObject(row, "results");
                    while ((result_status = sqlite3_step(results)) == SQLITE_ROW) {
                        cJSON *item = cJSON_CreateObject();
                        const char *keys[] = {"number", "capturedUtc", "requested", "actual", "baselineVoltage",
                            "baselineCurrent", "voltage", "current", "milliohms", "valid", "error"};
                        for (int col = 0; col < 11; ++col) {
                            if (sqlite3_column_type(results, col) == SQLITE_NULL) cJSON_AddNullToObject(item, keys[col]);
                            else if (col == 1 || col == 10)
                                cJSON_AddStringToObject(item, keys[col], reinterpret_cast<const char *>(sqlite3_column_text(results, col)));
                            else cJSON_AddNumberToObject(item, keys[col], sqlite3_column_double(results, col));
                        }
                        cJSON_AddItemToArray(items, item);
                    }
                }
                sqlite3_finalize(results);
                if (result_status != SQLITE_DONE) {
                    ESP_LOGE(TAG, "Cannot load resistance values: %s", sqlite3_errmsg(db));
                    cJSON_Delete(row);
                    sqlite3_finalize(statement);
                    return ESP_FAIL;
                }
                cJSON_AddItemToArray(array, row);
            }
        }
        sqlite3_finalize(statement);
        if (result != SQLITE_DONE) {
            ESP_LOGE(TAG, "Cannot load DCIR history (%d): %s", result, sqlite3_errmsg(db));
            return ESP_FAIL;
        }
        return ESP_OK;
    }, root, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_text(req, "503 Service Unavailable", "SD history unavailable.");
    }
    return send_json(req, root);
}

const httpd_uri_t resistance_routes[] = {
    {.uri = "/api/tester/resistance", .method = HTTP_GET, .handler = resistance_status_handler, .user_ctx = nullptr},
    {.uri = "/api/tester/resistance", .method = HTTP_POST, .handler = resistance_action_handler, .user_ctx = nullptr},
    {.uri = "/api/tester/resistance/history", .method = HTTP_GET, .handler = resistance_history_handler, .user_ctx = nullptr},
};

const httpd_uri_t battery_routes[] = {
    {.uri = "/api/tester/battery", .method = HTTP_POST, .handler = battery_handler, .user_ctx = nullptr},
    {.uri = "/api/tester/catalog", .method = HTTP_GET, .handler = catalog_handler, .user_ctx = nullptr},
};
const httpd_uri_t manual_routes[] = {
    {.uri = "/api/tester/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = nullptr},
    {.uri = "/api/tester/settings", .method = HTTP_GET, .handler = settings_handler, .user_ctx = nullptr},
    {.uri = "/api/tester/command", .method = HTTP_POST, .handler = command_handler, .user_ctx = nullptr},
};
const httpd_uri_t auto_routes[] = {
    {.uri = "/api/tester/plan", .method = HTTP_POST, .handler = plan_handler, .user_ctx = nullptr},
    {.uri = "/api/tester/plan/action", .method = HTTP_POST, .handler = plan_action_handler, .user_ctx = nullptr},
};

web_interface_extension_t extensions[4] = {};

} // namespace

esp_err_t register_web_extensions()
{
    extensions[0] = {"battery", "Battery", battery_html_start,
                     static_cast<size_t>(battery_html_end - battery_html_start), battery_js_start,
                     static_cast<size_t>(battery_js_end - battery_js_start), battery_routes,
                     sizeof(battery_routes) / sizeof(battery_routes[0]), "tab", nullptr};
    extensions[1] = {"manual", "Manual", manual_html_start, static_cast<size_t>(manual_html_end - manual_html_start),
                     manual_js_start, static_cast<size_t>(manual_js_end - manual_js_start), manual_routes,
                     sizeof(manual_routes) / sizeof(manual_routes[0]), "tab", nullptr};
    extensions[2] = {"auto", "Automatic", auto_html_start, static_cast<size_t>(auto_html_end - auto_html_start),
                     auto_js_start, static_cast<size_t>(auto_js_end - auto_js_start), auto_routes,
                     sizeof(auto_routes) / sizeof(auto_routes[0]), "tab", nullptr};
    extensions[3] = {"resistance", "Internal resistance", resistance_html_start,
                     static_cast<size_t>(resistance_html_end - resistance_html_start), resistance_js_start,
                     static_cast<size_t>(resistance_js_end - resistance_js_start), resistance_routes,
                     sizeof(resistance_routes) / sizeof(resistance_routes[0]), "tab", nullptr};
    for (const web_interface_extension_t &extension : extensions) {
        const esp_err_t err = web_interface_register_extension(&extension);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot register %s UI: %s", extension.id, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

} // namespace test_control
