-- SPDX-FileCopyrightText: 2026 Thomas Stoll
-- SPDX-License-Identifier: Apache-2.0

CREATE TABLE IF NOT EXISTS ebc_resistance_result (
  run_id TEXT NOT NULL COLLATE BINARY,
  result_number INTEGER NOT NULL CHECK (result_number BETWEEN 1 AND 3),
  captured_at_utc TEXT,
  device_uptime_ms INTEGER,
  requested_seconds REAL NOT NULL,
  actual_seconds REAL,
  baseline_voltage_v REAL NOT NULL,
  baseline_current_a REAL NOT NULL,
  load_voltage_v REAL,
  load_current_a REAL,
  resistance_mohm REAL,
  onset_uncertainty_seconds REAL NOT NULL,
  valid INTEGER NOT NULL CHECK (valid IN (0, 1)),
  error TEXT,
  PRIMARY KEY (run_id, result_number),
  FOREIGN KEY (run_id) REFERENCES ebc_test_run(run_id) ON DELETE CASCADE,
  CHECK (valid = 0 OR (resistance_mohm IS NOT NULL AND load_current_a IS NOT NULL AND load_current_a > 0))
);
CREATE INDEX IF NOT EXISTS ix_resistance_captured ON ebc_resistance_result(captured_at_utc);
CREATE VIEW IF NOT EXISTS ebc_resistance_result_view AS
SELECT x.*, r.battery_id, b.label AS battery_label, r.device_id,
       r.started_at_utc AS test_started_at_utc, r.status AS run_status
FROM ebc_resistance_result x
JOIN ebc_test_run r ON r.run_id = x.run_id
LEFT JOIN ebc_battery b ON b.battery_id = r.battery_id;
UPDATE ebc_schema_metadata SET schema_version = 2 WHERE singleton = 1;
