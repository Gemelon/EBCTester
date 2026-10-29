/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include "tester_model.h"

using test_control::TesterModel;
using test_store::PlanStep;

namespace {

/* These texts must stay identical to the English catalog entries so that the web UI translates them. */
const char *const A20_CHARGE_ERROR = "Current 0.01–20 A, charge voltage 0.01–30 V, cutoff current 0.01–20 A.";
const char *const A20_DISCHARGE_ERROR = "Current 0.01–20 A, cutoff voltage 0.01–30 V, time 0–239 min.";

void model_names()
{
    assert(std::string(test_control::tester_model_name(TesterModel::A20)) == "A20");
    assert(std::string(test_control::tester_model_name(TesterModel::A40L)) == "A40L");
    TesterModel model = TesterModel::A20;
    assert(test_control::tester_model_from_name("A40L", model) && model == TesterModel::A40L);
    assert(test_control::tester_model_from_name("A20", model) && model == TesterModel::A20);
    model = TesterModel::A40L;
    for (const char *invalid : {"", "a20", "A40", "EBC-A20", "A20 ", "A40L\n"}) {
        assert(!test_control::tester_model_from_name(invalid, model));
        assert(model == TesterModel::A40L);
    }
    assert(!test_control::tester_model_from_name(nullptr, model));
    assert(test_control::max_current_a(TesterModel::A20) == 20.0f);
    assert(test_control::max_current_a(TesterModel::A40L) == 40.0f);
}

void manual_charge()
{
    assert(test_control::check_charge(TesterModel::A20, 20.0f, 3.65f, 0.1f).empty());
    assert(test_control::check_charge(TesterModel::A20, 0.01f, 30.0f, 20.0f).empty());
    assert(test_control::check_charge(TesterModel::A20, 20.01f, 3.65f, 0.1f) == A20_CHARGE_ERROR);
    assert(test_control::check_charge(TesterModel::A20, 10.0f, 3.65f, 20.5f) == A20_CHARGE_ERROR);
    assert(test_control::check_charge(TesterModel::A20, 0.0f, 3.65f, 0.1f) == A20_CHARGE_ERROR);
    assert(test_control::check_charge(TesterModel::A20, 10.0f, 30.01f, 0.1f) == A20_CHARGE_ERROR);
    assert(test_control::check_charge(TesterModel::A20, NAN, 3.65f, 0.1f) == A20_CHARGE_ERROR);
    assert(test_control::check_charge(TesterModel::A40L, 40.0f, 3.65f, 40.0f).empty());
    assert(test_control::check_charge(TesterModel::A40L, 30.0f, 3.65f, 0.1f).empty());
    assert(test_control::check_charge(TesterModel::A40L, 40.01f, 3.65f, 0.1f) ==
           "Current 0.01–40 A, charge voltage 0.01–30 V, cutoff current 0.01–40 A.");
    assert(test_control::check_charge(TesterModel::A40L, INFINITY, 3.65f, 0.1f) != "");
}

void manual_discharge()
{
    assert(test_control::check_discharge_cc(TesterModel::A20, 20.0f, 2.0f, 239).empty());
    assert(test_control::check_discharge_cc(TesterModel::A20, 20.01f, 2.0f, 0) == A20_DISCHARGE_ERROR);
    assert(test_control::check_discharge_cc(TesterModel::A20, 10.0f, 2.0f, 240) == A20_DISCHARGE_ERROR);
    assert(test_control::check_discharge_cc(TesterModel::A20, 10.0f, 0.0f, 10) == A20_DISCHARGE_ERROR);
    assert(test_control::check_discharge_cc(TesterModel::A40L, 40.0f, 2.0f, 239).empty());
    assert(test_control::check_discharge_cc(TesterModel::A40L, 40.01f, 2.0f, 239) ==
           "Current 0.01–40 A, cutoff voltage 0.01–30 V, time 0–239 min.");
}

PlanStep step(uint8_t mode, uint16_t p1, uint16_t p2, uint16_t p3)
{
    PlanStep result;
    result.mode = mode;
    result.p1 = p1;
    result.p2 = p2;
    result.p3 = p3;
    return result;
}

void plan_limits()
{
    const std::vector<PlanStep> within_a20 = {step(1, 2000, 250, 0), step(3, 2000, 365, 2000), step(4, 0, 0, 10),
                                              step(5, 0, 0, 3)};
    assert(test_control::check_plan_current(within_a20, TesterModel::A20).empty());
    assert(test_control::check_plan_current(within_a20, TesterModel::A40L).empty());
    assert(test_control::check_plan_current({}, TesterModel::A20).empty());

    const std::vector<PlanStep> discharge_25a = {step(4, 0, 0, 1), step(1, 2500, 250, 0)};
    assert(test_control::check_plan_current(discharge_25a, TesterModel::A20) ==
           "Step 2: Current 0.01–20 A, cutoff voltage 0.01–30 V, time 0–239 min.");
    assert(test_control::check_plan_current(discharge_25a, TesterModel::A40L).empty());

    const std::vector<PlanStep> charge_cutoff_25a = {step(3, 1000, 365, 2500)};
    assert(test_control::check_plan_current(charge_cutoff_25a, TesterModel::A20) ==
           "Step 1: Current 0.01–20 A, charge voltage 0.01–30 V, cutoff current 0.01–20 A.");
    assert(test_control::check_plan_current(charge_cutoff_25a, TesterModel::A40L).empty());

    assert(test_control::check_plan_current({step(3, 4001, 365, 100)}, TesterModel::A40L) ==
           "Step 1: Current 0.01–40 A, charge voltage 0.01–30 V, cutoff current 0.01–40 A.");
    /* D-CP stores power in p1 (watts), not current: not subject to the current limit. */
    assert(test_control::check_plan_current({step(2, 2000, 250, 0)}, TesterModel::A20).empty());
}

void resistance_limits()
{
    assert(test_control::check_resistance_current(TesterModel::A20, 20.0, 0.5, 0.5, true).empty());
    assert(test_control::check_resistance_current(TesterModel::A20, 25.0, 0.5, 0.5, false) ==
           "Current exceeds the A20 limit of 20 A.");
    assert(test_control::check_resistance_current(TesterModel::A40L, 25.0, 0.5, 0.5, false).empty());
    assert(test_control::check_resistance_current(TesterModel::A40L, 40.5, 0.5, 0.5, false) ==
           "Current exceeds the A40L limit of 40 A.");
    /* Charge and preparation currents only matter for the automatic method. */
    assert(test_control::check_resistance_current(TesterModel::A20, 10.0, 30.0, 30.0, false).empty());
    assert(!test_control::check_resistance_current(TesterModel::A20, 10.0, 30.0, 0.5, true).empty());
    assert(!test_control::check_resistance_current(TesterModel::A20, 10.0, 0.5, 30.0, true).empty());
    assert(test_control::check_resistance_current(TesterModel::A40L, 10.0, 30.0, 30.0, true).empty());
}

} // namespace

int main()
{
    model_names();
    manual_charge();
    manual_discharge();
    plan_limits();
    resistance_limits();
    std::cout << "tester model host tests passed\n";
    return 0;
}
