/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mariadb_schema.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "mariadb_client.h"

namespace mariadb_sync {
namespace {

enum class ColumnKind { Text, Integer, Real, DateTime };

struct Column {
    const char *name;
    ColumnKind kind;
    const char *add_definition;
    bool key;
};

struct Table {
    const char *name;
    const Column *columns;
    size_t column_count;
};

struct Index {
    const char *table;
    const char *name;
    const char *columns;
    bool primary;
    bool unique;
};

const Column PLAN_COLUMNS[] = {
    {"plan_id", ColumnKind::Integer, "BIGINT UNSIGNED NULL", true},
    {"device_id", ColumnKind::Text, "VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL", true},
    {"plan_name", ColumnKind::Text, "VARCHAR(128) NULL", true},
    {"plan_version", ColumnKind::Integer, "INT NULL", false},
    {"plan_definition", ColumnKind::Text, "LONGTEXT NULL", false},
    {"created_at_utc", ColumnKind::DateTime, "DATETIME(3) NULL", false},
    {"updated_at_utc", ColumnKind::DateTime, "DATETIME(3) NULL", false},
};
const Column STEP_COLUMNS[] = {
    {"plan_id", ColumnKind::Integer, "BIGINT UNSIGNED NULL", true},
    {"step_number", ColumnKind::Integer, "INT NULL", true},
    {"mode", ColumnKind::Text, "VARCHAR(8) NULL", false},
    {"current_a", ColumnKind::Real, "DOUBLE NULL", false},
    {"power_w", ColumnKind::Real, "DOUBLE NULL", false},
    {"voltage_v", ColumnKind::Real, "DOUBLE NULL", false},
    {"cutoff_current_a", ColumnKind::Real, "DOUBLE NULL", false},
    {"max_runtime_minutes", ColumnKind::Integer, "INT NULL", false},
    {"wait_minutes", ColumnKind::Integer, "INT NULL", false},
    {"cycle_total_runs", ColumnKind::Integer, "INT NULL", false},
};
const Column BATTERY_COLUMNS[] = {
    {"battery_id", ColumnKind::Integer, "BIGINT UNSIGNED NULL", true},
    {"label", ColumnKind::Text, "VARCHAR(191) NULL", true},
    {"manufacturer", ColumnKind::Text, "VARCHAR(255) NULL", false},
    {"nominal_capacity_ah", ColumnKind::Real, "DOUBLE NULL", false},
    {"serial_number", ColumnKind::Text, "VARCHAR(128) NULL", false},
    {"manufactured_on", ColumnKind::Text, "VARCHAR(32) NULL", false},
    {"notes", ColumnKind::Text, "TEXT NULL", false},
    {"created_at_utc", ColumnKind::DateTime, "DATETIME(3) NULL", false},
    {"updated_at_utc", ColumnKind::DateTime, "DATETIME(3) NULL", false},
};
const Column RUN_COLUMNS[] = {
    {"run_id", ColumnKind::Text, "CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL", true},
    {"device_id", ColumnKind::Text, "VARCHAR(64) NULL", false},
    {"plan_id", ColumnKind::Integer, "BIGINT UNSIGNED NULL", false},
    {"plan_version", ColumnKind::Integer, "INT NULL", false},
    {"battery_id", ColumnKind::Integer, "BIGINT UNSIGNED NULL", false},
    {"started_at_utc", ColumnKind::DateTime, "DATETIME(3) NULL", false},
    {"ended_at_utc", ColumnKind::DateTime, "DATETIME(3) NULL", false},
    {"status", ColumnKind::Text, "VARCHAR(16) NULL", false},
    {"current_step", ColumnKind::Integer, "INT NULL", false},
    {"cycle_pass", ColumnKind::Integer, "INT NULL", false},
    {"firmware_version", ColumnKind::Text, "VARCHAR(32) NULL", false},
    {"notes", ColumnKind::Text, "TEXT NULL", false},
};
const Column SEGMENT_COLUMNS[] = {
    {"run_id", ColumnKind::Text, "CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL", true},
    {"segment_number", ColumnKind::Integer, "INT NULL", true},
    {"step_number", ColumnKind::Integer, "INT NULL", false},
    {"cycle_pass", ColumnKind::Integer, "INT NULL", false},
    {"setpoint_1", ColumnKind::Real, "DOUBLE NULL", false},
    {"setpoint_2", ColumnKind::Real, "DOUBLE NULL", false},
    {"setpoint_3", ColumnKind::Real, "DOUBLE NULL", false},
    {"first_sequence", ColumnKind::Integer, "BIGINT NULL", false},
};
const Column MEASUREMENT_COLUMNS[] = {
    {"measurement_id", ColumnKind::Integer, "BIGINT UNSIGNED NULL", true},
    {"run_id", ColumnKind::Text, "CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL", true},
    {"sequence_number", ColumnKind::Integer, "BIGINT NULL", true},
    {"segment_number", ColumnKind::Integer, "INT NULL", false},
    {"captured_at_utc", ColumnKind::DateTime, "DATETIME(3) NULL", false},
    {"device_uptime_ms", ColumnKind::Integer, "BIGINT NULL", false},
    {"mode_code", ColumnKind::Integer, "INT NULL", false},
    {"current_a", ColumnKind::Real, "DOUBLE NULL", false},
    {"voltage_v", ColumnKind::Real, "DOUBLE NULL", false},
    {"capacity_ah", ColumnKind::Real, "DOUBLE NULL", false},
    {"model_id", ColumnKind::Integer, "INT NULL", false},
};
const Column RESISTANCE_COLUMNS[] = {
    {"run_id", ColumnKind::Text, "CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL", true},
    {"result_number", ColumnKind::Integer, "TINYINT UNSIGNED NULL", true},
    {"captured_at_utc", ColumnKind::DateTime, "DATETIME(3) NULL", false},
    {"device_uptime_ms", ColumnKind::Integer, "BIGINT NULL", false},
    {"requested_seconds", ColumnKind::Real, "DOUBLE NULL", false},
    {"actual_seconds", ColumnKind::Real, "DOUBLE NULL", false},
    {"baseline_voltage_v", ColumnKind::Real, "DOUBLE NULL", false},
    {"baseline_current_a", ColumnKind::Real, "DOUBLE NULL", false},
    {"load_voltage_v", ColumnKind::Real, "DOUBLE NULL", false},
    {"load_current_a", ColumnKind::Real, "DOUBLE NULL", false},
    {"resistance_mohm", ColumnKind::Real, "DOUBLE NULL", false},
    {"onset_uncertainty_seconds", ColumnKind::Real, "DOUBLE NULL", false},
    {"valid", ColumnKind::Integer, "TINYINT NULL", false},
    {"error", ColumnKind::Text, "TEXT NULL", false},
};

const Table TABLES[] = {
    {"ebc_test_plan", PLAN_COLUMNS, sizeof(PLAN_COLUMNS) / sizeof(PLAN_COLUMNS[0])},
    {"ebc_test_plan_step", STEP_COLUMNS, sizeof(STEP_COLUMNS) / sizeof(STEP_COLUMNS[0])},
    {"ebc_battery", BATTERY_COLUMNS, sizeof(BATTERY_COLUMNS) / sizeof(BATTERY_COLUMNS[0])},
    {"ebc_test_run", RUN_COLUMNS, sizeof(RUN_COLUMNS) / sizeof(RUN_COLUMNS[0])},
    {"ebc_run_segment", SEGMENT_COLUMNS, sizeof(SEGMENT_COLUMNS) / sizeof(SEGMENT_COLUMNS[0])},
    {"ebc_measurement", MEASUREMENT_COLUMNS, sizeof(MEASUREMENT_COLUMNS) / sizeof(MEASUREMENT_COLUMNS[0])},
    {"ebc_resistance_result", RESISTANCE_COLUMNS,
     sizeof(RESISTANCE_COLUMNS) / sizeof(RESISTANCE_COLUMNS[0])},
};

const char *CREATE_TABLES[] = {
    "CREATE TABLE IF NOT EXISTS ebc_test_plan ("
    "plan_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "device_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
    "plan_name VARCHAR(128) NOT NULL, plan_version INT NOT NULL, plan_definition LONGTEXT NULL,"
    "created_at_utc DATETIME(3) NOT NULL, updated_at_utc DATETIME(3) NULL,"
    "PRIMARY KEY (plan_id), UNIQUE KEY uq_test_plan_device_name (device_id, plan_name)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
    "CREATE TABLE IF NOT EXISTS ebc_test_plan_step ("
    "plan_id BIGINT UNSIGNED NOT NULL, step_number INT NOT NULL, mode VARCHAR(8) NOT NULL,"
    "current_a DOUBLE NULL, power_w DOUBLE NULL, voltage_v DOUBLE NULL, cutoff_current_a DOUBLE NULL,"
    "max_runtime_minutes INT NULL, wait_minutes INT NULL, cycle_total_runs INT NULL,"
    "PRIMARY KEY (plan_id, step_number)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
    "CREATE TABLE IF NOT EXISTS ebc_battery ("
    "battery_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, label VARCHAR(191) NOT NULL,"
    "manufacturer VARCHAR(255) NULL, nominal_capacity_ah DOUBLE NULL, serial_number VARCHAR(128) NULL,"
    "manufactured_on VARCHAR(32) NULL, notes TEXT NULL, created_at_utc DATETIME(3) NOT NULL,"
    "updated_at_utc DATETIME(3) NULL, PRIMARY KEY (battery_id), UNIQUE KEY uq_battery_label (label),"
    "KEY ix_battery_serial (serial_number)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
    "CREATE TABLE IF NOT EXISTS ebc_test_run ("
    "run_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, device_id VARCHAR(64) NOT NULL,"
    "plan_id BIGINT UNSIGNED NULL, plan_version INT NULL, battery_id BIGINT UNSIGNED NULL,"
    "started_at_utc DATETIME(3) NOT NULL, ended_at_utc DATETIME(3) NULL, status VARCHAR(16) NOT NULL,"
    "current_step INT NULL, cycle_pass INT NULL, firmware_version VARCHAR(32) NULL, notes TEXT NULL,"
    "PRIMARY KEY (run_id), KEY ix_run_device_started (device_id, started_at_utc), KEY ix_run_battery (battery_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
    "CREATE TABLE IF NOT EXISTS ebc_run_segment ("
    "run_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, segment_number INT NOT NULL,"
    "step_number INT NOT NULL, cycle_pass INT NOT NULL, setpoint_1 DOUBLE NULL, setpoint_2 DOUBLE NULL,"
    "setpoint_3 DOUBLE NULL, first_sequence BIGINT NOT NULL, PRIMARY KEY (run_id, segment_number)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
    "CREATE TABLE IF NOT EXISTS ebc_measurement ("
    "measurement_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    "run_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, sequence_number BIGINT NOT NULL,"
    "segment_number INT NOT NULL, captured_at_utc DATETIME(3) NULL, device_uptime_ms BIGINT NOT NULL,"
    "mode_code INT NOT NULL, current_a DOUBLE NOT NULL, voltage_v DOUBLE NOT NULL, capacity_ah DOUBLE NOT NULL,"
    "model_id INT NULL, PRIMARY KEY (measurement_id), UNIQUE KEY uq_measurement_run_sequence (run_id, sequence_number),"
    "KEY ix_measurement_run_time (run_id, captured_at_utc), KEY ix_measurement_segment (run_id, segment_number)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
    "CREATE TABLE IF NOT EXISTS ebc_resistance_result ("
    "run_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, result_number TINYINT UNSIGNED NOT NULL,"
    "captured_at_utc DATETIME(3) NULL, device_uptime_ms BIGINT NULL, requested_seconds DOUBLE NOT NULL,"
    "actual_seconds DOUBLE NULL, baseline_voltage_v DOUBLE NOT NULL, baseline_current_a DOUBLE NOT NULL,"
    "load_voltage_v DOUBLE NULL, load_current_a DOUBLE NULL, resistance_mohm DOUBLE NULL,"
    "onset_uncertainty_seconds DOUBLE NOT NULL, valid TINYINT NOT NULL, error TEXT NULL,"
    "PRIMARY KEY (run_id, result_number), KEY ix_resistance_captured (captured_at_utc),"
    "CHECK (result_number BETWEEN 1 AND 3), CHECK (valid IN (0,1)),"
    "CHECK (valid = 0 OR (resistance_mohm IS NOT NULL AND load_current_a IS NOT NULL AND load_current_a > 0))"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
};

const Index INDEXES[] = {
    {"ebc_test_plan", "uq_test_plan_device_name", "device_id,plan_name", false, true},
    {"ebc_test_plan_step", "PRIMARY", "plan_id,step_number", true, true},
    {"ebc_battery", "uq_battery_label", "label", false, true},
    {"ebc_test_run", "PRIMARY", "run_id", true, true},
    {"ebc_run_segment", "PRIMARY", "run_id,segment_number", true, true},
    {"ebc_measurement", "PRIMARY", "measurement_id", true, true},
    {"ebc_measurement", "uq_measurement_run_sequence", "run_id,sequence_number", false, true},
    {"ebc_resistance_result", "PRIMARY", "run_id,result_number", true, true},
};

using ColumnsByTable = std::map<std::string, std::map<std::string, std::string>>;
struct IndexInfo {
    std::vector<std::string> columns;
    bool unique = false;
};
using IndexesByTable = std::map<std::string, std::map<std::string, IndexInfo>>;

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool read_columns(MariaDbClient &client, ColumnsByTable &columns, std::string &error)
{
    std::vector<std::vector<std::string>> rows;
    if (!client.query("SELECT TABLE_NAME, COLUMN_NAME, DATA_TYPE FROM information_schema.COLUMNS"
                      " WHERE TABLE_SCHEMA = DATABASE()",
                      rows, error, 256)) {
        return false;
    }
    for (const auto &row : rows) {
        if (row.size() == 3) {
            columns[lower(row[0])][lower(row[1])] = lower(row[2]);
        }
    }
    return true;
}

bool read_indexes(MariaDbClient &client, IndexesByTable &indexes, std::string &error)
{
    std::vector<std::vector<std::string>> rows;
    if (!client.query("SELECT TABLE_NAME, INDEX_NAME, SEQ_IN_INDEX, COLUMN_NAME, NON_UNIQUE"
                      " FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = DATABASE()"
                      " ORDER BY TABLE_NAME, INDEX_NAME, SEQ_IN_INDEX",
                      rows, error, 512)) {
        return false;
    }
    for (const auto &row : rows) {
        if (row.size() == 5) {
            IndexInfo &index = indexes[lower(row[0])][lower(row[1])];
            index.columns.push_back(lower(row[3]));
            index.unique = row[4] == "0";
        }
    }
    return true;
}

bool matches_kind(ColumnKind kind, const std::string &type)
{
    if (kind == ColumnKind::Text) {
        return type.find("char") != std::string::npos || type.find("text") != std::string::npos ||
               type == "enum" || type == "set";
    }
    if (kind == ColumnKind::DateTime) {
        return type == "datetime" || type == "timestamp";
    }
    if (kind == ColumnKind::Integer) {
        return type == "tinyint" || type == "smallint" || type == "mediumint" ||
               type == "int" || type == "integer" || type == "bigint" || type == "bit";
    }
    return type == "decimal" || type == "numeric" || type == "float" || type == "double";
}

bool has_index(const IndexesByTable &indexes, const Index &expected)
{
    const auto table = indexes.find(expected.table);
    if (table == indexes.end()) {
        return false;
    }
    std::vector<std::string> expected_columns;
    std::string columns = expected.columns;
    size_t start = 0;
    while (start <= columns.size()) {
        const size_t end = columns.find(',', start);
        expected_columns.push_back(lower(columns.substr(start, end - start)));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    for (const auto &index : table->second) {
        if (expected.primary && index.first != "primary") {
            continue;
        }
        if ((!expected.unique || index.second.unique) && index.second.columns == expected_columns) {
            return true;
        }
    }
    return false;
}

bool scalar_is_zero(MariaDbClient &client, const std::string &query, bool &is_zero, std::string &error)
{
    std::vector<std::vector<std::string>> rows;
    if (!client.query(query, rows, error, 1) || rows.empty() || rows[0].empty()) {
        if (error.empty()) {
            error = "Could not check whether an existing table is empty";
        }
        return false;
    }
    is_zero = rows[0][0] == "0";
    return true;
}

SchemaCheck inspect(MariaDbClient &client, ColumnsByTable &columns, IndexesByTable &indexes)
{
    std::string error;
    if (!read_columns(client, columns, error) || !read_indexes(client, indexes, error)) {
        return {SchemaResultState::Error, "Could not inspect MariaDB structure: " + error};
    }

    std::vector<std::string> missing;
    std::vector<std::string> incompatible;
    for (const Table &table : TABLES) {
        const auto found_table = columns.find(table.name);
        if (found_table == columns.end()) {
            missing.push_back(table.name);
            continue;
        }
        for (size_t i = 0; i < table.column_count; ++i) {
            const Column &column = table.columns[i];
            const auto found_column = found_table->second.find(column.name);
            if (found_column == found_table->second.end()) {
                missing.push_back(std::string(table.name) + "." + column.name);
            } else if (!matches_kind(column.kind, found_column->second)) {
                incompatible.push_back(std::string(table.name) + "." + column.name);
            }
        }
    }
    for (const Index &index : INDEXES) {
        if (!has_index(indexes, index)) {
            missing.push_back(std::string(index.table) + "." + index.name);
        }
    }
    if (!missing.empty()) {
        std::string message = "Missing MariaDB tables or fields: ";
        for (size_t i = 0; i < missing.size(); ++i) {
            message += i == 0 ? "" : ", ";
            message += missing[i];
        }
        return {SchemaResultState::NeedsMigration, message};
    }
    if (!incompatible.empty()) {
        std::string message = "Incompatible MariaDB fields: ";
        for (size_t i = 0; i < incompatible.size(); ++i) {
            message += i == 0 ? "" : ", ";
            message += incompatible[i];
        }
        return {SchemaResultState::Incompatible, message};
    }
    return {SchemaResultState::Ready, "MariaDB structure is ready."};
}

} // namespace

SchemaCheck inspect_schema(MariaDbClient &client)
{
    ColumnsByTable columns;
    IndexesByTable indexes;
    return inspect(client, columns, indexes);
}

bool migrate_schema(MariaDbClient &client, std::string &error)
{
    for (const char *statement : CREATE_TABLES) {
        if (!client.execute(statement, error)) {
            error = "Could not create the MariaDB schema: " + error;
            return false;
        }
    }

    ColumnsByTable columns;
    IndexesByTable indexes;
    if (!read_columns(client, columns, error)) {
        error = "Could not inspect MariaDB fields: " + error;
        return false;
    }
    for (const Table &table : TABLES) {
        const auto found_table = columns.find(table.name);
        if (found_table == columns.end()) {
            error = std::string("MariaDB table was not created: ") + table.name;
            return false;
        }
        for (size_t i = 0; i < table.column_count; ++i) {
            const Column &column = table.columns[i];
            if (found_table->second.find(column.name) != found_table->second.end()) {
                continue;
            }
            if (column.key) {
                error = std::string("Cannot safely add missing key field ") + table.name + "." +
                        column.name + " to an existing table. Back up and repair that table manually.";
                return false;
            }
            const std::string statement = std::string("ALTER TABLE `") + table.name + "` ADD COLUMN `" +
                                          column.name + "` " + column.add_definition;
            if (!client.execute(statement, error)) {
                error = std::string("Could not add field ") + table.name + "." + column.name + ": " + error;
                return false;
            }
        }
    }

    if (!read_indexes(client, indexes, error)) {
        error = "Could not inspect MariaDB indexes: " + error;
        return false;
    }
    for (const Index &index : INDEXES) {
        if (has_index(indexes, index)) {
            continue;
        }
        if (index.primary) {
            bool empty = false;
            if (!scalar_is_zero(client, std::string("SELECT COUNT(*) FROM `") + index.table + "`",
                                empty, error)) {
                return false;
            }
            if (!empty) {
                error = std::string("Cannot safely add missing primary key to non-empty table ") + index.table +
                        ". Back up and repair that table manually.";
                return false;
            }
            const std::string statement = std::string("ALTER TABLE `") + index.table +
                                          "` ADD PRIMARY KEY (" + index.columns + ")";
            if (!client.execute(statement, error)) {
                error = std::string("Could not add primary key to ") + index.table + ": " + error;
                return false;
            }
        } else {
            const std::string statement = std::string("CREATE UNIQUE INDEX `") + index.name + "` ON `" +
                                          index.table + "` (" + index.columns + ")";
            if (!client.execute(statement, error)) {
                error = std::string("Could not add required unique index to ") + index.table + ": " + error;
                return false;
            }
        }
        if (!read_indexes(client, indexes, error)) {
            error = "Could not verify MariaDB indexes: " + error;
            return false;
        }
    }

    const SchemaCheck result = inspect_schema(client);
    if (result.state != SchemaResultState::Ready) {
        error = result.message;
        return false;
    }
    return true;
}

} // namespace mariadb_sync
