/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tester_model.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace test_control {
namespace {

int current_limit(TesterModel model)
{
    return static_cast<int>(max_current_a(model));
}

bool in_range(float value, float minimum, float maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

} // namespace

const char *tester_model_name(TesterModel model)
{
    return model == TesterModel::A40L ? "A40L" : "A20";
}

bool tester_model_from_name(const char *name, TesterModel &model)
{
    if (name == nullptr) {
        return false;
    }
    if (std::strcmp(name, "A20") == 0) {
        model = TesterModel::A20;
        return true;
    }
    if (std::strcmp(name, "A40L") == 0) {
        model = TesterModel::A40L;
        return true;
    }
    return false;
}

float max_current_a(TesterModel model)
{
    return model == TesterModel::A40L ? 40.0f : 20.0f;
}

std::string check_charge(TesterModel model, float current_a, float voltage_v, float cutoff_a)
{
    const float limit = max_current_a(model);
    if (in_range(current_a, 0.01f, limit) && in_range(voltage_v, 0.01f, 30.0f) && in_range(cutoff_a, 0.01f, limit)) {
        return "";
    }
    char text[96];
    std::snprintf(text, sizeof(text), "Current 0.01–%d A, charge voltage 0.01–30 V, cutoff current 0.01–%d A.",
                  current_limit(model), current_limit(model));
    return text;
}

std::string check_discharge_cc(TesterModel model, float current_a, float cutoff_v, uint16_t minutes)
{
    if (in_range(current_a, 0.01f, max_current_a(model)) && in_range(cutoff_v, 0.01f, 30.0f) && minutes <= 239) {
        return "";
    }
    char text[80];
    std::snprintf(text, sizeof(text), "Current 0.01–%d A, cutoff voltage 0.01–30 V, time 0–239 min.",
                  current_limit(model));
    return text;
}

std::string check_plan_current(const std::vector<test_store::PlanStep> &steps, TesterModel model)
{
    const int limit = current_limit(model);
    const int limit_hundredths = limit * 100;
    char text[128];
    for (size_t index = 0; index < steps.size(); ++index) {
        const test_store::PlanStep &step = steps[index];
        if (step.mode == 1 && step.p1 > limit_hundredths) {
            std::snprintf(text, sizeof(text),
                          "Step %u: Current 0.01–%d A, cutoff voltage 0.01–30 V, time 0–239 min.",
                          static_cast<unsigned>(index + 1), limit);
            return text;
        }
        if (step.mode == 3 && (step.p1 > limit_hundredths || step.p3 > limit_hundredths)) {
            std::snprintf(text, sizeof(text),
                          "Step %u: Current 0.01–%d A, charge voltage 0.01–30 V, cutoff current 0.01–%d A.",
                          static_cast<unsigned>(index + 1), limit, limit);
            return text;
        }
    }
    return "";
}

std::string check_resistance_current(TesterModel model, double pulse_current_a, double charge_current_a,
                                     double prep_current_a, bool automatic)
{
    const double limit = max_current_a(model);
    if (pulse_current_a <= limit && (!automatic || std::fmax(charge_current_a, prep_current_a) <= limit)) {
        return "";
    }
    char text[96];
    std::snprintf(text, sizeof(text), "Current exceeds the %s limit of %d A.", tester_model_name(model),
                  current_limit(model));
    return text;
}

} // namespace test_control
