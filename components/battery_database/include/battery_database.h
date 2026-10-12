#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sqlite3.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runs while the shared SD-card lock is held; the connection is closed afterwards. */
typedef esp_err_t (*battery_database_operation_t)(sqlite3 *database, void *context);

/* Starts the background task that opens, creates and validates the SD database. */
esp_err_t battery_database_init(void);
bool battery_database_is_ready(void);
/* Serializes all SQLite access; returns ESP_ERR_INVALID_STATE until the database is ready. */
esp_err_t battery_database_execute(battery_database_operation_t operation, void *context,
                                   TickType_t timeout_ticks);
/* Writes a simulated, completed capacity test (battery, plan, run, segments, measurements). */
esp_err_t battery_database_create_demo_run(char *run_id, size_t run_id_size);

#ifdef __cplusplus
}
#endif
