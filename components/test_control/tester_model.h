/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "test_store.h"

/* Tester model and its current limits; free of ESP-IDF dependencies so that host tests can use it. */
namespace test_control {

/* Selected EBC hardware; it only determines the current limit (charge and discharge). */
enum class TesterModel : uint8_t { A20 = 0, A40L = 1 };

const char *tester_model_name(TesterModel model);
bool tester_model_from_name(const char *name, TesterModel &model);
float max_current_a(TesterModel model);

/* All checks return an empty string on success or an error message. */
std::string check_charge(TesterModel model, float current_a, float voltage_v, float cutoff_a);
std::string check_discharge_cc(TesterModel model, float current_a, float cutoff_v, uint16_t minutes);
/* Model-specific current check on top of test_store::validate_plan, which only enforces the
 * absolute range of the plan format. */
std::string check_plan_current(const std::vector<test_store::PlanStep> &steps, TesterModel model);
std::string check_resistance_current(TesterModel model, double pulse_current_a, double charge_current_a,
                                     double prep_current_a, bool automatic);

} // namespace test_control
