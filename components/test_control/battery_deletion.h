/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "sqlite3.h"

int delete_unused_battery(sqlite3 *database, const char *label);
