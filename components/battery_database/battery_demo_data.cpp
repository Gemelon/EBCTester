#include "battery_database.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "battery_demo";

namespace {

constexpr char DEMO_DEVICE_ID[] = "EBC-A20";
constexpr char DEMO_PLAN_NAME[] = "Demo capacity test";
constexpr double NOMINAL_CAPACITY_AH = 20.0;
constexpr double INTERNAL_RESISTANCE_OHM = 0.012;
constexpr int SAMPLE_INTERVAL_S = 30;
constexpr int MAX_SEGMENT_SAMPLES = 8 * 3600 / SAMPLE_INTERVAL_S;
constexpr time_t FALLBACK_START_UTC = 1767254400; /* 2026-01-01 08:00:00 UTC */
constexpr time_t VALID_TIME_THRESHOLD = 1700000000;

enum class StepMode { ChargeCv, DischargeCc, Wait };

struct DemoStep {
    StepMode mode;
    const char *mode_name;
    double current_a;
    double voltage_v;
    double cutoff_current_a;
    int wait_minutes;
};

constexpr DemoStep DEMO_STEPS[] = {
    {StepMode::ChargeCv, "C-CV", 5.0, 3.60, 1.0, 0},
    {StepMode::Wait, "WAIT", 0.0, 0.0, 0.0, 30},
    {StepMode::DischargeCc, "D-CC", 10.0, 2.60, 0.0, 0},
    {StepMode::Wait, "WAIT", 0.0, 0.0, 0.0, 30},
    {StepMode::ChargeCv, "C-CV", 5.0, 3.45, 1.0, 0},
};
constexpr size_t STEP_COUNT = sizeof(DEMO_STEPS) / sizeof(DEMO_STEPS[0]);
constexpr size_t MAX_SAMPLES = STEP_COUNT * (MAX_SEGMENT_SAMPLES + 1);

struct Sample {
    int64_t elapsed_ms;
    float current_a;
    float voltage_v;
    float capacity_ah;
    uint8_t segment;
    uint8_t mode_code;
};

struct DemoContext {
    Sample *samples;
    size_t sample_count;
    uint32_t first_sequence[STEP_COUNT];
    time_t start_utc;
    char run_id[24];
};

double open_circuit_voltage(double soc)
{
    if (soc < 0.1) {
        return 2.5 + soc / 0.1 * 0.7;
    }
    if (soc < 0.9) {
        return 3.2 + (soc - 0.1) / 0.8 * 0.13;
    }
    return 3.33 + (soc - 0.9) / 0.1 * 0.27;
}

double noise(double amplitude)
{
    return ((double)(esp_random() % 2001) / 1000.0 - 1.0) * amplitude;
}

uint8_t active_mode_code(StepMode mode)
{
    return mode == StepMode::ChargeCv ? 0x0C : 0x0A;
}

uint8_t complete_mode_code(StepMode mode)
{
    return mode == StepMode::ChargeCv ? 0x16 : 0x14;
}

/* Simple LiFePO4 model: OCV curve plus ohmic drop, CC/CV charging and CC discharge. */
void simulate(DemoContext &context)
{
    double soc = 0.2;
    double capacity = 0.0;
    double last_voltage = open_circuit_voltage(soc);
    uint8_t last_mode = 0x16;
    int64_t elapsed_ms = 0;
    size_t count = 0;

    for (size_t step = 0; step < STEP_COUNT; ++step) {
        const DemoStep &definition = DEMO_STEPS[step];
        context.first_sequence[step] = (uint32_t)count + 1;
        bool constant_voltage = false;
        double wait_start_voltage = last_voltage;
        if (definition.mode != StepMode::Wait) {
            capacity = 0.0;
        }
        int wait_samples = definition.wait_minutes * 60 / SAMPLE_INTERVAL_S;
        for (int index = 0; index <= MAX_SEGMENT_SAMPLES; ++index) {
            double ocv = open_circuit_voltage(soc);
            double current = 0.0;
            double voltage = ocv;
            bool finished = index == MAX_SEGMENT_SAMPLES;
            uint8_t mode_code = last_mode;
            switch (definition.mode) {
            case StepMode::ChargeCv:
                current = definition.current_a;
                voltage = ocv + current * INTERNAL_RESISTANCE_OHM;
                if (constant_voltage || voltage >= definition.voltage_v) {
                    constant_voltage = true;
                    current = fmin(definition.current_a,
                                   (definition.voltage_v - ocv) / INTERNAL_RESISTANCE_OHM);
                    voltage = definition.voltage_v;
                    finished = finished || current <= definition.cutoff_current_a;
                }
                mode_code = active_mode_code(definition.mode);
                break;
            case StepMode::DischargeCc:
                current = definition.current_a;
                voltage = ocv - current * INTERNAL_RESISTANCE_OHM;
                finished = finished || voltage <= definition.voltage_v;
                mode_code = active_mode_code(definition.mode);
                break;
            case StepMode::Wait:
                voltage = ocv + (wait_start_voltage - ocv) *
                                    exp(-(double)index * SAMPLE_INTERVAL_S / 300.0);
                finished = finished || index >= wait_samples;
                break;
            }
            if (finished && definition.mode != StepMode::Wait) {
                mode_code = complete_mode_code(definition.mode);
            }
            Sample &sample = context.samples[count++];
            sample.elapsed_ms = elapsed_ms;
            sample.current_a = current > 0.0 ? (float)fmax(0.0, current + noise(0.01)) : 0.0f;
            sample.voltage_v = (float)(voltage + noise(0.002));
            sample.capacity_ah = (float)capacity;
            sample.segment = (uint8_t)(step + 1);
            sample.mode_code = mode_code;
            last_voltage = voltage;
            last_mode = mode_code;
            elapsed_ms += SAMPLE_INTERVAL_S * 1000;
            if (finished) {
                break;
            }
            double charge_ah = current * SAMPLE_INTERVAL_S / 3600.0;
            capacity += charge_ah;
            soc += (definition.mode == StepMode::DischargeCc ? -charge_ah : charge_ah) /
                   NOMINAL_CAPACITY_AH;
        }
    }
    context.sample_count = count;
}

void format_utc(time_t value, char *buffer, size_t size)
{
    struct tm parts = {};
    gmtime_r(&value, &parts);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &parts);
}

int bind_optional(sqlite3_stmt *statement, int index, double value)
{
    return value > 0.0 ? sqlite3_bind_double(statement, index, value)
                       : sqlite3_bind_null(statement, index);
}

int run_statement(sqlite3_stmt *statement)
{
    int result = sqlite3_step(statement);
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return result == SQLITE_DONE ? SQLITE_OK : result;
}

int insert_plan(sqlite3 *database, const char *created_at, sqlite3_int64 *plan_id)
{
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(
        database,
        "INSERT INTO ebc_test_plan (device_id, plan_name, plan_version, created_at_utc) "
        "VALUES (?1, ?2, 1, ?3) ON CONFLICT (device_id, plan_name) DO NOTHING;",
        -1, &statement, NULL);
    if (result == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, DEMO_DEVICE_ID, -1, SQLITE_STATIC);
        sqlite3_bind_text(statement, 2, DEMO_PLAN_NAME, -1, SQLITE_STATIC);
        sqlite3_bind_text(statement, 3, created_at, -1, SQLITE_TRANSIENT);
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    statement = NULL;
    if (result == SQLITE_OK) {
        result = sqlite3_prepare_v2(
            database, "SELECT plan_id FROM ebc_test_plan WHERE device_id = ?1 AND plan_name = ?2;",
            -1, &statement, NULL);
    }
    if (result == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, DEMO_DEVICE_ID, -1, SQLITE_STATIC);
        sqlite3_bind_text(statement, 2, DEMO_PLAN_NAME, -1, SQLITE_STATIC);
        result = sqlite3_step(statement);
        if (result == SQLITE_ROW) {
            *plan_id = sqlite3_column_int64(statement, 0);
            result = SQLITE_OK;
        } else if (result == SQLITE_DONE) {
            result = SQLITE_NOTFOUND;
        }
    }
    sqlite3_finalize(statement);
    statement = NULL;
    if (result == SQLITE_OK) {
        result = sqlite3_prepare_v2(
            database,
            "INSERT OR IGNORE INTO ebc_test_plan_step (plan_id, step_number, mode, current_a, "
            "voltage_v, cutoff_current_a, wait_minutes) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);",
            -1, &statement, NULL);
    }
    for (size_t step = 0; result == SQLITE_OK && step < STEP_COUNT; ++step) {
        const DemoStep &definition = DEMO_STEPS[step];
        sqlite3_bind_int64(statement, 1, *plan_id);
        sqlite3_bind_int(statement, 2, (int)step + 1);
        sqlite3_bind_text(statement, 3, definition.mode_name, -1, SQLITE_STATIC);
        bind_optional(statement, 4, definition.current_a);
        bind_optional(statement, 5, definition.voltage_v);
        bind_optional(statement, 6, definition.cutoff_current_a);
        if (definition.wait_minutes > 0) {
            sqlite3_bind_int(statement, 7, definition.wait_minutes);
        }
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    return result;
}

int insert_battery(sqlite3 *database, const char *created_at, sqlite3_int64 *battery_id)
{
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(
        database,
        "INSERT INTO ebc_battery (label, manufacturer, nominal_capacity_ah, serial_number, "
        "notes, created_at_utc) VALUES ("
        "'Demo LiFePO4 ' || (SELECT COUNT(*) + 1 FROM ebc_battery WHERE label LIKE 'Demo LiFePO4 %'),"
        "'Demo', ?1, ?2, 'Simulierte Testdaten', ?3);",
        -1, &statement, NULL);
    if (result == SQLITE_OK) {
        char serial[24];
        snprintf(serial, sizeof(serial), "DEMO-%08" PRIX32, esp_random());
        sqlite3_bind_double(statement, 1, NOMINAL_CAPACITY_AH);
        sqlite3_bind_text(statement, 2, serial, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, created_at, -1, SQLITE_TRANSIENT);
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    if (result == SQLITE_OK) {
        *battery_id = sqlite3_last_insert_rowid(database);
    }
    return result;
}

int insert_run(sqlite3 *database, const DemoContext &context, sqlite3_int64 plan_id,
               sqlite3_int64 battery_id)
{
    char started_at[24];
    char ended_at[24];
    format_utc(context.start_utc, started_at, sizeof(started_at));
    format_utc(context.start_utc +
                   (time_t)(context.samples[context.sample_count - 1].elapsed_ms / 1000),
               ended_at, sizeof(ended_at));
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(
        database,
        "INSERT INTO ebc_test_run (run_id, device_id, plan_id, plan_version, battery_id, "
        "started_at_utc, ended_at_utc, status, current_step, cycle_pass, firmware_version, notes) "
        "VALUES (?1, ?2, ?3, 1, ?4, ?5, ?6, 'COMPLETED', ?7, 1, ?8, 'Simulierte Testdaten');",
        -1, &statement, NULL);
    if (result == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, context.run_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(statement, 2, DEMO_DEVICE_ID, -1, SQLITE_STATIC);
        sqlite3_bind_int64(statement, 3, plan_id);
        sqlite3_bind_int64(statement, 4, battery_id);
        sqlite3_bind_text(statement, 5, started_at, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 6, ended_at, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 7, (int)STEP_COUNT);
        sqlite3_bind_text(statement, 8, esp_app_get_description()->version, -1, SQLITE_STATIC);
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    statement = NULL;
    if (result == SQLITE_OK) {
        result = sqlite3_prepare_v2(
            database,
            "INSERT INTO ebc_run_segment (run_id, segment_number, step_number, cycle_pass, "
            "setpoint_1, setpoint_2, setpoint_3, first_sequence) "
            "VALUES (?1, ?2, ?2, 1, ?3, ?4, ?5, ?6);",
            -1, &statement, NULL);
    }
    for (size_t step = 0; result == SQLITE_OK && step < STEP_COUNT; ++step) {
        const DemoStep &definition = DEMO_STEPS[step];
        sqlite3_bind_text(statement, 1, context.run_id, -1, SQLITE_STATIC);
        sqlite3_bind_int(statement, 2, (int)step + 1);
        if (definition.mode != StepMode::Wait) {
            sqlite3_bind_double(statement, 3, definition.current_a);
            sqlite3_bind_double(statement, 4, definition.voltage_v);
            sqlite3_bind_double(statement, 5, definition.cutoff_current_a);
        }
        sqlite3_bind_int64(statement, 6, context.first_sequence[step]);
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    return result;
}

int insert_measurements(sqlite3 *database, const DemoContext &context)
{
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(
        database,
        "INSERT INTO ebc_measurement (run_id, sequence_number, segment_number, captured_at_utc, "
        "device_uptime_ms, mode_code, current_a, voltage_v, capacity_ah, model_id) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, round(?7, 3), round(?8, 3), round(?9, 4), 20);",
        -1, &statement, NULL);
    for (size_t index = 0; result == SQLITE_OK && index < context.sample_count; ++index) {
        const Sample &sample = context.samples[index];
        char captured_at[24];
        format_utc(context.start_utc + (time_t)(sample.elapsed_ms / 1000), captured_at,
                   sizeof(captured_at));
        sqlite3_bind_text(statement, 1, context.run_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(statement, 2, (sqlite3_int64)index + 1);
        sqlite3_bind_int(statement, 3, sample.segment);
        sqlite3_bind_text(statement, 4, captured_at, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 5, sample.elapsed_ms);
        sqlite3_bind_int(statement, 6, sample.mode_code);
        sqlite3_bind_double(statement, 7, sample.current_a);
        sqlite3_bind_double(statement, 8, sample.voltage_v);
        sqlite3_bind_double(statement, 9, sample.capacity_ah);
        result = run_statement(statement);
    }
    sqlite3_finalize(statement);
    return result;
}

esp_err_t write_demo_run(sqlite3 *database, void *arg)
{
    DemoContext &context = *static_cast<DemoContext *>(arg);
    char created_at[24];
    format_utc(time(NULL), created_at, sizeof(created_at));
    sqlite3_int64 plan_id = 0;
    sqlite3_int64 battery_id = 0;
    int result = sqlite3_exec(database, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    if (result == SQLITE_OK) {
        result = insert_plan(database, created_at, &plan_id);
    }
    if (result == SQLITE_OK) {
        result = insert_battery(database, created_at, &battery_id);
    }
    if (result == SQLITE_OK) {
        result = insert_run(database, context, plan_id, battery_id);
    }
    if (result == SQLITE_OK) {
        result = insert_measurements(database, context);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_exec(database, "COMMIT;", NULL, NULL, NULL);
    }
    if (result != SQLITE_OK) {
        ESP_LOGE(TAG, "Cannot write demo run (%d): %s", result, sqlite3_errmsg(database));
        sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
        return ESP_FAIL;
    }
    return ESP_OK;
}

} // namespace

esp_err_t battery_database_create_demo_run(char *run_id, size_t run_id_size)
{
    DemoContext context = {};
    context.samples = static_cast<Sample *>(heap_caps_malloc_prefer(
        MAX_SAMPLES * sizeof(Sample), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        MALLOC_CAP_8BIT));
    if (context.samples == NULL) {
        return ESP_ERR_NO_MEM;
    }
    simulate(context);
    time_t now = time(NULL);
    time_t duration_s = (time_t)(context.samples[context.sample_count - 1].elapsed_ms / 1000);
    context.start_utc = now >= VALID_TIME_THRESHOLD ? now - duration_s : FALLBACK_START_UTC;
    snprintf(context.run_id, sizeof(context.run_id), "demo-%08" PRIx32, esp_random());

    int64_t started = esp_log_timestamp();
    esp_err_t err = battery_database_execute(write_demo_run, &context, pdMS_TO_TICKS(10000));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Demo run %s written: %u measurements in %" PRIu32 " ms", context.run_id,
                 (unsigned)context.sample_count, (uint32_t)(esp_log_timestamp() - started));
        if (run_id != NULL && run_id_size > 0) {
            strlcpy(run_id, context.run_id, run_id_size);
        }
    }
    heap_caps_free(context.samples);
    return err;
}
