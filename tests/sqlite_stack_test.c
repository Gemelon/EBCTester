/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sqlite3.c"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

#ifdef SQLITE_USE_ALLOCA
#error SQLite scratch buffers must not use the task stack
#endif

int main(void)
{
    assert(sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 1) == SQLITE_OK);
    assert(sqlite3_initialize() == SQLITE_OK);
    sqlite3 *database = NULL;
    assert(sqlite3_open(":memory:", &database) == SQLITE_OK);

    /* Exercise the same allocation helpers used by B-tree balancing. */
    sqlite3_int64 before = sqlite3_memory_used();
    void *scratch = sqlite3StackAllocRaw(database, 7 * 4096);
    assert(scratch != NULL);
    assert(sqlite3_memory_used() >= before + 7 * 4096);
    sqlite3StackFree(database, scratch);
    assert(sqlite3_memory_used() == before);

    assert(sqlite3_exec(database,
        "PRAGMA page_size=4096;"
        "CREATE TABLE measurement(sequence INTEGER PRIMARY KEY, payload BLOB);"
        "CREATE INDEX measurement_payload ON measurement(payload);"
        "BEGIN;", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(database,
        "INSERT INTO measurement VALUES(?1, randomblob(256));",
        -1, &statement, NULL) == SQLITE_OK);
    for (int sequence = 1; sequence <= 5000; ++sequence) {
        assert(sqlite3_bind_int(statement, 1, sequence) == SQLITE_OK);
        assert(sqlite3_step(statement) == SQLITE_DONE);
        assert(sqlite3_reset(statement) == SQLITE_OK);
    }
    assert(sqlite3_finalize(statement) == SQLITE_OK);
    assert(sqlite3_exec(database, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_prepare_v2(database, "PRAGMA integrity_check;", -1,
                             &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(statement, 0), "ok") == 0);
    assert(sqlite3_finalize(statement) == SQLITE_OK);
    assert(sqlite3_close(database) == SQLITE_OK);
    assert(sqlite3_shutdown() == SQLITE_OK);
    puts("PASS: SQLite scratch buffers use heap; 5000 indexed writes pass integrity check");
    return 0;
}
