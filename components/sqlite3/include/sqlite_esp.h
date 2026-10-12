#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Route SQLite heap allocations to PSRAM when available, falling back to
 * internal RAM. Must be called before sqlite3_initialize().
 *
 * @return SQLITE_OK or an SQLite error code.
 */
int sqlite_esp_configure_memory(void);

#ifdef __cplusplus
}
#endif
