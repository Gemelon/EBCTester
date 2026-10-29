/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_store.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <memory>
#include <mutex>

#include "battery_database.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace test_store {
namespace {

const char *TAG = "test_store";
constexpr char DEVICE_ID[] = CONFIG_TEST_STORE_DEVICE_ID;
constexpr time_t VALID_TIME_THRESHOLD = 1700000000;
constexpr TickType_t SYNC_TIMEOUT = pdMS_TO_TICKS(8000);
constexpr TickType_t WRITER_TIMEOUT = pdMS_TO_TICKS(15000);
constexpr TickType_t WRITER_PERIOD = pdMS_TO_TICKS(5000);

struct RunInfo {
    std::string run_id;
    Battery battery;
    bool has_plan = false;
    Plan plan;
    time_t started = 0;
    std::string status;
    std::string notes;
    time_t ended = 0;
    std::vector<ResistanceResult> resistance;
};

enum class EventType : uint8_t { Begin, Sample, Finish };

struct Event {
    EventType type;
    Sample sample;
    std::shared_ptr<RunInfo> info;
};

/* Shared between producers and the writer task. */
std::mutex queue_mutex;
std::vector<Event> queue;
RecorderStatus recorder;
TaskHandle_t writer_task_handle;
std::atomic<SampleWrittenCallback> sample_written_callback{nullptr};

/* Owned by the writer task. */
struct WriterState {
    bool run_open = false;
    std::string run_id;
    int64_t sequence = 0;
    int segment = 0;
    int key[5] = {};
    uint16_t step = 1;
    uint16_t cycle = 1;
} writer;
bool interrupted_runs_marked;

void format_utc(time_t value, char *buffer, size_t size)
{
    struct tm parts = {};
    gmtime_r(&value, &parts);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &parts);
}

std::string now_text()
{
    char buffer[24];
    format_utc(time(nullptr), buffer, sizeof(buffer));
    return buffer;
}

int run_statement(sqlite3_stmt *statement)
{
    const int result = sqlite3_step(statement);
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return result == SQLITE_DONE ? SQLITE_OK : result;
}

void bind_text_or_null(sqlite3_stmt *statement, int index, const std::string &value)
{
    if (value.empty()) {
        sqlite3_bind_null(statement, index);
    } else {
        sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }
}

std::string column_text(sqlite3_stmt *statement, int index)
{
    const unsigned char *text = sqlite3_column_text(statement, index);
    return text != nullptr ? reinterpret_cast<const char *>(text) : "";
}

esp_err_t log_sql(sqlite3 *database, int result, const char *what)
{
    ESP_LOGE(TAG, "%s failed (%d): %s", what, result, sqlite3_errmsg(database));
    return ESP_FAIL;
}

void make_uuid(std::string &out)
{
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;
    char text[37];
    snprintf(text, sizeof(text),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
             bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    out = text;
}

/* ---------- Batteries ---------- */

int upsert_battery(sqlite3 *database, const Battery &battery, sqlite3_int64 *battery_id)
{
    sqlite3_stmt *statement = nullptr;
    int result = sqlite3_prepare_v2(
        database,
        "INSERT INTO ebc_battery (label, manufacturer, nominal_capacity_ah, serial_number, "
        "manufactured_on, notes, created_at_utc) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
        "ON CONFLICT (label) DO UPDATE SET "
        "manufacturer = COALESCE(excluded.manufacturer, manufacturer), "
        "nominal_capacity_ah = COALESCE(excluded.nominal_capacity_ah, nominal_capacity_ah), "
        "serial_number = COALESCE(excluded.serial_number, serial_number), "
        "manufactured_on = COALESCE(excluded.manufactured_on, manufactured_on), "
        "notes = COALESCE(excluded.notes, notes), "
        "updated_at_utc = excluded.created_at_utc;",
        -1, &statement, nullptr);
    if (result == SQLITE_OK) {
        const std::string now = now_text();
        sqlite3_bind_text(statement, 1, battery.label.c_str(), -1, SQLITE_TRANSIENT);
        bind_text_or_null(statement, 2, battery.manufacturer);
        if (battery.nominal_capacity_ah > 0.0) {
            sqlite3_bind_double(statement, 3, battery.nominal_capacity_ah);
        }
        bind_text_or_null(statement, 4, battery.serial_number);
        bind_text_or_null(statement, 5, battery.manufactured_on);
        bind_text_or_null(statement, 6, battery.notes);
        sqlite3_bind_text(statement, 7, now.c_str(), -1, SQLITE_TRANSIENT);
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    statement = nullptr;
    if (result == SQLITE_OK && battery_id != nullptr) {
        result = sqlite3_prepare_v2(database, "SELECT battery_id FROM ebc_battery WHERE label = ?1;", -1,
                                    &statement, nullptr);
        if (result == SQLITE_OK) {
            sqlite3_bind_text(statement, 1, battery.label.c_str(), -1, SQLITE_TRANSIENT);
            result = sqlite3_step(statement);
            if (result == SQLITE_ROW) {
                *battery_id = sqlite3_column_int64(statement, 0);
                result = SQLITE_OK;
            } else if (result == SQLITE_DONE) {
                result = SQLITE_NOTFOUND;
            }
        }
        sqlite3_finalize(statement);
    }
    return result;
}

/* ---------- Plans ---------- */

int read_plan_steps(sqlite3 *database, sqlite3_int64 plan_id, std::vector<PlanStep> &steps)
{
    sqlite3_stmt *statement = nullptr;
    int result = sqlite3_prepare_v2(
        database,
        "SELECT mode, current_a, power_w, voltage_v, cutoff_current_a, max_runtime_minutes, "
        "wait_minutes, cycle_total_runs FROM ebc_test_plan_step WHERE plan_id = ?1 "
        "ORDER BY step_number LIMIT 10;",
        -1, &statement, nullptr);
    steps.clear();
    if (result == SQLITE_OK) {
        sqlite3_bind_int64(statement, 1, plan_id);
        while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
            const std::string mode = column_text(statement, 0);
            auto hundredths = [&](int column) {
                return static_cast<uint16_t>(lround(sqlite3_column_double(statement, column) * 100.0));
            };
            PlanStep step;
            if (mode == "D-CC") {
                step = {1, hundredths(1), hundredths(3), static_cast<uint16_t>(sqlite3_column_int(statement, 5))};
            } else if (mode == "D-CP") {
                step = {2, static_cast<uint16_t>(lround(sqlite3_column_double(statement, 2))), hundredths(3),
                        static_cast<uint16_t>(sqlite3_column_int(statement, 5))};
            } else if (mode == "C-CV") {
                step = {3, hundredths(1), hundredths(3), hundredths(4)};
            } else if (mode == "WAIT") {
                step = {4, 0, 0, static_cast<uint16_t>(sqlite3_column_int(statement, 6))};
            } else {
                step = {5, 0, 0, static_cast<uint16_t>(sqlite3_column_int(statement, 7))};
            }
            steps.push_back(step);
        }
        if (result == SQLITE_DONE) {
            result = SQLITE_OK;
        }
    }
    sqlite3_finalize(statement);
    return result;
}

int write_plan_steps(sqlite3 *database, sqlite3_int64 plan_id, const std::vector<PlanStep> &steps)
{
    sqlite3_stmt *statement = nullptr;
    int result = sqlite3_prepare_v2(database, "DELETE FROM ebc_test_plan_step WHERE plan_id = ?1;", -1,
                                    &statement, nullptr);
    if (result == SQLITE_OK) {
        sqlite3_bind_int64(statement, 1, plan_id);
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    statement = nullptr;
    if (result == SQLITE_OK) {
        result = sqlite3_prepare_v2(
            database,
            "INSERT INTO ebc_test_plan_step (plan_id, step_number, mode, current_a, power_w, voltage_v, "
            "cutoff_current_a, max_runtime_minutes, wait_minutes, cycle_total_runs) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);",
            -1, &statement, nullptr);
    }
    for (size_t index = 0; result == SQLITE_OK && index < steps.size(); ++index) {
        const PlanStep &step = steps[index];
        sqlite3_bind_int64(statement, 1, plan_id);
        sqlite3_bind_int(statement, 2, static_cast<int>(index) + 1);
        sqlite3_bind_text(statement, 3, step_mode_name(step.mode), -1, SQLITE_STATIC);
        switch (step.mode) {
        case 1:
            sqlite3_bind_double(statement, 4, step.p1 / 100.0);
            sqlite3_bind_double(statement, 6, step.p2 / 100.0);
            if (step.p3 > 0) {
                sqlite3_bind_int(statement, 8, step.p3);
            }
            break;
        case 2:
            sqlite3_bind_double(statement, 5, step.p1);
            sqlite3_bind_double(statement, 6, step.p2 / 100.0);
            if (step.p3 > 0) {
                sqlite3_bind_int(statement, 8, step.p3);
            }
            break;
        case 3:
            sqlite3_bind_double(statement, 4, step.p1 / 100.0);
            sqlite3_bind_double(statement, 6, step.p2 / 100.0);
            sqlite3_bind_double(statement, 7, step.p3 / 100.0);
            break;
        case 4:
            sqlite3_bind_int(statement, 9, step.p3);
            break;
        default:
            sqlite3_bind_int(statement, 10, step.p3);
            break;
        }
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    return result;
}

int find_plan(sqlite3 *database, const std::string &name, sqlite3_int64 *plan_id, int *version)
{
    sqlite3_stmt *statement = nullptr;
    int result = sqlite3_prepare_v2(
        database, "SELECT plan_id, plan_version FROM ebc_test_plan WHERE device_id = ?1 AND plan_name = ?2;",
        -1, &statement, nullptr);
    if (result == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, DEVICE_ID, -1, SQLITE_STATIC);
        sqlite3_bind_text(statement, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        result = sqlite3_step(statement);
        if (result == SQLITE_ROW) {
            *plan_id = sqlite3_column_int64(statement, 0);
            *version = sqlite3_column_int(statement, 1);
            result = SQLITE_OK;
        } else if (result == SQLITE_DONE) {
            result = SQLITE_NOTFOUND;
        }
    }
    sqlite3_finalize(statement);
    return result;
}

int upsert_plan(sqlite3 *database, const Plan &plan, sqlite3_int64 *plan_id, int *version)
{
    const std::string definition = plan_to_definition(plan.steps);
    const std::string now = now_text();
    int result = find_plan(database, plan.name, plan_id, version);
    sqlite3_stmt *statement = nullptr;
    if (result == SQLITE_NOTFOUND) {
        result = sqlite3_prepare_v2(
            database,
            "INSERT INTO ebc_test_plan (device_id, plan_name, plan_version, plan_definition, created_at_utc) "
            "VALUES (?1, ?2, 1, ?3, ?4);",
            -1, &statement, nullptr);
        if (result == SQLITE_OK) {
            sqlite3_bind_text(statement, 1, DEVICE_ID, -1, SQLITE_STATIC);
            sqlite3_bind_text(statement, 2, plan.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 3, definition.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 4, now.c_str(), -1, SQLITE_TRANSIENT);
            result = run_statement(statement);
        }
        sqlite3_finalize(statement);
        if (result == SQLITE_OK) {
            *plan_id = sqlite3_last_insert_rowid(database);
            *version = 1;
            result = write_plan_steps(database, *plan_id, plan.steps);
        }
        return result;
    }
    if (result != SQLITE_OK) {
        return result;
    }
    std::vector<PlanStep> existing;
    result = read_plan_steps(database, *plan_id, existing);
    if (result != SQLITE_OK) {
        return result;
    }
    const bool changed = plan_to_definition(existing) != definition;
    if (changed) {
        *version += 1;
    }
    result = sqlite3_prepare_v2(
        database,
        "UPDATE ebc_test_plan SET plan_version = ?2, plan_definition = ?3, "
        "updated_at_utc = CASE WHEN ?5 THEN ?4 ELSE updated_at_utc END WHERE plan_id = ?1;",
        -1, &statement, nullptr);
    if (result == SQLITE_OK) {
        sqlite3_bind_int64(statement, 1, *plan_id);
        sqlite3_bind_int(statement, 2, *version);
        sqlite3_bind_text(statement, 3, definition.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 4, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 5, changed ? 1 : 0);
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    if (result == SQLITE_OK && changed) {
        result = write_plan_steps(database, *plan_id, plan.steps);
    }
    return result;
}

/* Runs a callback inside BEGIN IMMEDIATE ... COMMIT. */
template <typename Function> int in_transaction(sqlite3 *database, Function function)
{
    int result = sqlite3_exec(database, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
    if (result != SQLITE_OK) {
        return result;
    }
    result = function();
    if (result == SQLITE_OK) {
        result = sqlite3_exec(database, "COMMIT;", nullptr, nullptr, nullptr);
    }
    if (result != SQLITE_OK) {
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    return result;
}

/* Wraps a C++ lambda as a battery_database operation. */
template <typename Function> esp_err_t execute(Function function, TickType_t timeout = SYNC_TIMEOUT)
{
    return battery_database_execute(
        [](sqlite3 *database, void *context) { return (*static_cast<Function *>(context))(database); },
        &function, timeout);
}

/* ---------- Writer task ---------- */

int mark_interrupted_runs(sqlite3 *database)
{
    sqlite3_stmt *statement = nullptr;
    int result = sqlite3_prepare_v2(
        database,
        "UPDATE ebc_test_run SET status = 'INTERRUPTED', "
        "ended_at_utc = COALESCE((SELECT MAX(captured_at_utc) FROM ebc_measurement m "
        "WHERE m.run_id = ebc_test_run.run_id), started_at_utc), "
        "notes = COALESCE(notes, 'Neustart waehrend der Aufzeichnung') "
        "WHERE device_id = ?1 AND status = 'RUNNING';",
        -1, &statement, nullptr);
    if (result == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, DEVICE_ID, -1, SQLITE_STATIC);
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    if (result == SQLITE_OK && sqlite3_changes(database) > 0) {
        ESP_LOGW(TAG, "Marked %d unfinished test run(s) as INTERRUPTED", sqlite3_changes(database));
    }
    return result;
}

int write_begin(sqlite3 *database, const RunInfo &info)
{
    sqlite3_int64 battery_id = 0;
    sqlite3_int64 plan_id = 0;
    int plan_version = 0;
    int result = SQLITE_OK;
    if (!info.battery.label.empty()) {
        result = upsert_battery(database, info.battery, &battery_id);
    }
    if (result == SQLITE_OK && info.has_plan) {
        result = upsert_plan(database, info.plan, &plan_id, &plan_version);
    }
    if (result != SQLITE_OK) {
        return result;
    }
    char started[24];
    format_utc(info.started, started, sizeof(started));
    sqlite3_stmt *statement = nullptr;
    result = sqlite3_prepare_v2(
        database,
        "INSERT INTO ebc_test_run (run_id, device_id, plan_id, plan_version, battery_id, started_at_utc, "
        "status, current_step, cycle_pass, firmware_version, notes) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'RUNNING', 1, 1, ?7, ?8);",
        -1, &statement, nullptr);
    if (result == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, info.run_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, DEVICE_ID, -1, SQLITE_STATIC);
        if (plan_id > 0) {
            sqlite3_bind_int64(statement, 3, plan_id);
            sqlite3_bind_int(statement, 4, plan_version);
        }
        if (battery_id > 0) {
            sqlite3_bind_int64(statement, 5, battery_id);
        }
        sqlite3_bind_text(statement, 6, started, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 7, esp_app_get_description()->version, -1, SQLITE_STATIC);
        bind_text_or_null(statement, 8, info.notes);
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    return result;
}

struct SampleStatements {
    sqlite3_stmt *segment = nullptr;
    sqlite3_stmt *measurement = nullptr;
    ~SampleStatements()
    {
        sqlite3_finalize(segment);
        sqlite3_finalize(measurement);
    }
};

int write_sample(sqlite3 *database, SampleStatements &statements, const Sample &sample)
{
    int result = SQLITE_OK;
    if (statements.segment == nullptr) {
        result = sqlite3_prepare_v2(
            database,
            "INSERT INTO ebc_run_segment (run_id, segment_number, step_number, cycle_pass, setpoint_1, "
            "setpoint_2, setpoint_3, first_sequence) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);",
            -1, &statements.segment, nullptr);
        if (result == SQLITE_OK) {
            result = sqlite3_prepare_v2(
                database,
                "INSERT INTO ebc_measurement (run_id, sequence_number, segment_number, captured_at_utc, "
                "device_uptime_ms, mode_code, current_a, voltage_v, capacity_ah, model_id) "
                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, round(?7, 3), round(?8, 3), round(?9, 4), ?10);",
                -1, &statements.measurement, nullptr);
        }
        if (result != SQLITE_OK) {
            return result;
        }
    }
    const int key[5] = {sample.step, sample.cycle, static_cast<int>(lroundf(sample.setpoint_1 * 100)),
                        static_cast<int>(lroundf(sample.setpoint_2 * 100)),
                        static_cast<int>(lroundf(sample.setpoint_3 * 100))};
    const int64_t sequence = writer.sequence + 1;
    if (writer.segment == 0 || memcmp(key, writer.key, sizeof(key)) != 0) {
        sqlite3_stmt *statement = statements.segment;
        sqlite3_bind_text(statement, 1, writer.run_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2, writer.segment + 1);
        sqlite3_bind_int(statement, 3, sample.step);
        sqlite3_bind_int(statement, 4, sample.cycle);
        sqlite3_bind_double(statement, 5, key[2] / 100.0);
        sqlite3_bind_double(statement, 6, key[3] / 100.0);
        sqlite3_bind_double(statement, 7, key[4] / 100.0);
        sqlite3_bind_int64(statement, 8, sequence);
        result = run_statement(statement);
        if (result != SQLITE_OK) {
            return result;
        }
        writer.segment += 1;
        memcpy(writer.key, key, sizeof(key));
    }
    char captured[24];
    sqlite3_stmt *statement = statements.measurement;
    sqlite3_bind_text(statement, 1, writer.run_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, sequence);
    sqlite3_bind_int(statement, 3, writer.segment);
    if (sample.utc >= VALID_TIME_THRESHOLD) {
        format_utc(sample.utc, captured, sizeof(captured));
        sqlite3_bind_text(statement, 4, captured, -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(statement, 5, sample.uptime_ms);
    sqlite3_bind_int(statement, 6, sample.mode);
    sqlite3_bind_double(statement, 7, sample.current_a);
    sqlite3_bind_double(statement, 8, sample.voltage_v);
    sqlite3_bind_double(statement, 9, sample.capacity_ah);
    sqlite3_bind_int(statement, 10, sample.model);
    result = run_statement(statement);
    if (result == SQLITE_OK) {
        writer.sequence = sequence;
        writer.step = sample.step;
        writer.cycle = sample.cycle;
    }
    return result;
}

int write_progress(sqlite3 *database)
{
    sqlite3_stmt *statement = nullptr;
    int result = sqlite3_prepare_v2(
        database, "UPDATE ebc_test_run SET current_step = ?2, cycle_pass = ?3 WHERE run_id = ?1;", -1,
        &statement, nullptr);
    if (result == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, writer.run_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2, writer.step);
        sqlite3_bind_int(statement, 3, writer.cycle);
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    return result;
}

int write_finish(sqlite3 *database, const RunInfo &info)
{
    char ended[24];
    format_utc(info.ended, ended, sizeof(ended));
    sqlite3_stmt *statement = nullptr;
    int result = sqlite3_prepare_v2(
        database,
        "UPDATE ebc_test_run SET status = ?2, ended_at_utc = ?3, current_step = ?4, cycle_pass = ?5, "
        "notes = COALESCE(?6, notes) WHERE run_id = ?1;",
        -1, &statement, nullptr);
    if (result == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, writer.run_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, info.status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, ended, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 4, writer.step);
        sqlite3_bind_int(statement, 5, writer.cycle);
        bind_text_or_null(statement, 6, info.notes);
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    return result;
}

int write_resistance(sqlite3 *database, const RunInfo &info)
{
    if (info.resistance.empty()) return SQLITE_OK;
    sqlite3_stmt *statement = nullptr;
    int result = sqlite3_prepare_v2(database,
        "INSERT INTO ebc_resistance_result (run_id, result_number, captured_at_utc, device_uptime_ms, "
        "requested_seconds, actual_seconds, baseline_voltage_v, baseline_current_a, load_voltage_v, "
        "load_current_a, resistance_mohm, onset_uncertainty_seconds, valid, error) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14);",
        -1, &statement, nullptr);
    for (const auto &r : info.resistance) {
        if (result != SQLITE_OK) break;
        sqlite3_bind_text(statement, 1, writer.run_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2, r.number);
        if (r.captured) {
            if (r.epoch_ms > 0) {
                char utc[24], timestamp[32];
                format_utc(static_cast<time_t>(r.epoch_ms / 1000), utc, sizeof(utc));
                snprintf(timestamp, sizeof(timestamp), "%s.%03d", utc, static_cast<int>(r.epoch_ms % 1000));
                sqlite3_bind_text(statement, 3, timestamp, -1, SQLITE_TRANSIENT);
            }
            sqlite3_bind_int64(statement, 4, r.uptime_ms);
            sqlite3_bind_double(statement, 6, r.actual_seconds);
            sqlite3_bind_double(statement, 9, r.load_voltage_v);
            sqlite3_bind_double(statement, 10, r.load_current_a);
        }
        sqlite3_bind_double(statement, 5, r.requested_seconds);
        sqlite3_bind_double(statement, 7, r.baseline_voltage_v);
        sqlite3_bind_double(statement, 8, r.baseline_current_a);
        if (r.valid) sqlite3_bind_double(statement, 11, r.resistance_mohm);
        sqlite3_bind_double(statement, 12, r.onset_uncertainty_seconds);
        sqlite3_bind_int(statement, 13, r.valid ? 1 : 0);
        bind_text_or_null(statement, 14, r.error);
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    return result;
}

struct BatchResult {
    bool executed = false;
    uint32_t written = 0;
    std::vector<Sample> written_samples;
    std::string error;
};

/* Writes a batch of events in one transaction; on failure the open run is abandoned. */
esp_err_t write_batch(sqlite3 *database, std::vector<Event> &events, BatchResult &batch)
{
    batch.executed = true;
    if (!interrupted_runs_marked) {
        if (mark_interrupted_runs(database) == SQLITE_OK) {
            interrupted_runs_marked = true;
        }
    }
    const WriterState saved = writer;
    const int result = in_transaction(database, [&]() {
        SampleStatements statements;
        int status = SQLITE_OK;
        for (const Event &event : events) {
            switch (event.type) {
            case EventType::Begin:
                writer = {};
                writer.run_id = event.info->run_id;
                status = write_begin(database, *event.info);
                writer.run_open = status == SQLITE_OK;
                break;
            case EventType::Sample:
                if (writer.run_open) {
                    status = write_sample(database, statements, event.sample);
                    if (status == SQLITE_OK) {
                        batch.written++;
                        batch.written_samples.push_back(event.sample);
                    }
                }
                break;
            case EventType::Finish:
                if (writer.run_open) {
                    status = write_resistance(database, *event.info);
                    if (status == SQLITE_OK) status = write_finish(database, *event.info);
                    writer.run_open = false;
                }
                break;
            }
            if (status != SQLITE_OK) {
                return status;
            }
        }
        return writer.run_open && batch.written > 0 ? write_progress(database) : SQLITE_OK;
    });
    if (result != SQLITE_OK) {
        batch.error = sqlite3_errmsg(database);
        batch.written = 0;
        batch.written_samples.clear();
        log_sql(database, result, "Writing test run data");
        writer = saved;
        writer.run_open = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void writer_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, WRITER_PERIOD);
        if (!battery_database_is_ready()) {
            continue;
        }
        std::vector<Event> events;
        {
            std::lock_guard<std::mutex> guard(queue_mutex);
            events.swap(queue);
        }
        if (events.empty() && interrupted_runs_marked) {
            continue;
        }
        BatchResult batch;
        const esp_err_t err = execute(
            [&](sqlite3 *database) { return write_batch(database, events, batch); }, WRITER_TIMEOUT);
        if (err == ESP_OK) {
            const SampleWrittenCallback callback = sample_written_callback.load();
            if (callback != nullptr) {
                for (const Sample &sample : batch.written_samples) {
                    callback(sample);
                }
            }
        }
        std::lock_guard<std::mutex> guard(queue_mutex);
        if (!batch.executed) {
            /* Database busy or not available: keep the events for the next attempt. */
            ESP_LOGW(TAG, "Database not available (%s); %u events kept", esp_err_to_name(err),
                     static_cast<unsigned>(events.size()));
            events.insert(events.end(), queue.begin(), queue.end());
            queue.swap(events);
            continue;
        }
        recorder.samples_written += batch.written;
        if (err != ESP_OK) {
            recorder.failed = true;
            recorder.error = batch.error.empty() ? "Schreibfehler" : batch.error;
        }
        size_t pending = 0;
        for (const Event &event : queue) {
            pending += event.type == EventType::Sample ? 1 : 0;
        }
        recorder.samples_pending = pending;
    }
}

void enqueue(Event &&event)
{
    std::lock_guard<std::mutex> guard(queue_mutex);
    if (event.type == EventType::Sample) {
        if (recorder.samples_pending >= CONFIG_TEST_STORE_MAX_PENDING_SAMPLES) {
            recorder.samples_dropped++;
            return;
        }
        recorder.samples_pending++;
    }
    queue.push_back(std::move(event));
}

} // namespace

const char *step_mode_name(uint8_t mode)
{
    switch (mode) {
    case 1:
        return "D-CC";
    case 2:
        return "D-CP";
    case 3:
        return "C-CV";
    case 4:
        return "WAIT";
    case 5:
        return "CYCLE";
    default:
        return "?";
    }
}

std::string validate_plan(const std::vector<PlanStep> &steps)
{
    if (steps.empty()) {
        return "The plan contains no steps.";
    }
    if (steps.size() > MAX_PLAN_STEPS) {
        return "Up to 10 steps are allowed.";
    }
    bool cycle_seen = false;
    for (size_t index = 0; index < steps.size(); ++index) {
        const PlanStep &step = steps[index];
        const std::string row = "Step " + std::to_string(index + 1) + ": ";
        switch (step.mode) {
        case 1:
        case 2:
            if (step.p1 < 1 || step.p1 > (step.mode == 1 ? 5000 : 250) ||
                step.p2 < 1 || step.p2 > 3000 || step.p3 > 239) {
                return row + (step.mode == 1 ? "Current 0.01–50 A" : "Power 1–250 W") +
                       ", cutoff voltage 0.01–30 V, time 0–239 min.";
            }
            break;
        case 3:
            if (step.p1 < 1 || step.p1 > 5000 || step.p2 < 1 || step.p2 > 3000 ||
                step.p3 < 1 || step.p3 > 5000) {
                return row + "Current/cutoff 0.01–50 A, charge voltage 0.01–30 V.";
            }
            break;
        case 4:
            if (step.p3 < 1 || step.p3 > 239) {
                return row + "Wait time 1–239 min.";
            }
            break;
        case 5:
            if (index == 0) {
                return row + "A cycle cannot be the first step.";
            }
            if (cycle_seen) {
                return row + "Only one cycle step is allowed.";
            }
            if (step.p3 < 1 || step.p3 > 239) {
                return row + "Durchlaeufe 1-239.";
            }
            cycle_seen = true;
            break;
        default:
            return row + "Unknown operating mode.";
        }
    }
    return "";
}

std::string plan_to_definition(const std::vector<PlanStep> &steps)
{
    std::string definition;
    for (const PlanStep &step : steps) {
        char row[32];
        snprintf(row, sizeof(row), "%s%u,%u,%u,%u", definition.empty() ? "" : ";", step.mode, step.p1,
                 step.p2, step.p3);
        definition += row;
    }
    return definition;
}

bool plan_from_definition(const std::string &definition, std::vector<PlanStep> &steps)
{
    steps.clear();
    const char *cursor = definition.c_str();
    while (*cursor != '\0') {
        unsigned values[4];
        int consumed = 0;
        if (sscanf(cursor, "%u,%u,%u,%u%n", &values[0], &values[1], &values[2], &values[3], &consumed) != 4 ||
            values[0] > 5 || values[1] > 65535 || values[2] > 65535 || values[3] > 65535 ||
            steps.size() >= MAX_PLAN_STEPS) {
            steps.clear();
            return false;
        }
        steps.push_back({static_cast<uint8_t>(values[0]), static_cast<uint16_t>(values[1]),
                         static_cast<uint16_t>(values[2]), static_cast<uint16_t>(values[3])});
        cursor += consumed;
        if (*cursor == ';') {
            ++cursor;
        } else if (*cursor != '\0') {
            steps.clear();
            return false;
        }
    }
    return true;
}

esp_err_t list_batteries(std::vector<std::string> &labels)
{
    labels.clear();
    return execute([&](sqlite3 *database) {
        sqlite3_stmt *statement = nullptr;
        int result = sqlite3_prepare_v2(database, "SELECT label FROM ebc_battery ORDER BY label LIMIT 200;", -1,
                                        &statement, nullptr);
        while (result == SQLITE_OK && (result = sqlite3_step(statement)) == SQLITE_ROW) {
            labels.push_back(column_text(statement, 0));
            result = SQLITE_OK;
        }
        sqlite3_finalize(statement);
        return result == SQLITE_DONE || result == SQLITE_OK ? ESP_OK : log_sql(database, result, "Listing batteries");
    });
}

esp_err_t load_battery(const std::string &label, Battery &battery)
{
    return execute([&](sqlite3 *database) {
        sqlite3_stmt *statement = nullptr;
        int result = sqlite3_prepare_v2(
            database,
            "SELECT label, manufacturer, nominal_capacity_ah, serial_number, manufactured_on, notes "
            "FROM ebc_battery WHERE label = ?1;",
            -1, &statement, nullptr);
        esp_err_t err = ESP_OK;
        if (result == SQLITE_OK) {
            sqlite3_bind_text(statement, 1, label.c_str(), -1, SQLITE_TRANSIENT);
            result = sqlite3_step(statement);
        }
        if (result == SQLITE_ROW) {
            battery.label = column_text(statement, 0);
            battery.manufacturer = column_text(statement, 1);
            battery.nominal_capacity_ah = sqlite3_column_double(statement, 2);
            battery.serial_number = column_text(statement, 3);
            battery.manufactured_on = column_text(statement, 4);
            battery.notes = column_text(statement, 5);
        } else if (result == SQLITE_DONE) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            err = log_sql(database, result, "Loading battery");
        }
        sqlite3_finalize(statement);
        return err;
    });
}

esp_err_t save_battery(const Battery &battery)
{
    if (battery.label.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    return execute([&](sqlite3 *database) {
        const int result = upsert_battery(database, battery, nullptr);
        return result == SQLITE_OK ? ESP_OK : log_sql(database, result, "Saving battery");
    });
}

esp_err_t list_plans(std::vector<std::string> &names)
{
    names.clear();
    return execute([&](sqlite3 *database) {
        sqlite3_stmt *statement = nullptr;
        int result = sqlite3_prepare_v2(
            database, "SELECT plan_name FROM ebc_test_plan WHERE device_id = ?1 ORDER BY plan_name LIMIT 100;",
            -1, &statement, nullptr);
        if (result == SQLITE_OK) {
            sqlite3_bind_text(statement, 1, DEVICE_ID, -1, SQLITE_STATIC);
        }
        while (result == SQLITE_OK && (result = sqlite3_step(statement)) == SQLITE_ROW) {
            names.push_back(column_text(statement, 0));
            result = SQLITE_OK;
        }
        sqlite3_finalize(statement);
        return result == SQLITE_DONE || result == SQLITE_OK ? ESP_OK : log_sql(database, result, "Listing plans");
    });
}

esp_err_t load_plan(const std::string &name, Plan &plan)
{
    return execute([&](sqlite3 *database) {
        sqlite3_int64 plan_id = 0;
        int version = 0;
        int result = find_plan(database, name, &plan_id, &version);
        if (result == SQLITE_NOTFOUND) {
            return ESP_ERR_NOT_FOUND;
        }
        if (result == SQLITE_OK) {
            result = read_plan_steps(database, plan_id, plan.steps);
        }
        if (result != SQLITE_OK) {
            return log_sql(database, result, "Loading plan");
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT plan_name FROM ebc_test_plan WHERE plan_id = ?1;", -1,
                               &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, plan_id);
            plan.name = sqlite3_step(statement) == SQLITE_ROW ? column_text(statement, 0) : name;
        }
        sqlite3_finalize(statement);
        return ESP_OK;
    });
}

esp_err_t save_plan(const Plan &plan)
{
    if (plan.name.empty() || !validate_plan(plan.steps).empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    return execute([&](sqlite3 *database) {
        sqlite3_int64 plan_id = 0;
        int version = 0;
        const int result = in_transaction(database, [&]() { return upsert_plan(database, plan, &plan_id, &version); });
        if (result != SQLITE_OK) {
            return log_sql(database, result, "Saving plan");
        }
        ESP_LOGI(TAG, "Plan '%s' saved as version %d", plan.name.c_str(), version);
        return ESP_OK;
    });
}

esp_err_t begin_run(const Battery &battery, const Plan *plan, std::string &run_id, const char *notes)
{
    if (writer_task_handle == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    auto info = std::make_shared<RunInfo>();
    make_uuid(info->run_id);
    info->battery = battery;
    info->notes = notes != nullptr ? notes : "";
    info->has_plan = plan != nullptr && !plan->name.empty();
    if (info->has_plan) {
        info->plan = *plan;
    }
    info->started = time(nullptr);
    run_id = info->run_id;
    {
        std::lock_guard<std::mutex> guard(queue_mutex);
        recorder.active = true;
        recorder.failed = false;
        recorder.error.clear();
        recorder.run_id = run_id;
        recorder.samples_written = 0;
        recorder.samples_dropped = 0;
    }
    ESP_LOGI(TAG, "Recording test run %s", run_id.c_str());
    enqueue({EventType::Begin, {}, info});
    xTaskNotifyGive(writer_task_handle);
    return ESP_OK;
}

void add_sample(const Sample &sample)
{
    {
        std::lock_guard<std::mutex> guard(queue_mutex);
        if (!recorder.active) {
            return;
        }
    }
    enqueue({EventType::Sample, sample, nullptr});
}

void set_sample_written_callback(SampleWrittenCallback callback)
{
    sample_written_callback.store(callback);
}

void finish_run(const char *status, const char *notes)
{
    finish_resistance_run(status, notes, {});
}

void finish_resistance_run(const char *status, const char *notes, const std::vector<ResistanceResult> &results)
{
    auto info = std::make_shared<RunInfo>();
    info->status = status;
    info->notes = notes != nullptr ? notes : "";
    info->ended = time(nullptr);
    info->resistance = results;
    {
        std::lock_guard<std::mutex> guard(queue_mutex);
        if (!recorder.active) {
            return;
        }
        recorder.active = false;
    }
    ESP_LOGI(TAG, "Test run finished: %s", status);
    enqueue({EventType::Finish, {}, info});
    xTaskNotifyGive(writer_task_handle);
}

RecorderStatus recorder_status()
{
    std::lock_guard<std::mutex> guard(queue_mutex);
    return recorder;
}

} // namespace test_store

extern "C" esp_err_t test_store_init(void)
{
    using namespace test_store;
    if (writer_task_handle != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    queue.reserve(64);
    if (xTaskCreate(writer_task, "test_store", 12288, nullptr, 3, &writer_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
