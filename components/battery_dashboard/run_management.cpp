/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "run_management.h"

namespace run_management {
namespace {
std::string text(sqlite3_stmt *stmt, int column)
{
    const auto *value = sqlite3_column_text(stmt, column);
    return value ? reinterpret_cast<const char *>(value) : "";
}

bool timestamp(const std::string &value)
{
    if (value.empty()) return true;
    if (value.size() != 19) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const char separator = i == 4 || i == 7 ? '-' : i == 10 ? ' ' : i == 13 || i == 16 ? ':' : 0;
        if (separator ? value[i] != separator : value[i] < '0' || value[i] > '9') return false;
    }
    const int year = std::stoi(value.substr(0, 4));
    const int month = std::stoi(value.substr(5, 2)), day = std::stoi(value.substr(8, 2));
    const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return year > 0 && month >= 1 && month <= 12 && day >= 1 &&
           day <= days[month - 1] + (month == 2 && leap ? 1 : 0) &&
           value.substr(11, 2) <= "23" && value.substr(14, 2) <= "59" && value.substr(17, 2) <= "59";
}

int bind_filter(sqlite3_stmt *stmt, const Filter &filter)
{
    const std::string *values[] = {&filter.battery, &filter.status, &filter.from, &filter.until, &filter.search};
    for (int i = 0; i < 5; ++i) {
        const int result = sqlite3_bind_text(stmt, i + 1, values[i]->c_str(), -1, SQLITE_TRANSIENT);
        if (result != SQLITE_OK) return result;
    }
    return SQLITE_OK;
}
constexpr const char *WHERE =
    " FROM ebc_test_run r LEFT JOIN ebc_battery b ON b.battery_id=r.battery_id"
    " LEFT JOIN ebc_test_plan p ON p.plan_id=r.plan_id"
    " WHERE (?1='' OR CAST(r.battery_id AS TEXT)=?1)"
    " AND (?2='' OR r.status=?2) AND (?3='' OR r.started_at_utc>=?3)"
    " AND (?4='' OR r.started_at_utc<?4)"
    " AND (?5='' OR instr(lower(IFNULL(r.display_name,'') || ' ' || IFNULL(b.label,'') ||"
    " ' ' || IFNULL(p.plan_name,'') || ' ' || r.started_at_utc || ' ' || r.status || ' ' || r.run_id),lower(?5))>0)";
}

bool valid_name(const std::string &name)
{
    if (name.size() > 400) return false;
    size_t count = 0;
    for (size_t i = 0; i < name.size();) {
        const unsigned char lead = static_cast<unsigned char>(name[i++]);
        unsigned code = lead;
        int extra = 0;
        if (lead >= 0xC2 && lead <= 0xDF) { code = lead & 0x1F; extra = 1; }
        else if (lead >= 0xE0 && lead <= 0xEF) { code = lead & 0x0F; extra = 2; }
        else if (lead >= 0xF0 && lead <= 0xF4) { code = lead & 7; extra = 3; }
        else if (lead >= 0x80) return false;
        for (int j = 0; j < extra; ++j) {
            if (i == name.size()) return false;
            const unsigned char next = static_cast<unsigned char>(name[i++]);
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3F);
        }
        if ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) ||
            (extra == 3 && code < 0x10000) || code > 0x10FFFF ||
            (code >= 0xD800 && code <= 0xDFFF) || code < 0x20 ||
            (code >= 0x7F && code <= 0x9F) || ++count > 100) return false;
    }
    return true;
}

bool valid_filter(const Filter &filter)
{
    if (filter.page < 0 || filter.page > 1000000 || filter.search.size() > 100 ||
        !valid_name(filter.search) || !timestamp(filter.from) || !timestamp(filter.until) ||
        (!filter.from.empty() && !filter.until.empty() && filter.from >= filter.until)) return false;
    if (!filter.battery.empty()) {
        if (filter.battery.size() > 18 || filter.battery[0] == '0') return false;
        for (char c : filter.battery) if (c < '0' || c > '9') return false;
    }
    return filter.status.empty() || filter.status == "RUNNING" || filter.status == "COMPLETED" ||
           filter.status == "STOPPED" || filter.status == "INTERRUPTED" || filter.status == "ERROR";
}

int list(sqlite3 *db, const Filter &filter, Catalog &catalog)
{
    if (!valid_filter(filter)) return SQLITE_MISUSE;
    catalog = {};
    sqlite3_stmt *stmt = nullptr;
    int result = sqlite3_prepare_v2(db, "SELECT battery_id,label FROM ebc_battery ORDER BY label COLLATE NOCASE;",
                                     -1, &stmt, nullptr);
    while (result == SQLITE_OK && (result = sqlite3_step(stmt)) == SQLITE_ROW) {
        catalog.batteries.push_back({sqlite3_column_int64(stmt, 0), text(stmt, 1)});
        result = SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    if (result != SQLITE_DONE) return result;
    const std::string count_sql = std::string("SELECT COUNT(*)") + WHERE;
    result = sqlite3_prepare_v2(db, count_sql.c_str(), -1, &stmt, nullptr);
    if (result == SQLITE_OK) result = bind_filter(stmt, filter);
    if (result == SQLITE_OK) {
        result = sqlite3_step(stmt);
        if (result == SQLITE_ROW) { catalog.total = sqlite3_column_int(stmt, 0); result = SQLITE_OK; }
    }
    sqlite3_finalize(stmt);
    if (result != SQLITE_OK) return result;
    const std::string sql =
        std::string("SELECT r.run_id,r.display_name,b.label,r.started_at_utc,p.plan_name,r.status,r.battery_id") +
        WHERE + " ORDER BY r.started_at_utc DESC,r.run_id DESC LIMIT ?6 OFFSET ?7;";
    result = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (result == SQLITE_OK) result = bind_filter(stmt, filter);
    if (result == SQLITE_OK) result = sqlite3_bind_int(stmt, 6, PAGE_SIZE);
    if (result == SQLITE_OK) result = sqlite3_bind_int(stmt, 7, filter.page * PAGE_SIZE);
    while (result == SQLITE_OK && (result = sqlite3_step(stmt)) == SQLITE_ROW) {
        catalog.runs.push_back({text(stmt, 0), text(stmt, 1), text(stmt, 2), text(stmt, 3),
                               text(stmt, 4), text(stmt, 5), sqlite3_column_int64(stmt, 6)});
        result = SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    return result == SQLITE_DONE ? SQLITE_OK : result;
}

int change(sqlite3 *db, const std::string &id, const std::string *name, const std::string &active_id)
{
    if (name && !valid_name(*name)) return SQLITE_MISUSE;
    if (!active_id.empty() && id == active_id) return PROTECTED;
    int result = sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
    if (result != SQLITE_OK) return result;
    sqlite3_stmt *stmt = nullptr;
    result = sqlite3_prepare_v2(db, "SELECT status FROM ebc_test_run WHERE run_id=?1;", -1, &stmt, nullptr);
    if (result == SQLITE_OK) result = sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (result == SQLITE_OK) {
        result = sqlite3_step(stmt);
        if (result == SQLITE_DONE) result = SQLITE_NOTFOUND;
        else if (result == SQLITE_ROW) result = text(stmt, 0) == "RUNNING" ? PROTECTED : SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    if (result == SQLITE_OK) {
        result = sqlite3_prepare_v2(db, name ? "UPDATE ebc_test_run SET display_name=?2 WHERE run_id=?1;" :
                                            "DELETE FROM ebc_test_run WHERE run_id=?1;",
                                   -1, &stmt, nullptr);
        if (result == SQLITE_OK) result = sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        if (result == SQLITE_OK && name) result = name->empty() ? sqlite3_bind_null(stmt, 2) :
            sqlite3_bind_text(stmt, 2, name->c_str(), -1, SQLITE_TRANSIENT);
        if (result == SQLITE_OK) { result = sqlite3_step(stmt); if (result == SQLITE_DONE) result = SQLITE_OK; }
        sqlite3_finalize(stmt);
    }
    if (result == SQLITE_OK && !name) {
        result = sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='ebc_sync_state';",
                                   -1, &stmt, nullptr);
        if (result == SQLITE_OK) result = sqlite3_step(stmt);
        const bool has_sync = result == SQLITE_ROW;
        if (result == SQLITE_ROW || result == SQLITE_DONE) result = SQLITE_OK;
        sqlite3_finalize(stmt);
        if (result == SQLITE_OK && has_sync) {
            result = sqlite3_prepare_v2(db, "DELETE FROM ebc_sync_state WHERE run_id=?1;", -1, &stmt, nullptr);
            if (result == SQLITE_OK) result = sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            if (result == SQLITE_OK) { result = sqlite3_step(stmt); if (result == SQLITE_DONE) result = SQLITE_OK; }
            sqlite3_finalize(stmt);
        }
    }
    if (result == SQLITE_OK) result = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    if (result != SQLITE_OK) sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    return result;
}
}
