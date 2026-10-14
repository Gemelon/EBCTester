/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_control.h"
#include "test_control_internal.h"

#include <cmath>
#include <cstring>
#include <atomic>
#include <mutex>

#include "cJSON.h"
#include "ebc_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace test_control {
namespace {

const char *TAG = "test_control";
constexpr char NVS_NAMESPACE[] = "tester";
constexpr uint8_t MANUAL_BLOB_VERSION = 1;
constexpr int64_t STEP_START_TIMEOUT_US = 10 * 1000 * 1000;
constexpr time_t VALID_TIME_THRESHOLD = 1700000000;
constexpr int AVERAGING_LENGTH = CONFIG_TEST_CONTROL_AVERAGING_LENGTH;

struct StoredManual {
    uint8_t version;
    ManualSettings settings;
};

struct StoredRun {
    uint8_t state;
    uint8_t step_index;
    uint16_t cycle_pass;
};

struct Averager {
    int count = 0;
    double current = 0;
    double voltage = 0;
    double capacity = 0;
    test_store::Sample last;
    bool time_valid = false;
};

std::recursive_mutex state_mutex;
bool settings_loaded;
Settings settings;
PlanStatus plan_status;
bool step_started;
bool seen_active;
int64_t step_start_us;
int64_t manual_start_deadline_us;

bool capturing;
bool plan_capture;
Averager averager;
std::atomic<test_control_persisted_sample_callback_t> persisted_sample_callback{nullptr};

bool is_complete(uint8_t mode)
{
    return mode == 0x14 || mode == 0x15 || mode == 0x16;
}

bool is_stopped(uint8_t mode)
{
    return mode == 0x00 || mode == 0x01 || mode == 0x02;
}

uint16_t hundredths(float value)
{
    return static_cast<uint16_t>(lroundf(value * 100.0f));
}

/* ---------- NVS persistence ---------- */

esp_err_t open_nvs(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(NVS_NAMESPACE, mode, handle);
}

std::string battery_to_json(const test_store::Battery &battery)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "label", battery.label.c_str());
    cJSON_AddStringToObject(root, "manufacturer", battery.manufacturer.c_str());
    cJSON_AddStringToObject(root, "serial", battery.serial_number.c_str());
    cJSON_AddStringToObject(root, "date", battery.manufactured_on.c_str());
    cJSON_AddStringToObject(root, "notes", battery.notes.c_str());
    cJSON_AddNumberToObject(root, "capacity", battery.nominal_capacity_ah);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    std::string json = text != nullptr ? text : "{}";
    cJSON_free(text);
    return json;
}

void battery_from_json(const char *text, test_store::Battery &battery)
{
    cJSON *root = cJSON_Parse(text);
    if (root == nullptr) {
        return;
    }
    auto read = [&](const char *name, std::string &out) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
        if (cJSON_IsString(item) && item->valuestring != nullptr) {
            out = item->valuestring;
        }
    };
    read("label", battery.label);
    read("manufacturer", battery.manufacturer);
    read("serial", battery.serial_number);
    read("date", battery.manufactured_on);
    read("notes", battery.notes);
    const cJSON *capacity = cJSON_GetObjectItemCaseSensitive(root, "capacity");
    if (cJSON_IsNumber(capacity)) {
        battery.nominal_capacity_ah = capacity->valuedouble;
    }
    cJSON_Delete(root);
}

std::string read_nvs_string(nvs_handle_t handle, const char *key)
{
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0 || length > 2048) {
        return "";
    }
    std::string value(length, '\0');
    if (nvs_get_str(handle, key, value.data(), &length) != ESP_OK) {
        return "";
    }
    value.resize(length - 1);
    return value;
}

void save_run_state()
{
    nvs_handle_t handle;
    if (open_nvs(NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    const StoredRun run = {static_cast<uint8_t>(plan_status.state), plan_status.step_index, plan_status.cycle_pass};
    if (nvs_set_blob(handle, "run", &run, sizeof(run)) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

template <typename Writer> void save_setting(Writer writer)
{
    nvs_handle_t handle;
    esp_err_t err = open_nvs(NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = writer(handle);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot store tester setting: %s", esp_err_to_name(err));
    }
}

/* NVS is initialized later by the network manager, so settings are loaded lazily. */
void ensure_settings_loaded()
{
    if (settings_loaded) {
        return;
    }
    nvs_handle_t handle;
    const esp_err_t err = open_nvs(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_INITIALIZED) {
        return;
    }
    settings_loaded = true;
    if (err == ESP_OK) {
        StoredManual manual = {};
        size_t size = sizeof(manual);
        if (nvs_get_blob(handle, "manual", &manual, &size) == ESP_OK && size == sizeof(manual) &&
            manual.version == MANUAL_BLOB_VERSION) {
            settings.manual = manual.settings;
        }
        uint8_t auto_connect = 0;
        if (nvs_get_u8(handle, "auto", &auto_connect) == ESP_OK) {
            settings.auto_connect = auto_connect != 0;
        }
        const std::string battery = read_nvs_string(handle, "battery");
        if (!battery.empty()) {
            battery_from_json(battery.c_str(), settings.battery);
        }
        settings.plan.name = read_nvs_string(handle, "plan_name");
        if (!test_store::plan_from_definition(read_nvs_string(handle, "plan"), settings.plan.steps)) {
            ESP_LOGW(TAG, "Stored test plan is invalid and was ignored");
        }
        StoredRun run = {};
        size = sizeof(run);
        if (nvs_get_blob(handle, "run", &run, &size) == ESP_OK && size == sizeof(run) && run.state <= 4) {
            plan_status.state = static_cast<RunState>(run.state);
            plan_status.step_index = run.step_index;
            plan_status.cycle_pass = run.cycle_pass > 0 ? run.cycle_pass : 1;
        }
        nvs_close(handle);
    }
    if (plan_status.state == RunState::Running) {
        plan_status.state = RunState::Interrupted;
        plan_status.message = "Plan interrupted by restart.";
        ESP_LOGW(TAG, "Test plan was running before restart; marked as interrupted");
        save_run_state();
    }
    ebc_link_set_auto_connect(settings.auto_connect);
    ESP_LOGI(TAG, "Tester settings loaded (plan '%s', %u steps, auto-connect %s)", settings.plan.name.c_str(),
             static_cast<unsigned>(settings.plan.steps.size()), settings.auto_connect ? "on" : "off");
}

void save_manual()
{
    const StoredManual manual = {MANUAL_BLOB_VERSION, settings.manual};
    save_setting([&](nvs_handle_t handle) { return nvs_set_blob(handle, "manual", &manual, sizeof(manual)); });
}

/* ---------- Recording ---------- */

void flush_average()
{
    if (averager.count == 0) {
        return;
    }
    test_store::Sample sample = averager.last;
    sample.current_a = static_cast<float>(averager.current / averager.count);
    sample.voltage_v = static_cast<float>(averager.voltage / averager.count);
    sample.capacity_ah = static_cast<float>(averager.capacity / averager.count);
    test_store::add_sample(sample);
    averager = {};
}

void dispatch_persisted_sample(const test_store::Sample &sample)
{
    const test_control_persisted_sample_callback_t callback = persisted_sample_callback.load();
    if (callback != nullptr) {
        callback(sample.voltage_v, sample.current_a, sample.capacity_ah);
    }
}

void add_to_average(const ebc_frame_t &frame)
{
    test_store::Sample sample;
    sample.uptime_ms = esp_timer_get_time() / 1000;
    const time_t now = time(nullptr);
    const bool time_valid = now >= VALID_TIME_THRESHOLD;
    sample.utc = time_valid ? now : 0;
    sample.mode = frame.mode;
    sample.model = frame.model;
    sample.step = plan_capture ? plan_status.step_index + 1 : 1;
    sample.cycle = plan_capture ? plan_status.cycle_pass : 1;
    sample.setpoint_1 = frame.setpoint_1;
    sample.setpoint_2 = frame.setpoint_2;
    sample.setpoint_3 = frame.setpoint_3;
    if (averager.count > 0) {
        const test_store::Sample &last = averager.last;
        const bool boundary = last.mode != sample.mode || last.model != sample.model || last.step != sample.step ||
                              last.cycle != sample.cycle || averager.time_valid != time_valid ||
                              hundredths(last.setpoint_1) != hundredths(sample.setpoint_1) ||
                              hundredths(last.setpoint_2) != hundredths(sample.setpoint_2) ||
                              hundredths(last.setpoint_3) != hundredths(sample.setpoint_3);
        if (boundary) {
            flush_average();
        }
    }
    averager.count++;
    averager.current += frame.current_a;
    averager.voltage += frame.voltage_v;
    averager.capacity += frame.capacity_ah;
    averager.last = sample;
    averager.time_valid = time_valid;
    if (averager.count >= AVERAGING_LENGTH) {
        flush_average();
    }
}

void end_capture(const char *status, const char *notes)
{
    flush_average();
    test_store::finish_run(status, notes);
    capturing = false;
    plan_capture = false;
}

void update_capture(const ebc_frame_t &frame)
{
    const bool plan_running = plan_status.state == RunState::Running;
    if (!capturing && (ebc_link_mode_is_active(frame.mode) || plan_running)) {
        capturing = true;
        plan_capture = plan_running;
        averager = {};
        std::string run_id;
        const esp_err_t err = test_store::begin_run(settings.battery, plan_capture ? &settings.plan : nullptr, run_id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot start recording: %s", esp_err_to_name(err));
        }
    }
    if (!capturing) {
        return;
    }
    if (plan_capture) {
        if (plan_status.state == RunState::Running) {
            add_to_average(frame);
            return;
        }
        const char *status = plan_status.state == RunState::Complete ? "COMPLETED"
                             : plan_status.state == RunState::Failed ? "ERROR"
                                                                     : "STOPPED";
        end_capture(status, plan_status.message.empty() ? nullptr : plan_status.message.c_str());
        return;
    }
    if (ebc_link_mode_is_active(frame.mode) || !ebc_link_mode_is_idle(frame.mode)) {
        add_to_average(frame);
        return;
    }
    end_capture(is_complete(frame.mode) ? "COMPLETED" : "STOPPED", nullptr);
}

/* ---------- Plan runner ---------- */

void set_plan_state(RunState state, const std::string &message)
{
    plan_status.state = state;
    plan_status.message = message;
    ESP_LOGI(TAG, "Plan state %s: %s", run_state_name(state), message.c_str());
    save_run_state();
}

void advance_step()
{
    plan_status.step_index++;
    step_started = false;
    seen_active = false;
    if (plan_status.step_index >= settings.plan.steps.size()) {
        set_plan_state(RunState::Complete, "Plan completed successfully.");
    } else {
        save_run_state();
    }
}

void run_plan(const ebc_frame_t *frame, int64_t now)
{
    if (plan_status.state != RunState::Running) {
        return;
    }
    if (plan_status.step_index >= settings.plan.steps.size()) {
        set_plan_state(RunState::Complete, "Plan completed successfully.");
        return;
    }
    const test_store::PlanStep &step = settings.plan.steps[plan_status.step_index];
    switch (step.mode) {
    case 1:
    case 2:
    case 3: {
        if (frame == nullptr) {
            return;
        }
        if (!step_started) {
            static const uint8_t commands[] = {0, EBC_CMD_DISCHARGE_CC, EBC_CMD_DISCHARGE_CP, EBC_CMD_CHARGE_CV};
            if (ebc_link_queue_command(commands[step.mode], step.p1, step.p2, step.p3) == ESP_OK) {
                step_started = true;
                seen_active = false;
                step_start_us = now;
                plan_status.message = std::string("Step ") + std::to_string(plan_status.step_index + 1) + " (" +
                                      test_store::step_mode_name(step.mode) + ") started.";
            }
            return;
        }
        if (ebc_link_mode_is_active(frame->mode)) {
            seen_active = true;
            return;
        }
        if (!seen_active) {
            if (now - step_start_us > STEP_START_TIMEOUT_US) {
                ebc_link_queue_command(EBC_CMD_STOP, 0, 0, 0);
                set_plan_state(RunState::Failed, "The EBC did not start step " + std::to_string(plan_status.step_index + 1) +
                                                     " within 10 seconds.");
            }
            return;
        }
        if (is_complete(frame->mode)) {
            advance_step();
        } else if (is_stopped(frame->mode)) {
            set_plan_state(RunState::Failed,
                           "Step " + std::to_string(plan_status.step_index + 1) + " was stopped on the EBC.");
        }
        return;
    }
    case 4:
        if (!step_started) {
            step_started = true;
            step_start_us = now;
            plan_status.message = "Pause " + std::to_string(step.p3) + " min.";
        }
        if (now - step_start_us >= static_cast<int64_t>(step.p3) * 60 * 1000 * 1000) {
            advance_step();
        }
        return;
    case 5:
        if (plan_status.cycle_pass < step.p3) {
            plan_status.cycle_pass++;
            plan_status.step_index = 0;
            step_started = false;
            seen_active = false;
            plan_status.message = "Cycle " + std::to_string(plan_status.cycle_pass) + " of " +
                                  std::to_string(step.p3) + " started.";
            save_run_state();
        } else {
            advance_step();
        }
        return;
    default:
        ebc_link_queue_command(EBC_CMD_STOP, 0, 0, 0);
        set_plan_state(RunState::Failed, "Unknown operating mode in plan.");
        return;
    }
}

/* ---------- EBC link callbacks (run in the link task) ---------- */

void on_frame(const ebc_frame_t *frame, void *)
{
    if (frame->auxiliary) {
        return;
    }
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ensure_settings_loaded();
    if (resistance_test::busy()) {
        resistance_test::on_frame(*frame);
        return;
    }
    if (ebc_link_mode_is_active(frame->mode)) {
        manual_start_deadline_us = 0;
    }
    run_plan(frame, esp_timer_get_time());
    update_capture(*frame);
}

void on_connection_lost(void *)
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    manual_start_deadline_us = 0;
    resistance_test::on_connection_lost();
    if (plan_status.state == RunState::Running) {
        set_plan_state(RunState::Interrupted, "Connection to EBC lost.");
    }
    if (capturing) {
        end_capture("INTERRUPTED", "Connection to EBC lost");
    }
}

void on_tick(void *)
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ensure_settings_loaded();
    resistance_test::on_tick();
    if (manual_start_deadline_us && esp_timer_get_time() >= manual_start_deadline_us) {
        manual_start_deadline_us = 0;
        ESP_LOGE(TAG, "Manual command: EBC did not report an active mode within 10 seconds");
        const esp_err_t err = ebc_link_queue_command(EBC_CMD_STOP, 0, 0, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot queue stop after manual start timeout: %s", esp_err_to_name(err));
        }
    }
    run_plan(nullptr, esp_timer_get_time());
}

std::string check_manual_allowed()
{
    ebc_link_status_t link;
    ebc_link_get_status(&link);
    if (!link.connected) {
        return "The EBC is not connected.";
    }
    if (link.command_pending) {
        return "A command is still awaiting transmission.";
    }
    if (plan_status.state == RunState::Running) {
        return "A test plan is running. Please abort it first.";
    }
    if (resistance_test::busy()) {
        return "Internal resistance measurement is running. Please abort it first.";
    }
    if (manual_start_deadline_us || (link.has_frame && ebc_link_mode_is_active(link.frame.mode))) {
        return "An operation is active or starting. Please stop it first.";
    }
    return "";
}

std::string queue(uint8_t command, uint16_t p1, uint16_t p2, uint16_t p3)
{
    const esp_err_t err = ebc_link_queue_command(command, p1, p2, p3);
    if (err == ESP_ERR_INVALID_STATE) {
        return "The EBC is not connected or a command is pending.";
    }
    return err == ESP_OK ? "" : "Invalid values.";
}

bool in_range(float value, float minimum, float maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

} // namespace

const char *run_state_name(RunState state)
{
    switch (state) {
    case RunState::Running:
        return "running";
    case RunState::Complete:
        return "complete";
    case RunState::Interrupted:
        return "interrupted";
    case RunState::Failed:
        return "failed";
    default:
        return "idle";
    }
}

Settings get_settings()
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ensure_settings_loaded();
    return settings;
}

PlanStatus get_plan_status()
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ensure_settings_loaded();
    PlanStatus status = plan_status;
    if (status.state == RunState::Running && step_started) {
        status.step_elapsed_s = static_cast<uint32_t>((esp_timer_get_time() - step_start_us) / 1000000);
    }
    return status;
}

bool controls_busy()
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ebc_link_status_t link;
    ebc_link_get_status(&link);
    return resistance_test::busy() || plan_status.state == RunState::Running || manual_start_deadline_us != 0 ||
           link.command_pending ||
           (link.connected && link.has_frame && ebc_link_mode_is_active(link.frame.mode));
}

std::string start_resistance(const resistance_test::Config &config)
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ensure_settings_loaded();
    if (controls_busy() || capturing || test_store::recorder_status().active)
        return "Another operation or recording is still active.";
    return resistance_test::start(config, settings.battery);
}

std::string abort_resistance()
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    return resistance_test::abort("Measurement aborted by user.");
}

std::string set_auto_connect(bool enabled)
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ensure_settings_loaded();
    settings.auto_connect = enabled;
    ebc_link_set_auto_connect(enabled);
    save_setting([&](nvs_handle_t handle) { return nvs_set_u8(handle, "auto", enabled ? 1 : 0); });
    return "";
}

std::string connect()
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    if (controls_busy()) {
        return "Connecting is locked during an operation.";
    }
    return ebc_link_connect() == ESP_OK ? "" : "Could not establish connection.";
}

std::string disconnect()
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ensure_settings_loaded();
    if (controls_busy()) {
        return "Disconnecting is locked during an operation. Please stop it first.";
    }
    settings.auto_connect = false;
    save_setting([&](nvs_handle_t handle) { return nvs_set_u8(handle, "auto", 0); });
    return ebc_link_disconnect() == ESP_OK ? "" : "Disconnect failed.";
}

std::string stop()
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    if (resistance_test::busy()) {
        return resistance_test::abort("Measurement aborted by stop.");
    }
    manual_start_deadline_us = 0;
    if (plan_status.state == RunState::Running) {
        set_plan_state(RunState::Interrupted, "Plan aborted by stop.");
    }
    return queue(EBC_CMD_STOP, 0, 0, 0);
}

std::string start_charge(float current_a, float voltage_v, float cutoff_a)
{
    if (!in_range(current_a, 0.01f, 20.0f) || !in_range(voltage_v, 0.01f, 30.0f) ||
        !in_range(cutoff_a, 0.01f, 20.0f)) {
        return "Current 0.01–20 A, charge voltage 0.01–30 V, cutoff current 0.01–20 A.";
    }
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ensure_settings_loaded();
    std::string error = check_manual_allowed();
    if (error.empty()) {
        error = queue(EBC_CMD_CHARGE_CV, hundredths(current_a), hundredths(voltage_v), hundredths(cutoff_a));
    }
    if (error.empty()) {
        settings.manual.charge_current_a = current_a;
        manual_start_deadline_us = esp_timer_get_time() + STEP_START_TIMEOUT_US;
        settings.manual.charge_voltage_v = voltage_v;
        settings.manual.charge_cutoff_a = cutoff_a;
        save_manual();
    }
    return error;
}

std::string start_discharge_cc(float current_a, float cutoff_v, uint16_t minutes)
{
    if (!in_range(current_a, 0.01f, 20.0f) || !in_range(cutoff_v, 0.01f, 30.0f) || minutes > 239) {
        return "Current 0.01–20 A, cutoff voltage 0.01–30 V, time 0–239 min.";
    }
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ensure_settings_loaded();
    std::string error = check_manual_allowed();
    if (error.empty()) {
        error = queue(EBC_CMD_DISCHARGE_CC, hundredths(current_a), hundredths(cutoff_v), minutes);
    }
    if (error.empty()) {
        settings.manual.discharge_current_a = current_a;
        manual_start_deadline_us = esp_timer_get_time() + STEP_START_TIMEOUT_US;
        settings.manual.discharge_cutoff_v = cutoff_v;
        settings.manual.discharge_minutes = minutes;
        save_manual();
    }
    return error;
}

std::string start_discharge_cp(float power_w, float cutoff_v, uint16_t minutes)
{
    if (!in_range(power_w, 1.0f, 2000.0f) || !in_range(cutoff_v, 0.01f, 30.0f) || minutes > 239) {
        return "Power 1–2000 W, cutoff voltage 0.01–30 V, time 0–239 min.";
    }
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ensure_settings_loaded();
    std::string error = check_manual_allowed();
    if (error.empty()) {
        error = queue(EBC_CMD_DISCHARGE_CP, static_cast<uint16_t>(lroundf(power_w)), hundredths(cutoff_v), minutes);
    }
    if (error.empty()) {
        settings.manual.power_w = power_w;
        manual_start_deadline_us = esp_timer_get_time() + STEP_START_TIMEOUT_US;
        settings.manual.power_cutoff_v = cutoff_v;
        settings.manual.power_minutes = minutes;
        save_manual();
    }
    return error;
}

std::string set_battery(const test_store::Battery &battery)
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ensure_settings_loaded();
    if (capturing || resistance_test::busy()) {
        return "The battery cannot be changed during recording.";
    }
    settings.battery = battery;
    const std::string json = battery_to_json(battery);
    save_setting([&](nvs_handle_t handle) { return nvs_set_str(handle, "battery", json.c_str()); });
    return "";
}

std::string set_plan(const test_store::Plan &plan)
{
    if (!plan.steps.empty()) {
        const std::string error = test_store::validate_plan(plan.steps);
        if (!error.empty()) {
            return error;
        }
    }
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ensure_settings_loaded();
    if (plan_status.state == RunState::Running) {
        return "The plan cannot be changed while it is running.";
    }
    settings.plan = plan;
    const std::string definition = test_store::plan_to_definition(plan.steps);
    save_setting([&](nvs_handle_t handle) {
        esp_err_t err = nvs_set_str(handle, "plan_name", plan.name.c_str());
        return err == ESP_OK ? nvs_set_str(handle, "plan", definition.c_str()) : err;
    });
    return "";
}

std::string start_plan()
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    ensure_settings_loaded();
    if (plan_status.state == RunState::Running) {
        return "The plan is already running.";
    }
    if (plan_status.state == RunState::Interrupted) {
        return "The plan was interrupted. Please reset it first.";
    }
    if (controls_busy()) {
        return "An operation is active or a command is pending. Please stop it first.";
    }
    const std::string error = test_store::validate_plan(settings.plan.steps);
    if (!error.empty()) {
        return error;
    }
    if (settings.plan.name.empty()) {
        return "Please give the plan a name.";
    }
    ebc_link_status_t link;
    ebc_link_get_status(&link);
    if (!link.connected || !link.has_frame) {
        return "The EBC is not connected.";
    }
    if (!ebc_link_mode_is_idle(link.frame.mode)) {
        return "The EBC is not ready (is a manual test still running?).";
    }
    if (capturing) {
        end_capture("STOPPED", nullptr);
    }
    plan_status = {};
    plan_status.state = RunState::Running;
    step_started = false;
    seen_active = false;
    set_plan_state(RunState::Running, "Plan started.");
    return "";
}

std::string abort_plan()
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    if (plan_status.state != RunState::Running) {
        return "No plan is running.";
    }
    set_plan_state(RunState::Interrupted, "Plan aborted.");
    ebc_link_queue_command(EBC_CMD_STOP, 0, 0, 0);
    return "";
}

std::string reset_plan()
{
    std::lock_guard<std::recursive_mutex> guard(state_mutex);
    if (plan_status.state == RunState::Running) {
        return "A running plan must be aborted first.";
    }
    plan_status = {};
    step_started = false;
    seen_active = false;
    save_run_state();
    return "";
}

} // namespace test_control

extern "C" esp_err_t test_control_init(void)
{
    using namespace test_control;
    esp_err_t err = test_store_init();
    if (err == ESP_OK) {
        test_store::set_sample_written_callback(dispatch_persisted_sample);
    }
    if (err == ESP_OK) {
        const ebc_link_listener_t listener = {
            .on_frame = on_frame,
            .on_connection_lost = on_connection_lost,
            .on_tick = on_tick,
            .context = nullptr,
        };
        err = ebc_link_init(&listener);
    }
    if (err == ESP_OK) {
        err = register_web_extensions();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Tester control start failed: %s", esp_err_to_name(err));
    }
    return err;
}

extern "C" void test_control_set_persisted_sample_callback(
    test_control_persisted_sample_callback_t callback)
{
    test_control::persisted_sample_callback.store(callback);
}

extern "C" void test_control_handle_mqtt_command(const char *command, const char *value)
{
    if (command == nullptr) {
        return;
    }

    std::string error;
    if (std::strcmp(command, "start_plan") == 0) {
        error = test_control::start_plan();
    } else if (std::strcmp(command, "stop") == 0) {
        error = test_control::stop();
    } else if (std::strcmp(command, "abort") == 0) {
        if (test_control::get_plan_status().state == test_control::RunState::Running) {
            error = test_control::abort_plan();
        } else {
            error = test_control::abort_resistance();
        }
    } else if (std::strcmp(command, "select_plan") == 0 ||
               std::strcmp(command, "select_battery") == 0) {
        if (value == nullptr || value[0] == '\0' || std::strlen(value) > 64) {
            error = "Invalid selection name.";
        } else {
            for (const unsigned char ch : std::string(value)) {
                if (ch < 0x20 || ch == 0x7f) {
                    error = "Invalid selection name.";
                    break;
                }
            }
        }
        if (error.empty() && std::strcmp(command, "select_plan") == 0) {
            test_store::Plan plan;
            const esp_err_t err = test_store::load_plan(value, plan);
            if (err == ESP_OK) {
                error = test_control::set_plan(plan);
            } else {
                error = err == ESP_ERR_NOT_FOUND ? "Plan not found." : "Plan database unavailable.";
            }
        } else if (error.empty()) {
            test_store::Battery battery;
            const esp_err_t err = test_store::load_battery(value, battery);
            if (err == ESP_OK) {
                error = test_control::set_battery(battery);
            } else {
                error = err == ESP_ERR_NOT_FOUND ? "Battery not found." : "Battery database unavailable.";
            }
        }
    } else {
        return;
    }

    if (!error.empty()) {
        ESP_LOGD("test_control", "MQTT command rejected: %s", error.c_str());
    }
}
