/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cassert>
#include <cstdio>
#include <cstring>

#include "test_control.h"
#include "../components/test_control/test_control_internal.h"
#include "ebc_link.h"

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define CONFIG_TEST_CONTROL_AVERAGING_LENGTH 20
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_INITIALIZED 4
#define ESP_ERR_NVS_NOT_FOUND 5
#define ESP_ERR_NOT_FOUND 6

using nvs_open_mode_t = int;
using nvs_handle_t = int;
static uint8_t persisted_model, staged_model;
static bool model_stored, fail_open, fail_write, fail_commit, resistance_busy;
static ebc_link_status_t mock_link{};
static test_store::RecorderStatus mock_recorder;
static resistance_test::Config saved_resistance;
static unsigned connect_calls, resistance_saves;

static esp_err_t nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *handle)
{
    *handle = 1;
    return fail_open ? ESP_ERR_NO_MEM : ESP_OK;
}
static void nvs_close(nvs_handle_t) {}
static esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *) { return ESP_ERR_NVS_NOT_FOUND; }
static esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *) { return ESP_ERR_NVS_NOT_FOUND; }
static esp_err_t nvs_get_u8(nvs_handle_t, const char *key, uint8_t *value)
{
    if (std::strcmp(key, "model") != 0 || !model_stored) return ESP_ERR_NVS_NOT_FOUND;
    *value = persisted_model;
    return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t) { return ESP_OK; }
static esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *) { return ESP_OK; }
static esp_err_t nvs_set_u8(nvs_handle_t, const char *key, uint8_t value)
{
    if (fail_write) return ESP_ERR_NO_MEM;
    if (std::strcmp(key, "model") == 0) staged_model = value;
    return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t)
{
    if (fail_commit) return ESP_ERR_NO_MEM;
    if (staged_model != 0) {
        persisted_model = staged_model;
        model_stored = true;
    }
    return ESP_OK;
}
static const char *esp_err_to_name(esp_err_t) { return "mock storage error"; }
static int64_t esp_timer_get_time() { return 1000000; }

#include "../components/test_control/test_control.cpp"

extern "C" {
esp_err_t test_store_init() { return ESP_OK; }
esp_err_t ebc_link_init(const ebc_link_listener_t *) { return ESP_OK; }
void ebc_link_get_status(ebc_link_status_t *status) { *status = mock_link; }
void ebc_link_set_expected_model(uint8_t model) { mock_link.expected_model = model; }
void ebc_link_set_auto_connect(bool enabled) { mock_link.auto_connect = enabled; }
esp_err_t ebc_link_connect() { connect_calls++; return ESP_OK; }
esp_err_t ebc_link_disconnect() { mock_link.connected = false; return ESP_OK; }
esp_err_t ebc_link_queue_command(uint8_t, uint16_t, uint16_t, uint16_t)
{
    return mock_link.connected ? ESP_OK : ESP_ERR_INVALID_STATE;
}
bool ebc_link_mode_is_active(uint8_t mode) { return mode >= 0x0A && mode <= 0x0C; }
bool ebc_link_mode_is_idle(uint8_t mode) { return mode <= 2 || (mode >= 0x14 && mode <= 0x16); }
}

namespace test_store {
std::string validate_plan(const std::vector<PlanStep> &) { return ""; }
std::string plan_to_definition(const std::vector<PlanStep> &) { return ""; }
bool plan_from_definition(const std::string &, std::vector<PlanStep> &) { return true; }
const char *step_mode_name(uint8_t) { return "mock"; }
esp_err_t begin_run(const Battery &, const Plan *, std::string &, const char *) { return ESP_OK; }
void add_sample(const Sample &) {}
void set_sample_written_callback(SampleWrittenCallback) {}
void finish_run(const char *, const char *) {}
RecorderStatus recorder_status() { return mock_recorder; }
esp_err_t load_plan(const std::string &, Plan &) { return ESP_ERR_NOT_FOUND; }
esp_err_t load_battery(const std::string &, Battery &) { return ESP_ERR_NOT_FOUND; }
}
namespace resistance_test {
bool busy() { return resistance_busy; }
std::string validate(const Config &) { return ""; }
std::string save_config(const Config &config) { saved_resistance = config; resistance_saves++; return ""; }
std::string start(const Config &, const test_store::Battery &) { return ""; }
std::string abort(const char *) { return ""; }
void on_frame(const ebc_frame_t &) {}
void on_tick() {}
void on_connection_lost() {}
}
namespace test_control {
esp_err_t register_web_extensions() { return ESP_OK; }
}

int main()
{
    using namespace test_control;
    assert(get_settings().model == 0);
    assert(!connect().empty() && connect_calls == 0);
    assert(!set_auto_connect(true).empty());
    test_store::Plan plan{"Offline", {{1, 4000, 250, 239}}};
    assert(!set_plan(plan).empty());
    assert(!set_model(0x17).empty() && !model_stored);
    assert(set_model(0x10).empty());
    assert(persisted_model == 0x10 && mock_link.expected_model == 0x10);
    assert(!mock_link.connected && current_model_limits().max_discharge_current_a == 40);
    assert(!set_model(6).empty());
    assert(set_model(10).empty());
    assert(current_model_limits().max_charge_current_a == 50);
    assert(set_model(0x10).empty());
    assert(current_model_limits().max_charge_current_a == 40);
    assert(set_plan(plan).empty());
    plan.steps[0].p1 = 4001;
    assert(!set_plan(plan).empty());
    assert(get_settings().plan.steps[0].p1 == 4000);
    plan.steps = {{3, 4000, 500, 100}};
    assert(set_plan(plan).empty());
    plan.steps[0].p2 = 501;
    assert(!set_plan(plan).empty());
    plan.steps = {{2, 160, 400, 239}};
    assert(set_plan(plan).empty());
    plan.steps[0].p1 = 161;
    assert(!set_plan(plan).empty());
    plan.steps = {{2, 201, 500, 239}};
    assert(!set_plan(plan).empty());

    resistance_test::Config config;
    config.chargeCurrent = config.prepCurrent = config.pulseCurrent = 40;
    config.maxPower = 200;
    assert(save_resistance_config(config).empty() && resistance_saves == 1);
    config.pulseCurrent = 40.01;
    assert(!save_resistance_config(config).empty() && resistance_saves == 1);
    config.pulseCurrent = 40;
    config.chargeVoltage = 5.01;
    assert(!save_resistance_config(config).empty());

    settings_loaded = false;
    settings = {};
    assert(get_settings().model == 0x10);
    assert(mock_link.expected_model == 0x10 && current_model_limits().max_charge_current_a == 40);
    assert(set_model(0x09).empty());
    assert(get_settings().plan.steps.empty());
    config = {};
    assert(save_resistance_config(config).empty());
    config.pulseCurrent = 20.01;
    assert(!save_resistance_config(config).empty());
    plan.steps = {{3, 500, 1800, 100}, {1, 2000, 3000, 239}, {2, 85, 500, 239}};
    assert(set_plan(plan).empty());
    assert(set_model(0x10).empty());
    assert(get_settings().plan.steps[0].p2 == 1800);
    assert(!start_plan().empty());
    assert(!validate_plan_for_model(get_settings().plan.steps, current_model_limits()).empty());

    fail_open = true;
    assert(!set_model(0x09).empty() && get_settings().model == 0x10);
    fail_open = false;
    fail_write = true;
    assert(!set_model(0x09).empty() && get_settings().model == 0x10);
    fail_write = false;
    fail_commit = true;
    assert(!set_model(0x09).empty() && get_settings().model == 0x10 && persisted_model == 0x10);
    fail_commit = false;

    mock_link.command_pending = true;
    assert(!set_model(0x09).empty());
    mock_link.command_pending = false;
    mock_link.connected = mock_link.has_frame = true;
    mock_link.frame.mode = 0x0A;
    assert(!set_model(0x09).empty());
    mock_link = {};
    plan_status.state = RunState::Running;
    assert(!set_model(0x09).empty());
    plan_status.state = RunState::Idle;
    resistance_busy = true;
    assert(!set_model(0x09).empty());
    resistance_busy = false;
    mock_recorder.active = true;
    assert(!set_model(0x09).empty());
    mock_recorder.active = false;
    capturing = true;
    assert(!set_model(0x09).empty());
    capturing = false;
    assert(set_model(0x09).empty());
    assert(get_settings().model == 0x09 && persisted_model == 0x09);
    assert(connect().empty() && connect_calls == 1);
    persisted_model = 6;
    model_stored = true;
    settings_loaded = false;
    settings = {};
    assert(get_settings().model == 0 && mock_link.expected_model == 0);
    persisted_model = 0x10;
    settings_loaded = false;
    settings = {};
    assert(get_settings().model == 0x10 && mock_link.expected_model == 0x10);
    puts("Tester control tests passed: offline model limits, exact boundaries, persistence/reload, unsupported models, storage failures, preserved plans and operation/recording locks.");
}
