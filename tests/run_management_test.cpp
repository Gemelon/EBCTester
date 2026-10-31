/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#include "run_management.h"
#include "battery_deletion.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>

static void sql(sqlite3 *db, const std::string &query)
{
    const int result = sqlite3_exec(db, query.c_str(), nullptr, nullptr, nullptr);
    if (result != SQLITE_OK) std::cerr << query << ": " << sqlite3_errmsg(db) << '\n';
    assert(result == SQLITE_OK);
}
static int number(sqlite3 *db, const char *query)
{
    sqlite3_stmt *stmt = nullptr;
    assert(sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const int value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}
static std::string file(const std::string &path)
{
    std::ifstream stream(path);
    assert(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    sqlite3 *db = nullptr;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    sql(db, "PRAGMA foreign_keys=ON;");
    const std::string root = argv[1];
    sqlite3 *rollback_db = nullptr;
    assert(sqlite3_open(":memory:", &rollback_db) == SQLITE_OK);
    sql(rollback_db, file(root + "/components/battery_database/schema.sql"));
    sql(rollback_db, file(root + "/components/battery_database/resistance_schema.sql"));
    sql(rollback_db, "BEGIN IMMEDIATE;");
    sql(rollback_db, file(root + "/components/battery_database/run_management_schema.sql"));
    assert(sqlite3_exec(rollback_db, "INSERT INTO missing_table VALUES(1);", nullptr, nullptr, nullptr) != SQLITE_OK);
    sql(rollback_db, "ROLLBACK;");
    assert(number(rollback_db, "SELECT schema_version FROM ebc_schema_metadata") == 2);
    assert(number(rollback_db, "SELECT COUNT(*) FROM pragma_table_info('ebc_test_run') WHERE name='display_name'") == 0);
    assert(sqlite3_close(rollback_db) == SQLITE_OK);
    sql(db, file(root + "/components/battery_database/schema.sql"));
    sql(db, "INSERT INTO ebc_battery(battery_id,label,created_at_utc) VALUES(1,'Cell A','2026-10-01'),(2,'Cell B','2026-10-01');"
            "INSERT INTO ebc_test_plan(plan_id,device_id,plan_name,plan_version) VALUES(1,'device','Cycle plan',1);"
            "INSERT INTO ebc_test_run(run_id,device_id,battery_id,plan_id,started_at_utc,status) VALUES"
            "('done','device',1,1,'2026-10-30 23:59:59','COMPLETED'),"
            "('active','device',2,1,'2026-10-31 00:00:00','RUNNING');");
    sql(db, file(root + "/components/battery_database/resistance_schema.sql"));
    assert((delete_unused_battery(db, "Cell A") & 0xff) == SQLITE_CONSTRAINT);
    assert((delete_unused_battery(db, "Cell B") & 0xff) == SQLITE_CONSTRAINT);
    assert(delete_unused_battery(db, "missing") == SQLITE_NOTFOUND);
    assert(delete_unused_battery(db, "Cell A' OR 1=1 --") == SQLITE_NOTFOUND);
    sql(db, "INSERT INTO ebc_battery(label,created_at_utc) VALUES('Unused','2026-10-31');");
    assert(delete_unused_battery(db, "unused") == SQLITE_OK);
    assert(delete_unused_battery(db, "Unused") == SQLITE_NOTFOUND);
    assert(number(db, "SELECT COUNT(*) FROM ebc_battery") == 2);
    assert(number(db, "SELECT COUNT(*) FROM ebc_test_run") == 2);
    const std::string migration = file(root + "/components/battery_database/run_management_schema.sql");
    sql(db, "BEGIN IMMEDIATE;");
    sql(db, migration);
    sql(db, "COMMIT;");
    assert(number(db, "SELECT schema_version FROM ebc_schema_metadata") == 3);
    assert(number(db, "SELECT COUNT(*) FROM ebc_test_run WHERE display_name IS NULL") == 2);
    // Failed/repeated migration rolls back without changing existing data.
    sql(db, "BEGIN IMMEDIATE;");
    assert(sqlite3_exec(db, migration.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK);
    sql(db, "ROLLBACK;");
    assert(number(db, "SELECT COUNT(*) FROM ebc_test_run") == 2);
    using namespace run_management;
    std::string name = "Cell capacity check";
    assert(change(db, "active", &name, "") == PROTECTED);
    assert(change(db, "active", nullptr, "") == PROTECTED);
    assert(change(db, "done", &name, "done") == PROTECTED);
    assert(change(db, "done", nullptr, "done") == PROTECTED);
    assert(change(db, "missing", &name, "") == SQLITE_NOTFOUND);
    assert(change(db, "done' OR 1=1 --", nullptr, "") == SQLITE_NOTFOUND);
    assert(change(db, "done", &name, "") == SQLITE_OK);
    Catalog catalog;
    Filter filter;
    assert(list(db, filter, catalog) == SQLITE_OK && catalog.total == 2);
    assert(catalog.runs[1].name == name);
    filter.search = "CAPACITY";
    assert(list(db, filter, catalog) == SQLITE_OK && catalog.total == 1);
    filter.search = "%";
    assert(list(db, filter, catalog) == SQLITE_OK && catalog.total == 0);
    filter.search = ""; filter.battery = "1"; filter.status = "COMPLETED";
    filter.from = "2026-10-30 00:00:00"; filter.until = "2026-10-31 00:00:00";
    assert(list(db, filter, catalog) == SQLITE_OK && catalog.total == 1 && catalog.runs[0].id == "done");
    filter.from = "2026-10-31 00:00:00"; filter.until = "2026-11-01 00:00:00";
    assert(list(db, filter, catalog) == SQLITE_OK && catalog.total == 0);
    filter = {}; filter.status = "bad";
    assert(!valid_filter(filter));
    filter = {}; filter.page = -1;
    assert(!valid_filter(filter));
    filter = {}; filter.until = "2026-99-01 00:00:00";
    assert(!valid_filter(filter));
    filter.until = "2026-02-29 00:00:00";
    assert(!valid_filter(filter));
    filter.until = "2028-02-29 00:00:00";
    assert(valid_filter(filter));
    std::string empty;
    assert(change(db, "done", &empty, "") == SQLITE_OK);
    assert(number(db, "SELECT COUNT(*) FROM ebc_test_run WHERE display_name IS NULL") == 2);
    assert(valid_name(std::string(100, 'a')) && !valid_name(std::string(101, 'a')));
    std::string unicode;
    for (int i = 0; i < 100; ++i) unicode += "\xF0\x9F\x94\x8B";
    assert(valid_name(unicode) && !valid_name(unicode + "x"));
    assert(!valid_name("\xC0\xAF") && !valid_name("\xED\xA0\x80") && !valid_name("a\nb"));
    assert(!valid_name(std::string("a\0b", 3)));
    assert(change(db, "done", &unicode, "") == SQLITE_OK);
    sql(db, "INSERT INTO ebc_run_segment VALUES('done',1,1,1,0,0,0,1);"
            "INSERT INTO ebc_measurement(run_id,sequence_number,segment_number,device_uptime_ms,mode_code,current_a,voltage_v,capacity_ah)"
            " VALUES('done',1,1,1000,12,1,3.4,0.1);"
            "INSERT INTO ebc_resistance_result(run_id,result_number,requested_seconds,baseline_voltage_v,baseline_current_a,onset_uncertainty_seconds,valid)"
            " VALUES('done',1,1,3.4,0,1,0);"
            "CREATE TABLE ebc_sync_state(run_id TEXT PRIMARY KEY); INSERT INTO ebc_sync_state VALUES('done');"
            "CREATE TRIGGER refuse_delete BEFORE DELETE ON ebc_resistance_result BEGIN SELECT RAISE(ABORT,'test failure'); END;");
    assert(change(db, "done", nullptr, "") != SQLITE_OK);
    assert(number(db, "SELECT COUNT(*) FROM ebc_test_run WHERE run_id='done'") == 1);
    assert(number(db, "SELECT COUNT(*) FROM ebc_measurement") == 1);
    assert(number(db, "SELECT COUNT(*) FROM ebc_sync_state") == 1);
    sql(db, "DROP TRIGGER refuse_delete;");
    assert(change(db, "done", nullptr, "") == SQLITE_OK);
    assert(number(db, "SELECT COUNT(*) FROM ebc_measurement") == 0);
    assert(number(db, "SELECT COUNT(*) FROM ebc_run_segment") == 0);
    assert(number(db, "SELECT COUNT(*) FROM ebc_resistance_result") == 0);
    assert(number(db, "SELECT COUNT(*) FROM ebc_sync_state") == 0);
    assert(number(db, "SELECT COUNT(*) FROM ebc_battery") == 2);
    assert(number(db, "SELECT COUNT(*) FROM ebc_test_plan") == 1);
    for (int i = 0; i < 350; ++i)
        sql(db, "INSERT INTO ebc_test_run(run_id,device_id,started_at_utc,status) VALUES('run-" +
                std::to_string(i) + "','device','2026-01-01 00:00:00','STOPPED');");
    filter = {}; filter.status = "STOPPED"; filter.page = 6;
    assert(list(db, filter, catalog) == SQLITE_OK && catalog.total == 350 && catalog.runs.size() == 50);
    filter.page = 7;
    assert(list(db, filter, catalog) == SQLITE_OK && catalog.runs.empty());
    assert(sqlite3_close(db) == SQLITE_OK);
    std::cout << "Run management tests passed: migration, names, UTF-8, active protection, filters, pagination, cascades and rollback.\n";
}
