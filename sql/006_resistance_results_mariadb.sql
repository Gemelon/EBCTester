-- SPDX-FileCopyrightText: 2026 Thomas Stoll
-- SPDX-License-Identifier: Apache-2.0

-- Select the existing BatteryTester database in your SQL client first.
-- Run as a schema administrator, not the restricted firmware account.
-- Match the parent run_id type and collation exactly for its foreign key.
SET @run_type = NULL;
SET @run_charset = NULL;
SET @run_collation = NULL;
SELECT COLUMN_TYPE, CHARACTER_SET_NAME, COLLATION_NAME
INTO @run_type, @run_charset, @run_collation
FROM information_schema.COLUMNS
WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ebc_test_run' AND COLUMN_NAME = 'run_id';

SET @create_sql = CONCAT(
  'CREATE TABLE IF NOT EXISTS ebc_resistance_result (',
  'run_id ', @run_type,
  IF(@run_charset IS NULL, '', CONCAT(' CHARACTER SET ', @run_charset, ' COLLATE ', @run_collation)),
  ' NOT NULL,',
  'result_number TINYINT UNSIGNED NOT NULL,',
  'captured_at_utc DATETIME(3) NULL,',
  'device_uptime_ms BIGINT NULL,',
  'requested_seconds DOUBLE NOT NULL, actual_seconds DOUBLE NULL,',
  'baseline_voltage_v DOUBLE NOT NULL, baseline_current_a DOUBLE NOT NULL,',
  'load_voltage_v DOUBLE NULL, load_current_a DOUBLE NULL, resistance_mohm DOUBLE NULL,',
  'onset_uncertainty_seconds DOUBLE NOT NULL, valid TINYINT NOT NULL, error TEXT NULL,',
  'PRIMARY KEY (run_id, result_number),',
  'INDEX ix_resistance_captured (captured_at_utc),',
  'CONSTRAINT fk_resistance_run FOREIGN KEY (run_id) REFERENCES ebc_test_run(run_id) ON DELETE CASCADE,',
  'CHECK (result_number BETWEEN 1 AND 3), CHECK (valid IN (0,1)),',
  'CHECK (valid = 0 OR (resistance_mohm IS NOT NULL AND load_current_a IS NOT NULL AND load_current_a > 0))',
  ') ENGINE=InnoDB');
-- Missing parent table / wrong selected database deliberately causes an error.
PREPARE migration FROM @create_sql;
EXECUTE migration;
DEALLOCATE PREPARE migration;

CREATE OR REPLACE VIEW ebc_resistance_result_view AS
SELECT x.*, r.battery_id, b.label AS battery_label, r.device_id,
       r.started_at_utc AS test_started_at_utc, r.status AS run_status
FROM ebc_resistance_result x
JOIN ebc_test_run r ON r.run_id = x.run_id
LEFT JOIN ebc_battery b ON b.battery_id = r.battery_id;

-- Grant SELECT, INSERT, UPDATE on ebc_resistance_result to the firmware account.
-- Substitute your database, user and host before executing, e.g.:
-- GRANT SELECT, INSERT, UPDATE ON ebc_a20.ebc_resistance_result TO 'your_user'@'your_host';
-- For reporting via the view, grant SELECT on ebc_resistance_result_view too.
-- All captured_at_utc / test_started_at_utc values are UTC.
