/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <string>
#include "ebc_link.h"
#include "test_store.h"

namespace resistance_test {

struct Config {
    bool automatic = false;
    double chargeCurrent = 0.5, chargeVoltage = 3.45, chargeCutoff = 0.1;
    double capacityAh = 20, targetSoc = 50, prepCurrent = 0.5;
    double cutoffVoltage = 2.0, maxVoltage = 3.8, maxPower = 150;
    double restSeconds = 60, baselineSeconds = 3, idleCurrent = 0.02;
    double pulseCurrent = 0.5, pulseSeconds = 12;
    double time1 = 1, time2 = 5, time3 = 10, timingTolerance = 1.5;
    double maxGap = 3, maxChargeSeconds = 86400, maxPrepSeconds = 86400;
    double minVoltageDrop = 0.001;
};

struct Field {
    const char *name;
    double Config::*member;
    double min, max;
};

inline constexpr std::array<Field, 22> fields = {{
    {"chargeCurrent", &Config::chargeCurrent, .01, 40}, {"chargeVoltage", &Config::chargeVoltage, .01, 30},
    {"chargeCutoff", &Config::chargeCutoff, .01, 40}, {"capacityAh", &Config::capacityAh, .001, 10000},
    {"targetSoc", &Config::targetSoc, 0, 100}, {"prepCurrent", &Config::prepCurrent, .01, 40},
    {"cutoffVoltage", &Config::cutoffVoltage, .01, 30}, {"maxVoltage", &Config::maxVoltage, .01, 30},
    {"maxPower", &Config::maxPower, .01, 2000}, {"restSeconds", &Config::restSeconds, 1, 86400},
    {"baselineSeconds", &Config::baselineSeconds, 1, 60}, {"idleCurrent", &Config::idleCurrent, 0, .5},
    {"pulseCurrent", &Config::pulseCurrent, .01, 40}, {"pulseSeconds", &Config::pulseSeconds, 1, 120},
    {"time1", &Config::time1, .1, 120}, {"time2", &Config::time2, .1, 120},
    {"time3", &Config::time3, .1, 120}, {"timingTolerance", &Config::timingTolerance, .1, 5},
    {"maxGap", &Config::maxGap, .1, 5}, {"maxChargeSeconds", &Config::maxChargeSeconds, 10, 172800},
    {"maxPrepSeconds", &Config::maxPrepSeconds, 10, 172800},
    {"minVoltageDrop", &Config::minVoltageDrop, .0001, 1},
}};

struct Result {
    int64_t epoch_ms = 0, uptime_ms = 0;
    double requested = 0, actual = 0, voltage = 0, current = 0, milliohms = 0;
    bool captured = false, valid = false;
    std::string error;
};

struct Status {
    Config config;
    bool busy = false;
    std::string phase = "idle", message, runId;
    double elapsed = 0, removedAh = 0, targetAh = 0, baselineVoltage = 0, baselineCurrent = 0;
    double onsetUncertainty = 0;
    std::array<Result, 3> results;
};

Config get_config();
Status get_status();
bool busy();
std::string validate(const Config &config);
std::string save_config(const Config &config);
std::string start(const Config &config, const test_store::Battery &battery);
std::string abort(const char *reason);
void on_frame(const ebc_frame_t &frame);
void on_tick();
void on_connection_lost();

} // namespace resistance_test
