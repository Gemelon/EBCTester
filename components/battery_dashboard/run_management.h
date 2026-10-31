/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <sqlite3.h>
#include <string>
#include <vector>

namespace run_management {

constexpr int PAGE_SIZE = 50;
constexpr int PROTECTED = -1;
struct Filter {
    std::string battery, status, from, until, search;
    int page = 0;
};
struct Run {
    std::string id, name, battery, started, plan, status;
    sqlite3_int64 battery_id = 0;
};
struct Catalog {
    struct Battery { sqlite3_int64 id; std::string label; };
    std::vector<Battery> batteries;
    std::vector<Run> runs;
    int total = 0;
};

bool valid_name(const std::string &name);
bool valid_filter(const Filter &filter);
int list(sqlite3 *db, const Filter &filter, Catalog &catalog);
/* PROTECTED means the run is active; SQLITE_NOTFOUND means it was removed. */
int change(sqlite3 *db, const std::string &id, const std::string *name,
           const std::string &active_id);
}
