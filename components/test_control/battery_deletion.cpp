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
