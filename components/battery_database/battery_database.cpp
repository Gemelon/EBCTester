/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "battery_database.h"

#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sd_card_manager.h"
#include "sqlite_esp.h"

extern const char schema_sql_start[] asm("_binary_schema_sql_start");
extern const char schema_sql_end[] asm("_binary_schema_sql_end");
extern const char resistance_schema_start[] asm("_binary_resistance_schema_sql_start");

static const char *TAG = "battery_database";
static constexpr TickType_t SD_LOCK_TIMEOUT = pdMS_TO_TICKS(1000);
static constexpr TickType_t RETRY_INTERVAL = pdMS_TO_TICKS(2000);
static constexpr UBaseType_t DATABASE_TASK_PRIORITY = tskIDLE_PRIORITY + 1;
static constexpr char DATABASE_FILE_NAME[] = "/ebc_a20.db";
static std::atomic<bool> database_ready{false};

static void log_heap_state(const char *stage)
{
    ESP_LOGW(TAG,
             "Heap after %s: internal free=%u largest=%u, PSRAM free=%u largest=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static void log_database_file_state(const char *database_path, const char *stage)
{
    if (LOG_LOCAL_LEVEL < ESP_LOG_DEBUG || esp_log_level_get(TAG) < ESP_LOG_DEBUG) {
        return;
    }
    unsigned char header[24];
    size_t bytes_read = 0;
    struct stat file_stat = {};
    int fd = open(database_path, O_RDONLY);
    if (fd < 0 || fstat(fd, &file_stat) != 0) {
        ESP_LOGE(TAG, "Cannot inspect database after %s (errno %d)", stage, errno);
        if (fd >= 0) {
            close(fd);
        }
        return;
    }
    while (bytes_read < sizeof(header)) {
        ssize_t result = read(fd, header + bytes_read, sizeof(header) - bytes_read);
        if (result > 0) {
            bytes_read += (size_t)result;
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(fd);

    char header_hex[sizeof(header) * 3 + 1] = {};
    size_t hex_length = 0;
    for (size_t i = 0; i < bytes_read; ++i) {
        int written = snprintf(header_hex + hex_length, sizeof(header_hex) - hex_length,
                               "%02X%s", header[i], i + 1 < bytes_read ? " " : "");
        if (written < 0 || (size_t)written >= sizeof(header_hex) - hex_length) {
            break;
        }
        hex_length += (size_t)written;
    }
    ESP_LOGD(TAG, "Database state after %s: size=%" PRId64 ", header[%u]=%s",
             stage, (int64_t)file_stat.st_size, (unsigned)bytes_read,
             bytes_read > 0 ? header_hex : "<empty>");
}

static esp_err_t preserve_invalid_database_file(const char *database_path)
{
    static const char sqlite_header[] = "SQLite format 3";
    char header[sizeof(sqlite_header)] = {0};
    int fd = open(database_path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Cannot inspect database file (errno %d)", errno);
        return ESP_FAIL;
    }

    size_t bytes_read = 0;
    while (bytes_read < sizeof(header)) {
        ssize_t result = read(fd, header + bytes_read, sizeof(header) - bytes_read);
        if (result > 0) {
            bytes_read += (size_t)result;
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else if (result == 0) {
            break;
        } else {
            int saved_errno = errno;
            close(fd);
            ESP_LOGE(TAG, "Cannot read database header (errno %d)", saved_errno);
            return ESP_FAIL;
        }
    }

    int close_result = close(fd);
    if (close_result != 0) {
        ESP_LOGE(TAG, "Cannot close database header check (errno %d)", errno);
        return ESP_FAIL;
    }
    if (bytes_read == sizeof(header) &&
        memcmp(header, sqlite_header, sizeof(sqlite_header) - 1) == 0 &&
        header[sizeof(sqlite_header) - 1] == '\0') {
        return ESP_OK;
    }

    char preserved_path[sizeof(CONFIG_SD_CARD_MANAGER_MOUNT_POINT) +
                        sizeof("/ebc_a20.db.invalid-00000000")];
    int path_length = snprintf(preserved_path, sizeof(preserved_path),
                               "%s.invalid-%08" PRIx32, database_path, esp_random());
    if (path_length <= 0 || (size_t)path_length >= sizeof(preserved_path)) {
        ESP_LOGE(TAG, "Cannot create a safe archive path for invalid database");
        return ESP_ERR_INVALID_SIZE;
    }
    if (rename(database_path, preserved_path) != 0) {
        ESP_LOGE(TAG, "Cannot preserve invalid database file (errno %d)", errno);
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "Preserved file without a valid SQLite header as %s", preserved_path);
    return ESP_OK;
}

static int read_schema_version(sqlite3 *database, int *schema_version)
{
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(
        database,
        "SELECT schema_version FROM ebc_schema_metadata WHERE singleton = 1;",
        -1, &statement, NULL);
    if (result == SQLITE_OK) {
        result = sqlite3_step(statement);
        if (result == SQLITE_ROW) {
            *schema_version = sqlite3_column_int(statement, 0);
            result = SQLITE_OK;
        } else if (result == SQLITE_DONE) {
            result = SQLITE_CORRUPT;
        }
    }
    sqlite3_finalize(statement);
    return result;
}

static int count_user_tables(sqlite3 *database, int *table_count)
{
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(
        database, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table';",
        -1, &statement, NULL);
    if (result == SQLITE_OK) {
        result = sqlite3_step(statement);
        if (result == SQLITE_ROW) {
            *table_count = sqlite3_column_int(statement, 0);
            result = SQLITE_OK;
        }
    }
    sqlite3_finalize(statement);
    return result;
}

static int validate_schema(sqlite3 *database)
{
    static const char *const expected_objects[] = {
        "ebc_test_plan",
        "ebc_test_plan_step",
        "ebc_battery",
        "ebc_test_run",
        "ebc_run_segment",
        "ebc_measurement",
        "ebc_schema_metadata",
        "ebc_measurement_view",
        "ebc_resistance_result",
        "ebc_resistance_result_view",
    };
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(
        database,
        "SELECT COUNT(*) FROM sqlite_master WHERE name IN ("
        "'ebc_test_plan', 'ebc_test_plan_step', 'ebc_battery', "
        "'ebc_test_run', 'ebc_run_segment', 'ebc_measurement', "
        "'ebc_schema_metadata', 'ebc_measurement_view', "
        "'ebc_resistance_result', 'ebc_resistance_result_view');",
        -1, &statement, NULL);
    if (result == SQLITE_OK) {
        result = sqlite3_step(statement);
        if (result == SQLITE_ROW) {
            int found = sqlite3_column_int(statement, 0);
            result = found == sizeof(expected_objects) / sizeof(expected_objects[0])
                         ? SQLITE_OK
                         : SQLITE_SCHEMA;
        }
    }
    sqlite3_finalize(statement);
    return result;
}

static esp_err_t initialize_database_locked(void)
{
    ESP_LOGI(TAG, "Opening SQLite database on mounted SD card");
    char database_path[sizeof(CONFIG_SD_CARD_MANAGER_MOUNT_POINT) + sizeof(DATABASE_FILE_NAME)];
    int path_length = snprintf(database_path, sizeof(database_path), "%s%s",
                               CONFIG_SD_CARD_MANAGER_MOUNT_POINT, DATABASE_FILE_NAME);
    if (path_length <= 0 || (size_t)path_length >= sizeof(database_path)) {
        ESP_LOGE(TAG, "Database path exceeds buffer");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t preserve_err = preserve_invalid_database_file(database_path);
    if (preserve_err != ESP_OK) {
        return preserve_err;
    }

    sqlite3 *database = NULL;
    sqlite3_vfs *vfs = sqlite3_vfs_find("esp-fatfs");
    if (vfs == NULL || vfs->iVersion < 1) {
        ESP_LOGE(TAG, "The esp-fatfs SQLite VFS is unavailable or invalid");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Using SQLite VFS %s (version %d)", vfs->zName, vfs->iVersion);
    int result = sqlite3_open_v2(database_path, &database,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                 vfs->zName);
    if (result != SQLITE_OK) {
        ESP_LOGE(TAG, "Cannot open %s: %s", database_path,
                 database != NULL ? sqlite3_errmsg(database) : sqlite3_errstr(result));
        if (database != NULL) {
            sqlite3_close(database);
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SQLite database opened");
    log_database_file_state(database_path, "open");
    char *error = NULL;
    const char *setup_sql =
        "PRAGMA journal_mode=DELETE;"
        "PRAGMA synchronous=FULL;"
        "PRAGMA foreign_keys=ON;"
        "PRAGMA busy_timeout=5000;";
    result = sqlite3_exec(database, setup_sql, NULL, NULL, &error);
    if (result != SQLITE_OK) {
        ESP_LOGE(TAG, "SQLite configuration failed (%d): %s",
                 result, error != NULL ? error : sqlite3_errmsg(database));
    }
    log_database_file_state(database_path, "configuration");
    int table_count = 0;
    if (result == SQLITE_OK) {
        result = count_user_tables(database, &table_count);
        if (result != SQLITE_OK) {
            ESP_LOGE(TAG, "Cannot inspect SQLite tables (%d): %s",
                     result, sqlite3_errmsg(database));
            log_database_file_state(database_path, "table inspection failure");
        }
    }
    int schema_version = 0;
    if (result == SQLITE_OK && table_count != 0) {
        result = read_schema_version(database, &schema_version);
        if (result != SQLITE_OK) {
            ESP_LOGE(TAG, "Cannot read schema version (%d): %s",
                     result, sqlite3_errmsg(database));
        }
    }
    if (result == SQLITE_OK && schema_version > 2) {
        ESP_LOGE(TAG, "Database schema version %d is newer than supported version 2",
                 schema_version);
        result = SQLITE_ERROR;
    }
    if (result == SQLITE_OK && table_count == 0) {
        ESP_LOGI(TAG, "Creating initial SQLite schema");
        result = sqlite3_exec(database, "BEGIN IMMEDIATE;", NULL, NULL, &error);
        if (result == SQLITE_OK) {
            result = sqlite3_exec(database, schema_sql_start, NULL, NULL, &error);
            if (result != SQLITE_OK) {
                ESP_LOGE(TAG, "Cannot create SQLite schema (%d): %s",
                         result, error != NULL ? error : sqlite3_errmsg(database));
            }
        }
        if (result == SQLITE_OK) {
            result = sqlite3_exec(database, "COMMIT;", NULL, NULL, &error);
        } else {
            sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
        }
    } else if (result == SQLITE_OK && schema_version != 1 && schema_version != 2) {
        ESP_LOGE(TAG, "Unsupported database schema version %d", schema_version);
        result = SQLITE_ERROR;
    }
    if (result == SQLITE_OK && schema_version < 2) {
        ESP_LOGI(TAG, "Migrating SQLite schema to version 2 (DCIR results)");
        result = sqlite3_exec(database, "BEGIN IMMEDIATE;", NULL, NULL, &error);
        if (result == SQLITE_OK) {
            result = sqlite3_exec(database, resistance_schema_start, NULL, NULL, &error);
        }
        if (result == SQLITE_OK) {
            result = sqlite3_exec(database, "COMMIT;", NULL, NULL, &error);
        } else {
            ESP_LOGE(TAG, "DCIR migration failed: %s", error != NULL ? error : sqlite3_errmsg(database));
            sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
        }
    }
    if (result == SQLITE_OK) {
        result = validate_schema(database);
        if (result != SQLITE_OK) {
            ESP_LOGE(TAG, "SQLite schema validation failed: %s",
                     sqlite3_errmsg(database));
        }
    }
    if (result == SQLITE_OK) {
        sqlite3_stmt *integrity = NULL;
        result = sqlite3_prepare_v2(database, "PRAGMA integrity_check;", -1,
                                    &integrity, NULL);
        if (result != SQLITE_OK) {
            ESP_LOGE(TAG, "Cannot prepare integrity check (%d): %s",
                     result, sqlite3_errmsg(database));
        }
        if (result == SQLITE_OK) {
            int step_result = sqlite3_step(integrity);
            const unsigned char *integrity_result =
                step_result == SQLITE_ROW ? sqlite3_column_text(integrity, 0) : NULL;
            if (integrity_result == NULL ||
                sqlite3_stricmp((const char *)integrity_result, "ok") != 0) {
                ESP_LOGE(TAG, "SQLite integrity check failed: %s",
                         integrity_result != NULL ? (const char *)integrity_result :
                                                    sqlite3_errmsg(database));
                result = step_result == SQLITE_ROW ? SQLITE_CORRUPT : step_result;
            } else if (sqlite3_step(integrity) != SQLITE_DONE) {
                ESP_LOGE(TAG, "SQLite integrity check returned unexpected extra results");
                result = SQLITE_CORRUPT;
            }
        }
        sqlite3_finalize(integrity);
    }

    if (result != SQLITE_OK) {
        ESP_LOGE(TAG, "Cannot initialize database (SQLite result %d): %s",
                 result,
                 error != NULL ? error : sqlite3_errmsg(database));
        if ((result & 0xFF) == SQLITE_NOMEM) {
            log_heap_state("SQLite out-of-memory");
        }
        sqlite3_free(error);
        sqlite3_close(database);
        return ESP_FAIL;
    }
    sqlite3_free(error);
    result = sqlite3_close(database);
    if (result != SQLITE_OK) {
        ESP_LOGE(TAG, "Cannot close database cleanly: %s", sqlite3_errstr(result));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void database_worker(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Initializing SQLite library");
    int sqlite_result = sqlite_esp_configure_memory();
    if (sqlite_result != SQLITE_OK) {
        ESP_LOGW(TAG, "Cannot install PSRAM-aware SQLite allocator: %s",
                 sqlite3_errstr(sqlite_result));
    }
    sqlite_result = sqlite3_initialize();
    if (sqlite_result != SQLITE_OK) {
        ESP_LOGE(TAG, "SQLite library initialization failed: %s",
                 sqlite3_errstr(sqlite_result));
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "SQLite library initialized");

    for (;;) {
        esp_err_t err = sd_card_manager_storage_acquire(SD_LOCK_TIMEOUT);
        if (err == ESP_OK) {
            err = initialize_database_locked();
            sd_card_manager_storage_release();
            if (err == ESP_OK) {
                database_ready.store(true);
                ESP_LOGI(TAG, "SQLite database is ready on the SD card");
                vTaskDelete(NULL);
            }
            ESP_LOGE(TAG, "Database initialization failed: %s", esp_err_to_name(err));
            vTaskDelete(NULL);
        }
        vTaskDelay(RETRY_INTERVAL);
    }
}

bool battery_database_is_ready(void)
{
    return database_ready.load();
}

esp_err_t battery_database_execute(battery_database_operation_t operation, void *context,
                                   TickType_t timeout_ticks)
{
    if (operation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!database_ready.load()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = sd_card_manager_storage_acquire(timeout_ticks);
    if (err != ESP_OK) {
        return err;
    }
    char database_path[sizeof(CONFIG_SD_CARD_MANAGER_MOUNT_POINT) + sizeof(DATABASE_FILE_NAME)];
    snprintf(database_path, sizeof(database_path), "%s%s",
             CONFIG_SD_CARD_MANAGER_MOUNT_POINT, DATABASE_FILE_NAME);
    sqlite3 *database = NULL;
    int result = sqlite3_open_v2(database_path, &database, SQLITE_OPEN_READWRITE, "esp-fatfs");
    if (result == SQLITE_OK) {
        result = sqlite3_exec(database, "PRAGMA foreign_keys=ON;PRAGMA busy_timeout=5000;",
                              NULL, NULL, NULL);
    }
    if (result != SQLITE_OK) {
        ESP_LOGE(TAG, "Cannot open database for operation (%d): %s", result,
                 database != NULL ? sqlite3_errmsg(database) : sqlite3_errstr(result));
        if ((result & 0xFF) == SQLITE_NOMEM) {
            log_heap_state("SQLite open");
        }
        sqlite3_close(database);
        sd_card_manager_storage_release();
        return ESP_FAIL;
    }
    err = operation(database, context);
    result = sqlite3_close(database);
    if (result != SQLITE_OK) {
        ESP_LOGE(TAG, "Cannot close database after operation: %s", sqlite3_errstr(result));
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
    }
    sd_card_manager_storage_release();
    return err;
}

esp_err_t battery_database_init(void)
{
    if (xTaskCreate(database_worker, "battery_db_init", 8192, NULL,
                    DATABASE_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create database initialization task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
