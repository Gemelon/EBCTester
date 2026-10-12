/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "resistance_test.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/time.h>
#include "battery_database.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

namespace resistance_test {
namespace {
const char *TAG = "resistance";
std::recursive_mutex mutex;
Config config;
Status status;
bool loaded = false;
int64_t phase_start = 0, last_frame = 0, last_idle = 0, pulse_start = 0;
double previous_current = 0, baseline_sum_u = 0, baseline_sum_i = 0;
int baseline_count = 0;
int64_t baseline_start = 0;
bool recorded = false;
std::string terminal_state;
std::string terminal_message;

double seconds(int64_t delta) { return delta / 1000000.0; }
uint16_t encode(double value) { return static_cast<uint16_t>(std::lround(value * 100)); }

void load()
{
    if (loaded) return;
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open("resistance", NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_INITIALIZED) return;
    if (opened == ESP_ERR_NVS_NOT_FOUND) { loaded = true; return; }
    if (opened != ESP_OK) { ESP_LOGE(TAG, "Cannot load settings: %s", esp_err_to_name(opened)); return; }
    Config stored;
    size_t size = sizeof(stored);
    uint32_t version = 0;
    const esp_err_t ver = nvs_get_u32(handle, "version", &version);
    const esp_err_t err = nvs_get_blob(handle, "config", &stored, &size);
    nvs_close(handle);
    if (ver == ESP_OK && version == 1 && err == ESP_OK && size == sizeof(stored) && validate(stored).empty()) {
        config = stored;
    } else if (err != ESP_ERR_NVS_NOT_FOUND || ver != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Stored settings invalid or unreadable: %s; defaults retained", esp_err_to_name(err));
    }
    loaded = true;
}

std::string summary()
{
    // Stored in run notes: kept by the existing SD history and MariaDB backup.
    char text[2048];
    const auto &c = status.config;
    int used = snprintf(text, sizeof(text),
        "DCIR v1; phase=%s; message=%s; temperature=unknown; fourWire=true; automatic=%d; "
        "U1=%.6f V; I1=%.6f A; onsetUncertainty=%.3f s; removed=%.6f Ah; target=%.6f Ah;",
        status.phase.c_str(), status.message.c_str(), c.automatic, status.baselineVoltage,
        status.baselineCurrent, status.onsetUncertainty, status.removedAh, status.targetAh);
    for (const auto &field : fields) {
        if (used < 0 || static_cast<size_t>(used) >= sizeof(text)) break;
        used += snprintf(text + used, sizeof(text) - used, " %s=%.6g;", field.name, c.*field.member);
    }
    for (const auto &r : status.results) {
        if (used < 0 || static_cast<size_t>(used) >= sizeof(text)) break;
        used += snprintf(text + used, sizeof(text) - used,
            " R@%.3fs: captured=%d valid=%d actual=%.3fs U2=%.6fV I2=%.6fA R=%.6fmOhm error=%s;",
            r.requested, r.captured, r.valid, r.actual, r.voltage, r.current, r.milliohms, r.error.c_str());
    }
    return text;
}

void phase(const char *name, int64_t now)
{
    status.phase = name;
    phase_start = now;
    ESP_LOGI(TAG, "Phase %s", name);
}

void finish(const char *name, const std::string &message)
{
    status.phase = name;
    status.message = message;
    status.busy = false;
    if (recorded) {
        const auto notes = summary();
        std::vector<test_store::ResistanceResult> results;
        for (size_t i = 0; i < status.results.size(); ++i) {
            const auto &r = status.results[i];
            test_store::ResistanceResult row;
            row.number = static_cast<uint8_t>(i + 1);
            row.epoch_ms = r.epoch_ms;
            row.uptime_ms = r.uptime_ms;
            row.requested_seconds = r.requested;
            row.actual_seconds = r.actual;
            row.baseline_voltage_v = status.baselineVoltage;
            row.baseline_current_a = status.baselineCurrent;
            row.load_voltage_v = r.voltage;
            row.load_current_a = r.current;
            row.resistance_mohm = r.milliohms;
            row.onset_uncertainty_seconds = status.onsetUncertainty;
            row.captured = r.captured;
            row.valid = r.valid;
            row.error = r.error.empty() && !r.captured ? "Sample not captured: " + message : r.error;
            results.push_back(row);
        }
        test_store::finish_resistance_run(strcmp(name, "complete") == 0 ? "COMPLETED" :
                                         strcmp(name, "interrupted") == 0 ? "INTERRUPTED" : "ERROR",
                                         notes.c_str(), results);
        recorded = false;
    }
    ESP_LOGI(TAG, "%s: %s", name, message.c_str());
}

bool command(uint8_t cmd, uint16_t p1 = 0, uint16_t p2 = 0, uint16_t p3 = 0)
{
    const esp_err_t err = ebc_link_queue_command(cmd, p1, p2, p3);
    if (err == ESP_OK) return true;
    ESP_LOGE(TAG, "Cannot queue 0x%02x: %s", cmd, esp_err_to_name(err));
    finish("failed", "Could not send command. Check the tester's state.");
    return false;
}

void stop_for(const char *name, const std::string &message)
{
    terminal_state = name;
    terminal_message = message;
    if (command(EBC_CMD_STOP)) phase("stopping", esp_timer_get_time());
}

void reset_baseline()
{
    baseline_start = 0;
    baseline_count = 0;
    baseline_sum_u = baseline_sum_i = 0;
}

void record(const ebc_frame_t &frame, int64_t now)
{
    test_store::Sample sample;
    sample.uptime_ms = now / 1000;
    const time_t utc = time(nullptr);
    sample.utc = utc >= 1700000000 ? utc : 0;
    sample.mode = frame.mode;
    sample.model = frame.model;
    sample.current_a = frame.current_a;
    sample.voltage_v = frame.voltage_v;
    sample.capacity_ah = frame.capacity_ah;
    sample.setpoint_1 = frame.setpoint_1;
    sample.setpoint_2 = frame.setpoint_2;
    sample.setpoint_3 = frame.setpoint_3;
    sample.step = status.phase == "charging" || status.phase == "charge_start" ? 1 :
                  status.phase == "discharging" || status.phase == "discharge_start" ? 2 :
                  status.phase == "rest" ? 3 : 4;
    test_store::add_sample(sample);
}
} // namespace

std::string validate(const Config &c)
{
    for (const auto &field : fields) {
        const double value = c.*field.member;
        if (!std::isfinite(value) || value < field.min || value > field.max) {
            return std::string("Invalid value: ") + field.name;
        }
    }
    if (!(c.time1 < c.time2 && c.time2 < c.time3 && c.pulseSeconds >= c.time3 + c.timingTolerance))
        return "Sample times must be increasing; pulse duration must be at least the last sample time plus timing tolerance.";
    if (c.restSeconds < c.baselineSeconds) return "Rest duration must be at least as long as the U1 measurement window.";
    if (c.cutoffVoltage >= c.maxVoltage || (c.automatic && c.chargeVoltage >= c.maxVoltage))
        return "Cutoff/charge voltage must be below the maximum voltage.";
    if (c.automatic && (c.chargeVoltage <= c.cutoffVoltage || c.chargeCutoff > c.chargeCurrent))
        return "Charge voltage must exceed cutoff voltage; cutoff current must not exceed charge current.";
    if (c.pulseCurrent <= c.idleCurrent) return "Test current must exceed the allowed idle current.";
    if (c.pulseCurrent * c.maxVoltage > c.maxPower ||
        (c.automatic && std::fmax(c.chargeCurrent, c.prepCurrent) * c.maxVoltage > c.maxPower))
        return "Current multiplied by maximum voltage exceeds the power limit.";
    for (double value : {c.pulseCurrent, c.chargeCurrent, c.prepCurrent, c.chargeVoltage, c.chargeCutoff, c.cutoffVoltage})
        if (std::fabs(value * 100 - std::round(value * 100)) > 0.00001)
            return "UART current and voltage setpoints must use increments of 0.01.";
    return "";
}

Config get_config() { std::lock_guard<std::recursive_mutex> guard(mutex); load(); return config; }
bool busy() { std::lock_guard<std::recursive_mutex> guard(mutex); return status.busy; }
Status get_status()
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    load();
    Status copy = status;
    if (copy.runId.empty()) {
        copy.config = config;
        copy.results[0].requested = config.time1;
        copy.results[1].requested = config.time2;
        copy.results[2].requested = config.time3;
    }
    if (copy.busy) copy.elapsed = seconds(esp_timer_get_time() - phase_start);
    return copy;
}

std::string save_config(const Config &c)
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    load();
    if (status.busy) return "Settings are locked during measurement.";
    const auto error = validate(c);
    if (!error.empty()) return error;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("resistance", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "version", 1);
        if (err == ESP_OK) err = nvs_set_blob(handle, "config", &c, sizeof(c));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot persist settings: %s", esp_err_to_name(err));
        return "Could not save settings in NVS.";
    }
    config = c;
    loaded = true;
    return "";
}

std::string start(const Config &c, const test_store::Battery &battery)
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if (status.busy) return "An internal resistance test is already running.";
    if (!battery_database_is_ready()) return "The SD database must be ready for measurement.";
    if (battery.label.empty()) return "Please select a battery first.";
    ebc_link_status_t link;
    ebc_link_get_status(&link);
    if (!link.connected || !link.has_frame || !ebc_link_mode_is_idle(link.frame.mode) || link.command_pending)
        return "The tester must be connected and idle.";
    if (link.last_frame_age_ms > c.maxGap * 1000) return "Tester data is too old.";
    if (link.frame.voltage_v <= c.cutoffVoltage || link.frame.voltage_v >= c.maxVoltage)
        return "Battery voltage is outside the configured protection limits.";
    const auto error = save_config(c);
    if (!error.empty()) return error;
    Status fresh;
    fresh.config = c;
    fresh.targetAh = c.automatic ? c.capacityAh * (100 - c.targetSoc) / 100 : 0;
    fresh.results[0].requested = c.time1;
    fresh.results[1].requested = c.time2;
    fresh.results[2].requested = c.time3;
    fresh.phase = "starting";
    status = fresh;
    const auto initial_notes = summary();
    const esp_err_t err = test_store::begin_run(battery, nullptr, status.runId, initial_notes.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Recorder start: %s", esp_err_to_name(err));
        status.phase = "failed";
        status.message = "Could not start recording.";
        return "Could not start recording.";
    }
    status.busy = recorded = true;
    status.message = "Temperature unknown; target SOC is calculated. Four-wire measurement.";
    last_frame = esp_timer_get_time();
    last_idle = last_frame;
    previous_current = 0;
    pulse_start = 0;
    reset_baseline();
    if (c.automatic) {
        if (command(EBC_CMD_CHARGE_CV, encode(c.chargeCurrent), encode(c.chargeVoltage), encode(c.chargeCutoff)))
            phase("charge_start", last_frame);
    } else {
        phase("rest", last_frame);
    }
    return status.busy ? "" : status.message;
}

std::string abort(const char *reason)
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if (!status.busy) return "No internal resistance test is active.";
    if (status.busy && status.phase != "stopping") stop_for("interrupted", reason);
    return status.busy ? "" : status.message;
}

void on_connection_lost()
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if (status.busy) finish("interrupted", "UART connection lost. The tester may still be running; check it in person.");
}

void on_tick()
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if (!status.busy) return;
    const int64_t now = esp_timer_get_time();
    const double elapsed = seconds(now - phase_start);
    if (status.phase == "stopping" && elapsed >= 10) {
        finish("failed", "Stop not confirmed. Check the tester in person.");
        return;
    }
    if (status.phase == "stopping") return;
    const auto recorder = test_store::recorder_status();
    if (recorder.failed || recorder.samples_dropped) {
        stop_for("failed", "Recording failed or raw data was dropped.");
        return;
    }
    if (seconds(now - last_frame) > status.config.maxGap) {
        stop_for("failed", "UART sampling gap exceeds the protection limit.");
    } else if ((status.phase == "charge_start" || status.phase == "discharge_start" ||
                status.phase == "pulse_start" || status.phase == "prep_stop") &&
               elapsed >= 10) {
        stop_for("failed", "The tester did not confirm startup within 10 seconds.");
    } else if (status.phase == "charging" && elapsed >= status.config.maxChargeSeconds) {
        stop_for("failed", "Maximum charging time reached.");
    } else if (status.phase == "discharging" && elapsed >= status.config.maxPrepSeconds) {
        stop_for("failed", "Maximum preparation discharge time reached.");
    } else if (status.phase == "rest" && elapsed > status.config.restSeconds + status.config.baselineSeconds + 10) {
        stop_for("failed", "No stable open-circuit measurement during the rest period.");
    } else if (status.phase == "pulse" && seconds(now - pulse_start) >= status.config.pulseSeconds) {
        bool valid = true;
        for (auto &r : status.results) {
            if (!r.captured) r.error = "No UART sample within the time window.";
            valid = valid && r.valid;
        }
        stop_for(valid ? "complete" : "failed", valid ? "Load-pulse test completed." : "At least one result is invalid.");
    }
}

void on_frame(const ebc_frame_t &f)
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if (!status.busy || f.auxiliary) return;
    const int64_t now = esp_timer_get_time();
    const double dt = seconds(now - last_frame);
    const auto &c = status.config;
    record(f, now);
    last_frame = now;
    if (status.phase == "stopping") {
        if (ebc_link_mode_is_idle(f.mode) && std::fabs(f.current_a) <= c.idleCurrent)
            finish(terminal_state.c_str(), terminal_message);
        return;
    }
    if (!std::isfinite(f.voltage_v) || !std::isfinite(f.current_a) || f.voltage_v <= c.cutoffVoltage ||
        f.voltage_v >= c.maxVoltage || std::fabs(f.current_a * f.voltage_v) > c.maxPower) {
        stop_for("failed", "Voltage/power limit reached or invalid sample.");
        return;
    }
    if (dt > c.maxGap) { stop_for("failed", "UART sampling gap; measurement aborted."); return; }
    if (ebc_link_mode_is_active(f.mode) &&
        ((status.phase == "charge_start" && f.mode != 0x0c) ||
         ((status.phase == "discharge_start" || status.phase == "pulse_start") && f.mode != 0x0a))) {
        stop_for("failed", "The tester reports a different operating mode than requested.");
        return;
    }
    if (status.phase == "charge_start" && f.mode == 0x0c) { phase("charging", now); return; }
    if (status.phase == "charging") {
        if (f.mode == 0x16) {
            if (status.targetAh <= 0) { phase("rest", now); reset_baseline(); }
            else if (command(EBC_CMD_DISCHARGE_CC, encode(c.prepCurrent), encode(c.cutoffVoltage), 0))
                phase("discharge_start", now);
        } else if (f.mode != 0x0c) stop_for("failed", "Charging ended prematurely.");
        return;
    }
    if (status.phase == "discharge_start" && f.mode == 0x0a) {
        previous_current = f.current_a;
        phase("discharging", now);
        return;
    }
    if (status.phase == "discharging") {
        if (f.mode != 0x0a || f.current_a <= 0) {
            stop_for("failed", "Target SOC not reached: discharge ended or no positive load current.");
            return;
        }
        status.removedAh += (previous_current + f.current_a) * .5 * dt / 3600;
        previous_current = f.current_a;
        if (status.removedAh >= status.targetAh && command(EBC_CMD_STOP)) phase("prep_stop", now);
        return;
    }
    if (status.phase == "prep_stop") {
        if (ebc_link_mode_is_idle(f.mode) && std::fabs(f.current_a) <= c.idleCurrent) {
            phase("rest", now);
            reset_baseline();
        } else if (seconds(now - phase_start) >= 10) stop_for("failed", "Preparation stop not confirmed.");
        return;
    }
    if (status.phase == "rest") {
        if (!ebc_link_mode_is_idle(f.mode) || std::fabs(f.current_a) > c.idleCurrent) {
            reset_baseline();
            if (ebc_link_mode_is_active(f.mode)) stop_for("failed", "Unexpected operation during the rest period.");
            return;
        }
        last_idle = now;
        if (seconds(now - phase_start) < c.restSeconds - c.baselineSeconds) return;
        if (!baseline_start) baseline_start = now;
        baseline_sum_u += f.voltage_v;
        baseline_sum_i += f.current_a;
        baseline_count++;
        if (baseline_count >= 2 && seconds(now - baseline_start) >= c.baselineSeconds &&
            seconds(now - phase_start) >= c.restSeconds) {
            status.baselineVoltage = baseline_sum_u / baseline_count;
            status.baselineCurrent = baseline_sum_i / baseline_count;
            if (command(EBC_CMD_DISCHARGE_CC, encode(c.pulseCurrent), encode(c.cutoffVoltage),
                        static_cast<uint16_t>(std::ceil(c.pulseSeconds / 60)))) {
                pulse_start = now;
                phase("pulse_start", now);
            }
        }
        return;
    }
    if (status.phase == "pulse_start") {
        if (f.mode == 0x0a && f.current_a > c.idleCurrent) {
            status.onsetUncertainty = seconds(now - last_idle);
            if (seconds(now - pulse_start) >= c.pulseSeconds) {
                stop_for("failed", "Load pulse detected only after the configured duration expired.");
                return;
            }
            phase("pulse", now);
        } else return;
    }
    if (status.phase == "pulse") {
        if (f.mode != 0x0a || f.current_a <= c.idleCurrent) {
            stop_for("failed", "Load pulse ended prematurely or no discharge current.");
            return;
        }
        const double elapsed = seconds(now - pulse_start);
        if (elapsed > c.pulseSeconds) return;
        for (auto &r : status.results) {
            if (r.captured || elapsed < r.requested) continue;
            r.captured = true;
            r.uptime_ms = now / 1000;
            struct timeval utc = {};
            if (gettimeofday(&utc, nullptr) != 0) {
                ESP_LOGE(TAG, "Cannot read UTC clock for resistance sample");
            } else if (utc.tv_sec >= 1700000000) {
                r.epoch_ms = static_cast<int64_t>(utc.tv_sec) * 1000 + utc.tv_usec / 1000;
            }
            r.actual = elapsed;
            r.voltage = f.voltage_v;
            r.current = f.current_a;
            const double drop = status.baselineVoltage - r.voltage;
            if (elapsed - r.requested > c.timingTolerance) r.error = "UART sample too late.";
            else if (drop < c.minVoltageDrop) r.error = "Voltage drop too small or negative.";
            else if (r.current <= 0) r.error = "No positive measured load current.";
            else { r.milliohms = 1000 * drop / r.current; r.valid = true; }
        }
    }
}
} // namespace resistance_test
