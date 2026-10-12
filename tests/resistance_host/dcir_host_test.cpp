/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
#include <sys/time.h>
#include "resistance_test.h"

using resistance_test::Config;
int64_t clock_us = 1000000;
bool db_ready = true, connected = true, queue_fails = false;
test_store::RecorderStatus recorder;
std::vector<uint8_t> commands;
std::vector<test_store::Sample> samples;
std::string finish_status, notes;
std::vector<test_store::ResistanceResult> result_rows;

int64_t esp_timer_get_time() { return clock_us; }
bool battery_database_is_ready() { return db_ready; }
extern "C" bool ebc_link_mode_is_idle(uint8_t mode) { return mode <= 2 || (mode >= 0x14 && mode <= 0x16); }
extern "C" bool ebc_link_mode_is_active(uint8_t mode) { return mode >= 0x0a && mode <= 0x0c; }
extern "C" void ebc_link_get_status(ebc_link_status_t *link)
{
    *link = {};
    link->connected = connected;
    link->has_frame = true;
    link->frame.voltage_v = 3.3f;
}
extern "C" esp_err_t ebc_link_queue_command(uint8_t cmd, uint16_t, uint16_t, uint16_t)
{
    commands.push_back(cmd);
    return queue_fails ? ESP_FAIL : ESP_OK;
}
namespace test_store {
esp_err_t begin_run(const Battery &, const Plan *, std::string &run, const char *initial_notes)
{
    recorder = {};
    recorder.active = true;
    run = "test-run";
    samples.clear();
    assert(initial_notes != nullptr && std::string(initial_notes).find("DCIR v1;") == 0);
    return ESP_OK;
}
void add_sample(const Sample &s) { samples.push_back(s); }
void finish_run(const char *state, const char *text)
{
    recorder.active = false;
    finish_status = state;
    notes = text;
}
void finish_resistance_run(const char *state, const char *text, const std::vector<ResistanceResult> &results)
{
    result_rows = results;
    finish_run(state, text);
}
RecorderStatus recorder_status() { return recorder; }
}

void feed(uint8_t mode = 0, float u = 3.3f, float i = 0, double delta = 1)
{
    clock_us += static_cast<int64_t>(delta * 1000000);
    ebc_frame_t frame{};
    frame.mode = mode;
    frame.voltage_v = u;
    frame.current_a = i;
    resistance_test::on_frame(frame);
}

Config prepared()
{
    Config c;
    c.restSeconds = 2;
    c.baselineSeconds = 1;
    return c;
}
void start(const Config &c)
{
    test_store::Battery battery;
    battery.label = "Host test";
    commands.clear();
    const auto error = resistance_test::start(c, battery);
    if (!error.empty()) std::cerr << error << '\n';
    assert(error.empty());
}
void rest()
{
    feed();
    feed();
    assert(resistance_test::get_status().phase == "pulse_start");
    assert(commands.back() == EBC_CMD_DISCHARGE_CC);
}
void finish_pulse()
{
    for (int t = 1; t <= 12; ++t) {
        feed(0x0a, 3.25f, 5);
        resistance_test::on_tick();
    }
    assert(resistance_test::get_status().phase == "stopping");
    feed();
}

int main()
{
    Config c = prepared();
    assert(resistance_test::validate(c).empty());
    auto invalid = c;
    invalid.pulseSeconds = 10;
    assert(!resistance_test::validate(invalid).empty());
    invalid = c; invalid.time2 = invalid.time1;
    assert(!resistance_test::validate(invalid).empty());
    invalid = c; invalid.maxPower = .01;
    assert(!resistance_test::validate(invalid).empty());
    invalid = c; invalid.pulseCurrent = NAN;
    assert(!resistance_test::validate(invalid).empty());
    db_ready = false;
    assert(!resistance_test::start(c, {}).empty());
    db_ready = true;

    start(c);
    rest();
    finish_pulse();
    auto state = resistance_test::get_status();
    assert(state.phase == "complete" && !state.busy);
    assert(finish_status == "COMPLETED");
    assert(notes.find("DCIR v1;") == 0);
    assert(notes.find("temperature=unknown") != std::string::npos);
    assert(samples.size() == 15);
    assert(result_rows.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        const double time = i == 0 ? 1 : i == 1 ? 5 : 10;
        assert(state.results[i].valid);
        assert(std::fabs(state.results[i].actual - time) < .001);
        assert(std::fabs(state.results[i].milliohms - 10) < .001);
        assert(state.results[i].current == 5); // Measured, not commanded 0.5 A.
        assert(result_rows[i].number == i + 1);
        assert(result_rows[i].captured && result_rows[i].valid);
        assert(result_rows[i].uptime_ms > 0);
        assert(result_rows[i].epoch_ms > 1700000000000LL);
        assert(std::fabs(result_rows[i].resistance_mohm - 10) < .001);
    }
    std::cout << "Prepared pulse: 1/5/10s = 10mOhm, measured current, raw samples, SD notes OK\n";

    host_clock_synchronized = false;
    start(c); rest(); finish_pulse();
    assert(result_rows.size() == 3);
    for (const auto &row : result_rows) {
        assert(row.epoch_ms == 0);
        assert(row.uptime_ms > 0);
        assert(row.valid);
    }
    host_clock_synchronized = true;

    start(c); rest();
    feed(0x0a, 3.35f, 5);
    assert(!resistance_test::get_status().results[0].valid);
    resistance_test::abort("negative drop test");
    assert(resistance_test::busy()); // Must wait for stop acknowledgement.
    feed();
    assert(!resistance_test::busy());

    c.timingTolerance = .1;
    start(c); rest();
    feed(0x0a, 3.25f, 5, 1.2);
    assert(resistance_test::get_status().results[0].error == "UART sample too late.");
    resistance_test::abort("late sample test"); feed();

    start(prepared()); rest();
    clock_us += 4000000;
    resistance_test::on_tick();
    assert(resistance_test::get_status().phase == "stopping");
    feed();
    assert(resistance_test::get_status().phase == "failed");
    assert(finish_status == "ERROR");

    start(prepared());
    resistance_test::on_connection_lost();
    assert(resistance_test::get_status().phase == "interrupted");
    assert(finish_status == "INTERRUPTED");

    start(prepared());
    queue_fails = true;
    assert(!resistance_test::abort("command error").empty());
    assert(resistance_test::get_status().phase == "failed");
    queue_fails = false;
    start(prepared());
    resistance_test::abort("stopping timeout");
    clock_us += 5000000;
    resistance_test::abort("repeated abort");
    clock_us += 5000000;
    resistance_test::on_tick();
    assert(resistance_test::get_status().phase == "failed");

    start(prepared());
    recorder.samples_dropped = 1;
    resistance_test::on_tick();
    assert(resistance_test::get_status().phase == "stopping");
    feed();
    assert(resistance_test::get_status().phase == "failed");
    std::cout << "Invalid data, timing tolerance, frame gaps, abort acknowledgement, connection loss, command error OK\n";

    c = prepared();
    c.automatic = true;
    c.capacityAh = .001;
    c.targetSoc = 50;
    start(c);
    assert(commands.back() == EBC_CMD_CHARGE_CV);
    feed(0x0c, 3.4f, .5f);
    assert(resistance_test::get_status().phase == "charging");
    feed(0x16, 3.45f);
    assert(commands.back() == EBC_CMD_DISCHARGE_CC);
    feed(0x0a, 3.3f, 1.8f);
    feed(0x0a, 3.3f, 1.8f);
    feed(0x0a, 3.3f, 1.8f);
    state = resistance_test::get_status();
    assert(state.phase == "prep_stop");
    assert(state.removedAh >= state.targetAh);
    assert(commands.back() == EBC_CMD_STOP);
    feed();
    assert(resistance_test::get_status().phase == "rest");
    rest(); finish_pulse();
    assert(resistance_test::get_status().phase == "complete");

    c.targetSoc = 100;
    start(c);
    feed(0x0c, 3.4f, .5f);
    feed(0x16, 3.45f);
    assert(resistance_test::get_status().phase == "rest");
    resistance_test::abort("100 percent test"); feed();

    c = prepared();
    c.automatic = true;
    c.maxChargeSeconds = 10;
    start(c);
    feed(0x0c, 3.4f, .5f);
    for (int t = 0; t < 10; ++t) feed(0x0c, 3.4f, .5f);
    resistance_test::on_tick();
    assert(resistance_test::get_status().phase == "stopping");
    feed();
    assert(resistance_test::get_status().phase == "failed");

    start(prepared());
    feed(0, 2.4f);
    assert(resistance_test::get_status().phase == "stopping");
    feed();
    assert(resistance_test::get_status().phase == "failed");
    std::cout << "Automatic charge / measured-current integration / target SOC / rest / pulse, 100% SOC, time and voltage limits OK\n";
}
