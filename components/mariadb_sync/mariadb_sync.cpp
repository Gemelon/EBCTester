#include "mariadb_sync.h"

#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "battery_database.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mariadb_client.h"
#include "mariadb_sync_internal.h"
extern "C" {
#include "network_manager.h"
}
#include "nvs.h"

namespace mariadb_sync {
namespace {

const char *TAG = "mariadb_sync";
constexpr char NVS_NAMESPACE[] = "mariadb_sync";
constexpr TickType_t DATABASE_TIMEOUT = pdMS_TO_TICKS(15000);
constexpr TickType_t WORKER_POLL = pdMS_TO_TICKS(30000);
constexpr int64_t AUTO_START_DELAY_US = 60LL * 1000 * 1000;
constexpr int MEASUREMENT_BATCH = 200;
constexpr int SEGMENT_BATCH = 200;
constexpr size_t REMOTE_RUN_ID_MAX = 36;
constexpr time_t VALID_TIME = 1700000000;
constexpr uint32_t WORKER_STACK = 12288;

std::mutex state_mutex;
Settings current_settings;
bool settings_loaded = false;
Status current_status;
TaskHandle_t worker_handle = nullptr;
int64_t last_attempt_us = 0;

time_t wall_clock()
{
    const time_t now = time(nullptr);
    return now > VALID_TIME ? now : 0;
}

std::string nvs_read_string(nvs_handle_t handle, const char *key, const std::string &fallback)
{
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0) {
        return fallback;
    }
    std::string value(length, '\0');
    if (nvs_get_str(handle, key, value.data(), &length) != ESP_OK) {
        return fallback;
    }
    value.resize(length - 1);
    return value;
}

/* NVS is initialized by the network manager, so loading is deferred until first use. */
void load_settings_locked()
{
    if (settings_loaded) {
        return;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        settings_loaded = true;
        return;
    }
    if (err != ESP_OK) {
        return;
    }
    Settings loaded;
    uint8_t enabled = 0;
    if (nvs_get_u8(handle, "enabled", &enabled) == ESP_OK) {
        loaded.enabled = enabled != 0;
    }
    nvs_get_u16(handle, "port", &loaded.port);
    nvs_get_u16(handle, "interval", &loaded.interval_minutes);
    loaded.host = nvs_read_string(handle, "host", loaded.host);
    loaded.user = nvs_read_string(handle, "user", loaded.user);
    loaded.password = nvs_read_string(handle, "password", "");
    loaded.database = nvs_read_string(handle, "database", loaded.database);
    nvs_close(handle);
    current_settings = std::move(loaded);
    settings_loaded = true;
}

void update_status(State state, const std::string &message)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    current_status.state = state;
    current_status.message = message;
}

void finish(State state, const std::string &message)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    current_status.state = state;
    current_status.message = message;
    current_status.busy = false;
    if (state == State::Error) {
        ESP_LOGW(TAG, "%s", message.c_str());
    } else {
        ESP_LOGI(TAG, "%s", message.c_str());
    }
}

MariaDbClient::Config client_config(const Settings &settings)
{
    MariaDbClient::Config config;
    config.host = settings.host;
    config.port = settings.port;
    config.user = settings.user;
    config.password = settings.password;
    config.database = settings.database;
    return config;
}

bool network_ready(std::string &error)
{
    network_status_t network = {};
    network_manager_get_status(&network);
    if (!network.connected) {
        error = "No Wi-Fi connection to the home network";
        return false;
    }
    return true;
}

/* ---------- local SQLite access (runs under the SD lock) ---------- */

std::string literal(sqlite3_stmt *statement, int column)
{
    switch (sqlite3_column_type(statement, column)) {
    case SQLITE_INTEGER:
        return std::to_string(static_cast<long long>(sqlite3_column_int64(statement, column)));
    case SQLITE_FLOAT: {
        const char *text = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
        return text != nullptr ? text : "NULL";
    }
    case SQLITE_TEXT:
    case SQLITE_BLOB: {
        const char *text = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
        const int length = sqlite3_column_bytes(statement, column);
        return text != nullptr && length > 0 ? mariadb_text_literal(text, static_cast<size_t>(length))
                                             : "NULL";
    }
    default:
        return "NULL";
    }
}

std::string tuple(sqlite3_stmt *statement, int first_column, int column_count)
{
    std::string out = "(";
    for (int i = 0; i < column_count; ++i) {
        if (i != 0) {
            out += ',';
        }
        out += literal(statement, first_column + i);
    }
    out += ')';
    return out;
}

esp_err_t sqlite_error(sqlite3 *database, const char *what)
{
    ESP_LOGE(TAG, "%s: %s", what, sqlite3_errmsg(database));
    return ESP_FAIL;
}

struct PendingRun {
    std::string run_id;
    int64_t synced_sequence = -1;
    int64_t synced_segment = -1;
    std::string signature;
};

struct PrepareContext {
    bool full = false;
    std::vector<PendingRun> runs;
};

esp_err_t prepare_operation(sqlite3 *database, void *arg)
{
    auto *context = static_cast<PrepareContext *>(arg);
    /* Sync bookkeeping is owned by this component and kept outside the versioned data schema. */
    const char *setup =
        "CREATE TABLE IF NOT EXISTS ebc_sync_state ("
        " run_id TEXT PRIMARY KEY COLLATE BINARY,"
        " synced_sequence INTEGER NOT NULL DEFAULT -1,"
        " synced_segment INTEGER NOT NULL DEFAULT -1,"
        " run_signature TEXT);"
        "DELETE FROM ebc_sync_state WHERE run_id NOT IN (SELECT run_id FROM ebc_test_run);";
    if (sqlite3_exec(database, setup, nullptr, nullptr, nullptr) != SQLITE_OK) {
        return sqlite_error(database, "Cannot prepare sync state");
    }
    if (context->full && sqlite3_exec(database, "DELETE FROM ebc_sync_state;", nullptr, nullptr,
                                      nullptr) != SQLITE_OK) {
        return sqlite_error(database, "Cannot reset sync state");
    }
    sqlite3_stmt *statement = nullptr;
    const char *sql =
        "SELECT r.run_id,"
        " IFNULL(s.synced_sequence, -1), IFNULL(s.synced_segment, -1), s.run_signature,"
        " r.status || '|' || IFNULL(r.ended_at_utc, '') || '|' || IFNULL(r.current_step, '') || '|' ||"
        " IFNULL(r.cycle_pass, '') || '|' || IFNULL(r.plan_id, '') || '|' || IFNULL(r.battery_id, '') ||"
        " '|' || IFNULL(r.notes, '') || '|DCIR2|' ||"
        " (SELECT COUNT(*) FROM ebc_resistance_result x WHERE x.run_id = r.run_id),"
        " (SELECT IFNULL(MAX(sequence_number), -1) FROM ebc_measurement m WHERE m.run_id = r.run_id),"
        " (SELECT IFNULL(MAX(segment_number), -1) FROM ebc_run_segment g WHERE g.run_id = r.run_id)"
        " FROM ebc_test_run r LEFT JOIN ebc_sync_state s ON s.run_id = r.run_id"
        " ORDER BY r.started_at_utc;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return sqlite_error(database, "Cannot list runs for sync");
    }
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        PendingRun run;
        run.run_id = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
        run.synced_sequence = sqlite3_column_int64(statement, 1);
        run.synced_segment = sqlite3_column_int64(statement, 2);
        const unsigned char *stored = sqlite3_column_text(statement, 3);
        run.signature = reinterpret_cast<const char *>(sqlite3_column_text(statement, 4));
        const int64_t max_sequence = sqlite3_column_int64(statement, 5);
        const int64_t max_segment = sqlite3_column_int64(statement, 6);
        const bool changed = stored == nullptr ||
                             run.signature != reinterpret_cast<const char *>(stored);
        if (changed || max_sequence > run.synced_sequence || max_segment > run.synced_segment) {
            context->runs.push_back(std::move(run));
        }
    }
    sqlite3_finalize(statement);
    return result == SQLITE_DONE ? ESP_OK : sqlite_error(database, "Cannot read runs for sync");
}

enum RunColumn {
    RUN_ID, RUN_DEVICE, RUN_PLAN, RUN_PLAN_VERSION, RUN_BATTERY, RUN_STARTED, RUN_ENDED,
    RUN_STATUS, RUN_STEP, RUN_CYCLE, RUN_FIRMWARE, RUN_NOTES, RUN_COLUMN_COUNT
};

struct RunData {
    std::string run_id;
    std::vector<std::string> run;
    bool has_plan = false;
    std::vector<std::string> plan; /* device_id, plan_name, plan_version, definition, created, updated */
    int64_t plan_version = 0;
    std::vector<std::string> step_tails; /* ",step,...)" without the remote plan_id */
    bool has_battery = false;
    std::string battery_tuple;
    std::string battery_label;
    std::string resistance_tuples;
};

esp_err_t load_run_operation(sqlite3 *database, void *arg)
{
    auto *data = static_cast<RunData *>(arg);
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(database,
                           "SELECT run_id, device_id, plan_id, plan_version, battery_id, started_at_utc,"
                           " ended_at_utc, status, current_step, cycle_pass, firmware_version, notes"
                           " FROM ebc_test_run WHERE run_id = ?1;",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return sqlite_error(database, "Cannot prepare run query");
    }
    sqlite3_bind_text(statement, 1, data->run_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return ESP_ERR_NOT_FOUND;
    }
    data->run.clear();
    for (int i = 0; i < RUN_COLUMN_COUNT; ++i) {
        data->run.push_back(literal(statement, i));
    }
    const bool has_plan = sqlite3_column_type(statement, RUN_PLAN) != SQLITE_NULL;
    const int64_t plan_id = sqlite3_column_int64(statement, RUN_PLAN);
    const bool has_battery = sqlite3_column_type(statement, RUN_BATTERY) != SQLITE_NULL;
    const int64_t battery_id = sqlite3_column_int64(statement, RUN_BATTERY);
    sqlite3_finalize(statement);

    data->has_plan = false;
    if (has_plan) {
        if (sqlite3_prepare_v2(database,
                               "SELECT device_id, plan_name, plan_version, plan_definition, created_at_utc,"
                               " updated_at_utc FROM ebc_test_plan WHERE plan_id = ?1;",
                               -1, &statement, nullptr) != SQLITE_OK) {
            return sqlite_error(database, "Cannot prepare plan query");
        }
        sqlite3_bind_int64(statement, 1, plan_id);
        if (sqlite3_step(statement) == SQLITE_ROW) {
            data->has_plan = true;
            data->plan.clear();
            for (int i = 0; i < 6; ++i) {
                data->plan.push_back(literal(statement, i));
            }
            data->plan_version = sqlite3_column_int64(statement, 2);
        }
        sqlite3_finalize(statement);
    }
    data->step_tails.clear();
    if (data->has_plan) {
        if (sqlite3_prepare_v2(database,
                               "SELECT step_number, mode, current_a, power_w, voltage_v, cutoff_current_a,"
                               " max_runtime_minutes, wait_minutes, cycle_total_runs"
                               " FROM ebc_test_plan_step WHERE plan_id = ?1 ORDER BY step_number;",
                               -1, &statement, nullptr) != SQLITE_OK) {
            return sqlite_error(database, "Cannot prepare plan step query");
        }
        sqlite3_bind_int64(statement, 1, plan_id);
        while (sqlite3_step(statement) == SQLITE_ROW) {
            std::string row = tuple(statement, 0, 9);
            row[0] = ',';
            data->step_tails.push_back(std::move(row));
        }
        sqlite3_finalize(statement);
    }

    data->has_battery = false;
    if (has_battery) {
        if (sqlite3_prepare_v2(database,
                               "SELECT label, manufacturer, nominal_capacity_ah, serial_number, manufactured_on,"
                               " notes, created_at_utc, updated_at_utc FROM ebc_battery WHERE battery_id = ?1;",
                               -1, &statement, nullptr) != SQLITE_OK) {
            return sqlite_error(database, "Cannot prepare battery query");
        }
        sqlite3_bind_int64(statement, 1, battery_id);
        if (sqlite3_step(statement) == SQLITE_ROW) {
            data->has_battery = true;
            data->battery_tuple = tuple(statement, 0, 8);
            data->battery_label = literal(statement, 0);
        }
        sqlite3_finalize(statement);
    }
    if (sqlite3_prepare_v2(database,
        "SELECT run_id, result_number, captured_at_utc, device_uptime_ms, requested_seconds, actual_seconds, "
        "baseline_voltage_v, baseline_current_a, load_voltage_v, load_current_a, resistance_mohm, "
        "onset_uncertainty_seconds, valid, error FROM ebc_resistance_result WHERE run_id = ?1 "
        "ORDER BY result_number;", -1, &statement, nullptr) != SQLITE_OK) {
        return sqlite_error(database, "Cannot prepare resistance results query");
    }
    sqlite3_bind_text(statement, 1, data->run_id.c_str(), -1, SQLITE_TRANSIENT);
    data->resistance_tuples.clear();
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        if (!data->resistance_tuples.empty()) data->resistance_tuples += ',';
        data->resistance_tuples += tuple(statement, 0, 14);
    }
    sqlite3_finalize(statement);
    return result == SQLITE_DONE ? ESP_OK : sqlite_error(database, "Cannot load resistance results");
}

struct BatchContext {
    std::string run_id;
    std::string run_literal;
    /* Progress of the previous, successfully transferred batch. */
    bool save = false;
    int64_t save_sequence = -1;
    int64_t save_segment = -1;
    const std::string *signature = nullptr;
    bool load = true;
    int64_t after_sequence = -1;
    int64_t after_segment = -1;
    std::string segments;
    int segment_count = 0;
    int64_t last_segment = -1;
    std::string measurements;
    int measurement_count = 0;
    int64_t last_sequence = -1;
};

esp_err_t batch_operation(sqlite3 *database, void *arg)
{
    auto *batch = static_cast<BatchContext *>(arg);
    sqlite3_stmt *statement = nullptr;
    if (batch->save) {
        if (sqlite3_prepare_v2(database,
                               "INSERT INTO ebc_sync_state (run_id, synced_sequence, synced_segment, run_signature)"
                               " VALUES (?1, ?2, ?3, ?4) ON CONFLICT (run_id) DO UPDATE SET"
                               " synced_sequence = excluded.synced_sequence,"
                               " synced_segment = excluded.synced_segment,"
                               " run_signature = IFNULL(excluded.run_signature, ebc_sync_state.run_signature);",
                               -1, &statement, nullptr) != SQLITE_OK) {
            return sqlite_error(database, "Cannot prepare sync progress update");
        }
        sqlite3_bind_text(statement, 1, batch->run_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 2, batch->save_sequence);
        sqlite3_bind_int64(statement, 3, batch->save_segment);
        if (batch->signature != nullptr) {
            sqlite3_bind_text(statement, 4, batch->signature->c_str(), -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(statement, 4);
        }
        const int result = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (result != SQLITE_DONE) {
            return sqlite_error(database, "Cannot store sync progress");
        }
    }
    batch->segments.clear();
    batch->segment_count = 0;
    batch->measurements.clear();
    batch->measurement_count = 0;
    if (!batch->load) {
        return ESP_OK;
    }

    if (sqlite3_prepare_v2(database,
                           "SELECT segment_number, step_number, cycle_pass, setpoint_1, setpoint_2, setpoint_3,"
                           " first_sequence FROM ebc_run_segment WHERE run_id = ?1 AND segment_number > ?2"
                           " ORDER BY segment_number LIMIT ?3;",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return sqlite_error(database, "Cannot prepare segment query");
    }
    sqlite3_bind_text(statement, 1, batch->run_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, batch->after_segment);
    sqlite3_bind_int(statement, 3, SEGMENT_BATCH);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        batch->segments += batch->segment_count++ == 0 ? "(" : ",(";
        batch->segments += batch->run_literal;
        batch->segments += ',';
        batch->segments += tuple(statement, 0, 7).substr(1);
        batch->last_segment = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);
    const int64_t known_segment = batch->segment_count > 0 ? batch->last_segment : batch->after_segment;

    if (sqlite3_prepare_v2(database,
                           "SELECT sequence_number, segment_number, captured_at_utc, device_uptime_ms, mode_code,"
                           " current_a, voltage_v, capacity_ah, model_id FROM ebc_measurement"
                           " WHERE run_id = ?1 AND sequence_number > ?2 AND segment_number <= ?3"
                           " ORDER BY sequence_number LIMIT ?4;",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return sqlite_error(database, "Cannot prepare measurement query");
    }
    sqlite3_bind_text(statement, 1, batch->run_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, batch->after_sequence);
    sqlite3_bind_int64(statement, 3, known_segment);
    sqlite3_bind_int(statement, 4, MEASUREMENT_BATCH);
    batch->measurements.reserve(MEASUREMENT_BATCH * 170);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        batch->measurements += batch->measurement_count++ == 0 ? "(" : ",(";
        batch->measurements += batch->run_literal;
        batch->measurements += ',';
        batch->measurements += tuple(statement, 0, 9).substr(1);
        batch->last_sequence = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);
    return ESP_OK;
}

/* ---------- remote transfer ---------- */

std::string compare_text(const std::string &literal_value)
{
    return literal_value + " COLLATE utf8mb4_unicode_ci";
}

bool transaction_failed(MariaDbClient &client, std::string &error)
{
    std::string ignored;
    client.execute("ROLLBACK", ignored);
    (void)error;
    return false;
}

bool sync_plan(MariaDbClient &client, const RunData &run, std::string &plan_id, std::string &error)
{
    std::vector<std::vector<std::string>> rows;
    if (!client.query("SELECT plan_id, plan_version FROM ebc_test_plan WHERE device_id = " +
                          compare_text(run.plan[0]) + " AND plan_name = " + compare_text(run.plan[1]),
                      rows, error, 1)) {
        return false;
    }
    if (!rows.empty() && strtoll(rows[0][1].c_str(), nullptr, 10) >= run.plan_version) {
        plan_id = rows[0][0];
        return true;
    }
    if (!client.execute("START TRANSACTION", error)) {
        return false;
    }
    if (rows.empty()) {
        uint64_t insert_id = 0;
        if (!client.execute("INSERT INTO ebc_test_plan (device_id, plan_name, plan_version, plan_definition,"
                            " created_at_utc, updated_at_utc) VALUES (" + run.plan[0] + ',' + run.plan[1] +
                                ',' + run.plan[2] + ',' + run.plan[3] + ',' + run.plan[4] + ',' + run.plan[5] + ')',
                            error, nullptr, &insert_id)) {
            return transaction_failed(client, error);
        }
        plan_id = std::to_string(insert_id);
    } else {
        plan_id = rows[0][0];
        if (!client.execute("UPDATE ebc_test_plan SET plan_version = " + run.plan[2] +
                                ", plan_definition = " + run.plan[3] + ", updated_at_utc = " + run.plan[5] +
                                " WHERE plan_id = " + plan_id,
                            error)) {
            return transaction_failed(client, error);
        }
    }
    if (!client.execute("DELETE FROM ebc_test_plan_step WHERE plan_id = " + plan_id, error)) {
        return transaction_failed(client, error);
    }
    if (!run.step_tails.empty()) {
        std::string sql = "INSERT INTO ebc_test_plan_step (plan_id, step_number, mode, current_a, power_w,"
                          " voltage_v, cutoff_current_a, max_runtime_minutes, wait_minutes, cycle_total_runs)"
                          " VALUES ";
        for (size_t i = 0; i < run.step_tails.size(); ++i) {
            sql += i == 0 ? "(" : ",(";
            sql += plan_id;
            sql += run.step_tails[i];
        }
        if (!client.execute(sql, error)) {
            return transaction_failed(client, error);
        }
    }
    return client.execute("COMMIT", error);
}

bool sync_battery(MariaDbClient &client, const RunData &run, std::string &battery_id, std::string &error)
{
    uint64_t insert_id = 0;
    if (!client.execute(
            "INSERT INTO ebc_battery (label, manufacturer, nominal_capacity_ah, serial_number, manufactured_on,"
            " notes, created_at_utc, updated_at_utc) VALUES " + run.battery_tuple +
                " ON DUPLICATE KEY UPDATE battery_id = LAST_INSERT_ID(battery_id),"
                " manufacturer = IFNULL(VALUES(manufacturer), manufacturer),"
                " nominal_capacity_ah = IFNULL(VALUES(nominal_capacity_ah), nominal_capacity_ah),"
                " serial_number = IFNULL(VALUES(serial_number), serial_number),"
                " manufactured_on = IFNULL(VALUES(manufactured_on), manufactured_on),"
                " notes = IFNULL(VALUES(notes), notes),"
                " updated_at_utc = IFNULL(VALUES(updated_at_utc), updated_at_utc)",
            error, nullptr, &insert_id)) {
        return false;
    }
    if (insert_id != 0) {
        battery_id = std::to_string(insert_id);
        return true;
    }
    std::vector<std::vector<std::string>> rows;
    if (!client.query("SELECT battery_id FROM ebc_battery WHERE label = " + compare_text(run.battery_label),
                      rows, error, 1)) {
        return false;
    }
    if (rows.empty()) {
        error = "Could not match battery in MariaDB";
        return false;
    }
    battery_id = rows[0][0];
    return true;
}

bool sync_run(MariaDbClient &client, const PendingRun &pending, size_t index, size_t total,
              uint32_t &measurement_total, std::string &error)
{
    if (pending.run_id.size() > REMOTE_RUN_ID_MAX) {
        error = "Run ID exceeds 36 characters";
        return false;
    }
    RunData run;
    run.run_id = pending.run_id;
    esp_err_t err = battery_database_execute(load_run_operation, &run, DATABASE_TIMEOUT);
    if (err != ESP_OK) {
        error = std::string("Local database: ") + esp_err_to_name(err);
        return false;
    }
    std::string plan_id = "NULL";
    if (run.has_plan && !sync_plan(client, run, plan_id, error)) {
        return false;
    }
    std::string battery_id = "NULL";
    if (run.has_battery && !sync_battery(client, run, battery_id, error)) {
        return false;
    }
    std::string sql = "INSERT INTO ebc_test_run (run_id, device_id, plan_id, plan_version, battery_id,"
                      " started_at_utc, ended_at_utc, status, current_step, cycle_pass, firmware_version, notes)"
                      " VALUES (";
    for (int i = 0; i < RUN_COLUMN_COUNT; ++i) {
        if (i != 0) {
            sql += ',';
        }
        sql += i == RUN_PLAN ? plan_id : i == RUN_BATTERY ? battery_id : run.run[i];
    }
    sql += ") ON DUPLICATE KEY UPDATE plan_id = VALUES(plan_id), plan_version = VALUES(plan_version),"
           " battery_id = VALUES(battery_id), ended_at_utc = VALUES(ended_at_utc), status = VALUES(status),"
           " current_step = VALUES(current_step), cycle_pass = VALUES(cycle_pass),"
           " firmware_version = VALUES(firmware_version), notes = VALUES(notes)";
    if (!client.execute(sql, error)) {
        return false;
    }

    BatchContext batch;
    batch.run_id = pending.run_id;
    batch.run_literal = run.run[RUN_ID];
    int64_t sequence = pending.synced_sequence;
    int64_t segment = pending.synced_segment;
    uint32_t run_measurements = 0;
    for (;;) {
        batch.after_sequence = sequence;
        batch.after_segment = segment;
        err = battery_database_execute(batch_operation, &batch, DATABASE_TIMEOUT);
        if (err != ESP_OK) {
            error = std::string("Local database: ") + esp_err_to_name(err);
            return false;
        }
        batch.save = false;
        if (batch.segment_count == 0 && batch.measurement_count == 0) {
            break;
        }
        if (batch.segment_count > 0) {
            if (!client.execute("INSERT IGNORE INTO ebc_run_segment (run_id, segment_number, step_number,"
                                " cycle_pass, setpoint_1, setpoint_2, setpoint_3, first_sequence) VALUES " +
                                    batch.segments,
                                error)) {
                return false;
            }
            segment = batch.last_segment;
        }
        if (batch.measurement_count > 0) {
            if (!client.execute("INSERT IGNORE INTO ebc_measurement (run_id, sequence_number, segment_number,"
                                " captured_at_utc, device_uptime_ms, mode_code, current_a, voltage_v,"
                                " capacity_ah, model_id) VALUES " + batch.measurements,
                                error)) {
                return false;
            }
            sequence = batch.last_sequence;
            run_measurements += static_cast<uint32_t>(batch.measurement_count);
        }
        batch.save = true;
        batch.save_sequence = sequence;
        batch.save_segment = segment;
        char message[128];
        snprintf(message, sizeof(message), "Test run %u/%u (%s): %" PRIu32 " measurements transferred",
                 static_cast<unsigned>(index + 1), static_cast<unsigned>(total), pending.run_id.c_str(),
                 run_measurements);
        update_status(State::Syncing, message);
    }
    if (!run.resistance_tuples.empty()) {
        if (!client.execute(
            "INSERT INTO ebc_resistance_result (run_id, result_number, captured_at_utc, device_uptime_ms, "
            "requested_seconds, actual_seconds, baseline_voltage_v, baseline_current_a, load_voltage_v, "
            "load_current_a, resistance_mohm, onset_uncertainty_seconds, valid, error) VALUES " +
            run.resistance_tuples +
            " ON DUPLICATE KEY UPDATE captured_at_utc=VALUES(captured_at_utc), "
            "device_uptime_ms=VALUES(device_uptime_ms), requested_seconds=VALUES(requested_seconds), "
            "actual_seconds=VALUES(actual_seconds), baseline_voltage_v=VALUES(baseline_voltage_v), "
            "baseline_current_a=VALUES(baseline_current_a), load_voltage_v=VALUES(load_voltage_v), "
            "load_current_a=VALUES(load_current_a), resistance_mohm=VALUES(resistance_mohm), "
            "onset_uncertainty_seconds=VALUES(onset_uncertainty_seconds), valid=VALUES(valid), error=VALUES(error)",
            error)) {
            error = "DCIR backup failed. Check MariaDB migration 006 and INSERT/UPDATE permissions: " + error;
            return false;
        }
    }
    batch.save = true;
    batch.save_sequence = sequence;
    batch.save_segment = segment;
    batch.signature = &pending.signature;
    batch.load = false;
    err = battery_database_execute(batch_operation, &batch, DATABASE_TIMEOUT);
    if (err != ESP_OK) {
        error = std::string("Could not save sync status: ") + esp_err_to_name(err);
        return false;
    }
    measurement_total += run_measurements;
    return true;
}

bool check_preconditions(const Settings &settings, std::string &error)
{
    if (settings.host.empty() || settings.user.empty() || settings.database.empty()) {
        error = "MariaDB credentials are incomplete";
        return false;
    }
    if (settings.password.empty()) {
        error = "MariaDB password has not been saved";
        return false;
    }
    return network_ready(error);
}

void run_test(const Settings &settings)
{
    update_status(State::Testing, "Verbindung wird getestet ...");
    std::string error;
    if (!check_preconditions(settings, error)) {
        finish(State::Error, error);
        return;
    }
    MariaDbClient client;
    if (!client.connect(client_config(settings), error)) {
        finish(State::Error, "Connection failed: " + error);
        return;
    }
    std::vector<std::vector<std::string>> rows;
    if (!client.query("SELECT COUNT(*) FROM ebc_test_run", rows, error, 1) || rows.empty()) {
        finish(State::Error, "Logged in, but no access to ebc_test_run: " + error);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        current_status.server_version = client.server_version();
    }
    finish(State::Ok, "Verbindung OK: MariaDB " + client.server_version() + ", " + rows[0][0] +
                          " test runs on the server");
}

void run_sync(const Settings &settings, bool full)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        current_status.last_attempt = wall_clock();
        last_attempt_us = esp_timer_get_time();
    }
    update_status(State::Syncing, full ? "Vollstaendige Uebertragung wird vorbereitet ..."
                                       : "Preparing synchronization ...");
    std::string error;
    if (!check_preconditions(settings, error)) {
        finish(State::Error, error);
        return;
    }
    PrepareContext prepare;
    prepare.full = full;
    esp_err_t err = battery_database_execute(prepare_operation, &prepare, DATABASE_TIMEOUT);
    if (err != ESP_OK) {
        finish(State::Error, std::string("Local database unavailable: ") + esp_err_to_name(err));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        current_status.pending_runs = static_cast<uint32_t>(prepare.runs.size());
    }
    if (prepare.runs.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex);
        current_status.last_success = wall_clock();
        current_status.last_runs = 0;
        current_status.last_measurements = 0;
        current_status.state = State::Ok;
        current_status.message = "Everything up to date — no new data";
        current_status.busy = false;
        return;
    }
    MariaDbClient client;
    if (!client.connect(client_config(settings), error)) {
        finish(State::Error, "Connection failed: " + error);
        return;
    }
    uint32_t measurements = 0;
    for (size_t i = 0; i < prepare.runs.size(); ++i) {
        if (!sync_run(client, prepare.runs[i], i, prepare.runs.size(), measurements, error)) {
            finish(State::Error, "Test run " + prepare.runs[i].run_id + ": " + error);
            return;
        }
        std::lock_guard<std::mutex> lock(state_mutex);
        current_status.pending_runs = static_cast<uint32_t>(prepare.runs.size() - i - 1);
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        current_status.server_version = client.server_version();
        current_status.last_success = wall_clock();
        current_status.last_runs = static_cast<uint32_t>(prepare.runs.size());
        current_status.last_measurements = measurements;
    }
    char message[96];
    snprintf(message, sizeof(message), "%u test runs synchronized, %" PRIu32 " measurements transferred",
             static_cast<unsigned>(prepare.runs.size()), measurements);
    finish(State::Ok, message);
}

/* Claims the worker for an automatic sync when enabled, due and the prerequisites are met. */
bool claim_automatic_sync()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    load_settings_locked();
    const int64_t now = esp_timer_get_time();
    if (!settings_loaded || !current_settings.enabled || current_settings.interval_minutes == 0 ||
        current_settings.password.empty() || current_status.busy || now < AUTO_START_DELAY_US ||
        !battery_database_is_ready()) {
        return false;
    }
    if (last_attempt_us != 0 &&
        now - last_attempt_us < static_cast<int64_t>(current_settings.interval_minutes) * 60 * 1000000) {
        return false;
    }
    network_status_t network = {};
    network_manager_get_status(&network);
    if (!network.connected) {
        return false;
    }
    current_status.busy = true;
    return true;
}

void worker_task(void *)
{
    for (;;) {
        uint32_t commands = 0;
        xTaskNotifyWait(0, UINT32_MAX, &commands, WORKER_POLL);
        if (commands == 0) {
            if (!claim_automatic_sync()) {
                continue;
            }
            ESP_LOGI(TAG, "Starting automatic MariaDB sync");
            commands = COMMAND_SYNC;
        }
        const Settings settings = get_settings();
        if ((commands & COMMAND_TEST) != 0) {
            run_test(settings);
        } else {
            run_sync(settings, (commands & COMMAND_FULL_SYNC) != 0);
        }
    }
}

} // namespace

Settings get_settings()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    load_settings_locked();
    return current_settings;
}

esp_err_t save_settings(const Settings &settings, bool keep_password)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    load_settings_locked();
    Settings updated = settings;
    if (keep_password) {
        updated.password = current_settings.password;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, "enabled", updated.enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, "port", updated.port);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, "interval", updated.interval_minutes);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "host", updated.host.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "user", updated.user.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "password", updated.password.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "database", updated.database.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        current_settings = std::move(updated);
        settings_loaded = true;
        ESP_LOGI(TAG, "MariaDB settings saved (%s@%s:%u/%s, auto=%d, every %u min)",
                 current_settings.user.c_str(), current_settings.host.c_str(),
                 static_cast<unsigned>(current_settings.port), current_settings.database.c_str(),
                 current_settings.enabled, static_cast<unsigned>(current_settings.interval_minutes));
    }
    return err;
}

Status get_status()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return current_status;
}

esp_err_t request(uint32_t command)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    if (worker_handle == nullptr || current_status.busy) {
        return ESP_ERR_INVALID_STATE;
    }
    current_status.busy = true;
    current_status.state = (command & COMMAND_TEST) != 0 ? State::Testing : State::Syncing;
    current_status.message = "Starting operation ...";
    xTaskNotify(worker_handle, command, eSetBits);
    return ESP_OK;
}

esp_err_t start_worker()
{
    if (worker_handle != nullptr) {
        return ESP_OK;
    }
    if (xTaskCreate(worker_task, "mariadb_sync", WORKER_STACK, nullptr, tskIDLE_PRIORITY + 1,
                    &worker_handle) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create MariaDB sync task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

} // namespace mariadb_sync

esp_err_t mariadb_sync_init(void)
{
    esp_err_t err = mariadb_sync::register_web_extension();
    if (err == ESP_OK) {
        err = mariadb_sync::start_worker();
    }
    return err;
}
