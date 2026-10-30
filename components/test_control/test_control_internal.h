/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string>

#include "test_store.h"
#include "resistance_test.h"

namespace test_control {

enum class RunState : uint8_t { Idle = 0, Running = 1, Complete = 2, Interrupted = 3, Failed = 4 };

struct ManualSettings {
    float charge_current_a = 0.5f;
    float charge_voltage_v = 3.45f;
    float charge_cutoff_a = 0.1f;
    float discharge_current_a = 0.5f;
    float discharge_cutoff_v = 2.5f;
    uint16_t discharge_minutes = 239;
    float power_w = 5.0f;
    float power_cutoff_v = 2.5f;
    uint16_t power_minutes = 239;
};

struct PlanStatus {
    RunState state = RunState::Idle;
    uint8_t step_index = 0;
    uint16_t cycle_pass = 1;
    uint32_t step_elapsed_s = 0;
    std::string message;
};

struct Settings {
    ManualSettings manual;
    uint8_t model = 0;
    bool auto_connect = false;
    test_store::Battery battery;
    test_store::Plan plan;
};

const char *run_state_name(RunState state);
Settings get_settings();
PlanStatus get_plan_status();
bool controls_busy();
std::string start_resistance(const resistance_test::Config &config);
std::string abort_resistance();
std::string save_resistance_config(const resistance_test::Config &config);

/* All functions return an empty string on success or a German error message. */
std::string set_auto_connect(bool enabled);
std::string set_model(uint8_t model);
std::string connect();
std::string disconnect();
std::string stop();
std::string start_charge(float current_a, float voltage_v, float cutoff_a);
std::string start_discharge_cc(float current_a, float cutoff_v, uint16_t minutes);
std::string start_discharge_cp(float power_w, float cutoff_v, uint16_t minutes);
std::string set_battery(const test_store::Battery &battery);
std::string set_plan(const test_store::Plan &plan);
std::string start_plan();
std::string abort_plan();
std::string reset_plan();

esp_err_t register_web_extensions();

} // namespace test_control
