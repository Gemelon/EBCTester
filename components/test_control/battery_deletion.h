/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "sqlite3.h"

int delete_unused_battery(sqlite3 *database, const char *label);
int rename_battery_label(sqlite3 *database, const char *old_label, const char *new_label);
