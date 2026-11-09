/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */
#include "battery_deletion.h"

int delete_unused_battery(sqlite3 *database, const char *label)
{
    sqlite3_stmt *statement = nullptr;
    int result = sqlite3_prepare_v2(database, "DELETE FROM ebc_battery WHERE label = ?1;",
                                    -1, &statement, nullptr);
    if (result == SQLITE_OK) {
        result = sqlite3_bind_text(statement, 1, label, -1, SQLITE_TRANSIENT);
        if (result == SQLITE_OK) result = sqlite3_step(statement);
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) return result;
    return sqlite3_changes(database) == 1 ? SQLITE_OK : SQLITE_NOTFOUND;
}

int rename_battery_label(sqlite3 *database, const char *old_label, const char *new_label)
{
    sqlite3_stmt *statement = nullptr;
    int result = sqlite3_prepare_v2(
        database,
        "UPDATE ebc_battery SET label = ?2, updated_at_utc = CURRENT_TIMESTAMP WHERE label = ?1;",
        -1, &statement, nullptr);
    if (result == SQLITE_OK) {
        result = sqlite3_bind_text(statement, 1, old_label, -1, SQLITE_TRANSIENT);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_text(statement, 2, new_label, -1, SQLITE_TRANSIENT);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_step(statement);
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) return result;
    return sqlite3_changes(database) == 1 ? SQLITE_OK : SQLITE_NOTFOUND;
}
