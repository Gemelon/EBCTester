#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "esp_err.h"

/* SQLite persistence for batteries, test plans and recorded test runs. */
namespace test_store {

enum class StepMode : uint8_t { DischargeCc = 1, DischargeCp = 2, ChargeCv = 3, Wait = 4, Cycle = 5 };

struct PlanStep {
    uint8_t mode = 0;
    uint16_t p1 = 0;
    uint16_t p2 = 0;
    uint16_t p3 = 0;
};

struct Plan {
    std::string name;
    std::vector<PlanStep> steps;
};

struct Battery {
    std::string label;
    std::string manufacturer;
    std::string serial_number;
    std::string manufactured_on; /* ISO date YYYY-MM-DD */
    std::string notes;
    double nominal_capacity_ah = 0.0;
};

struct Sample {
    int64_t uptime_ms = 0;
    time_t utc = 0; /* 0 when the clock is not synchronized */
    uint8_t mode = 0;
    uint8_t model = 0;
    float current_a = 0;
    float voltage_v = 0;
    float capacity_ah = 0;
    uint16_t step = 1;
    uint16_t cycle = 1;
    float setpoint_1 = 0;
    float setpoint_2 = 0;
    float setpoint_3 = 0;
};

struct RecorderStatus {
    bool active = false;
    bool failed = false;
    std::string run_id;
    uint32_t samples_written = 0;
    uint32_t samples_pending = 0;
    uint32_t samples_dropped = 0;
    std::string error;
};

struct ResistanceResult {
    uint8_t number = 0;
    int64_t epoch_ms = 0, uptime_ms = 0;
    double requested_seconds = 0, actual_seconds = 0;
    double baseline_voltage_v = 0, baseline_current_a = 0;
    double load_voltage_v = 0, load_current_a = 0, resistance_mohm = 0;
    double onset_uncertainty_seconds = 0;
    bool captured = false, valid = false;
    std::string error;
};

constexpr size_t MAX_PLAN_STEPS = 10;

const char *step_mode_name(uint8_t mode);
/* Checks ranges and cycle placement; returns a German error text or an empty string. */
std::string validate_plan(const std::vector<PlanStep> &steps);
std::string plan_to_definition(const std::vector<PlanStep> &steps);
bool plan_from_definition(const std::string &definition, std::vector<PlanStep> &steps);

esp_err_t list_batteries(std::vector<std::string> &labels);
esp_err_t load_battery(const std::string &label, Battery &battery);
/* Inserts or updates by label; empty fields keep existing values. */
esp_err_t save_battery(const Battery &battery);

esp_err_t list_plans(std::vector<std::string> &names);
esp_err_t load_plan(const std::string &name, Plan &plan);
/* Inserts or updates by name; the plan version increases when the steps change. */
esp_err_t save_plan(const Plan &plan);

/* Recording is asynchronous: database writes run in a background task. */
esp_err_t begin_run(const Battery &battery, const Plan *plan, std::string &run_id, const char *notes = nullptr);
void add_sample(const Sample &sample);
/* status: COMPLETED, STOPPED, INTERRUPTED or ERROR. */
void finish_run(const char *status, const char *notes);
void finish_resistance_run(const char *status, const char *notes, const std::vector<ResistanceResult> &results);
RecorderStatus recorder_status();

} // namespace test_store

extern "C" esp_err_t test_store_init(void);
