-- SPDX-FileCopyrightText: 2026 Thomas Stoll
-- SPDX-License-Identifier: Apache-2.0

CREATE TABLE IF NOT EXISTS ebc_schema_metadata (
  singleton INTEGER PRIMARY KEY,
  schema_version INTEGER NOT NULL
);
INSERT OR IGNORE INTO ebc_schema_metadata (singleton, schema_version) VALUES (1, 1);

CREATE TABLE IF NOT EXISTS ebc_test_plan (
  plan_id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  plan_name TEXT NOT NULL COLLATE NOCASE,
  plan_version INTEGER NOT NULL,
  plan_definition TEXT,
  created_at_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at_utc TEXT,
  UNIQUE (device_id, plan_name)
);

CREATE TABLE IF NOT EXISTS ebc_test_plan_step (
  plan_id INTEGER NOT NULL,
  step_number INTEGER NOT NULL,
  mode TEXT NOT NULL CHECK (mode IN ('D-CC', 'D-CP', 'C-CV', 'WAIT', 'CYCLE')),
  current_a NUMERIC,
  power_w NUMERIC,
  voltage_v NUMERIC,
  cutoff_current_a NUMERIC,
  max_runtime_minutes INTEGER,
  wait_minutes INTEGER,
  cycle_total_runs INTEGER,
  PRIMARY KEY (plan_id, step_number),
  FOREIGN KEY (plan_id) REFERENCES ebc_test_plan (plan_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS ebc_battery (
  battery_id INTEGER PRIMARY KEY AUTOINCREMENT,
  label TEXT NOT NULL COLLATE NOCASE UNIQUE,
  manufacturer TEXT,
  nominal_capacity_ah NUMERIC,
  serial_number TEXT,
  manufactured_on TEXT,
  notes TEXT,
  created_at_utc TEXT NOT NULL,
  updated_at_utc TEXT
);
CREATE INDEX IF NOT EXISTS ix_battery_serial ON ebc_battery (serial_number);

CREATE TABLE IF NOT EXISTS ebc_test_run (
  run_id TEXT PRIMARY KEY COLLATE BINARY,
  device_id TEXT NOT NULL,
  plan_id INTEGER,
  plan_version INTEGER,
  battery_id INTEGER,
  started_at_utc TEXT NOT NULL,
  ended_at_utc TEXT,
  status TEXT NOT NULL CHECK (status IN ('RUNNING', 'COMPLETED', 'STOPPED', 'INTERRUPTED', 'ERROR')),
  current_step INTEGER,
  cycle_pass INTEGER,
  firmware_version TEXT,
  notes TEXT,
  FOREIGN KEY (plan_id) REFERENCES ebc_test_plan (plan_id) ON DELETE SET NULL,
  FOREIGN KEY (battery_id) REFERENCES ebc_battery (battery_id)
);
CREATE INDEX IF NOT EXISTS ix_run_device_started ON ebc_test_run (device_id, started_at_utc);
CREATE INDEX IF NOT EXISTS ix_run_battery ON ebc_test_run (battery_id);

CREATE TABLE IF NOT EXISTS ebc_run_segment (
  run_id TEXT NOT NULL COLLATE BINARY,
  segment_number INTEGER NOT NULL,
  step_number INTEGER NOT NULL,
  cycle_pass INTEGER NOT NULL,
  setpoint_1 NUMERIC,
  setpoint_2 NUMERIC,
  setpoint_3 NUMERIC,
  first_sequence INTEGER NOT NULL,
  PRIMARY KEY (run_id, segment_number),
  FOREIGN KEY (run_id) REFERENCES ebc_test_run (run_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS ebc_measurement (
  measurement_id INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id TEXT NOT NULL COLLATE BINARY,
  sequence_number INTEGER NOT NULL,
  segment_number INTEGER NOT NULL,
  captured_at_utc TEXT,
  device_uptime_ms INTEGER NOT NULL,
  mode_code INTEGER NOT NULL,
  current_a NUMERIC NOT NULL,
  voltage_v NUMERIC NOT NULL,
  capacity_ah NUMERIC NOT NULL,
  model_id INTEGER,
  UNIQUE (run_id, sequence_number),
  FOREIGN KEY (run_id) REFERENCES ebc_test_run (run_id) ON DELETE CASCADE,
  FOREIGN KEY (run_id, segment_number)
    REFERENCES ebc_run_segment (run_id, segment_number) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS ix_measurement_run_time ON ebc_measurement (run_id, captured_at_utc);
CREATE INDEX IF NOT EXISTS ix_measurement_segment ON ebc_measurement (run_id, segment_number);

CREATE VIEW IF NOT EXISTS ebc_measurement_view AS
SELECT
  m.measurement_id,
  m.run_id,
  m.sequence_number,
  m.captured_at_utc,
  m.device_uptime_ms,
  m.mode_code,
  CASE m.mode_code
    WHEN 0x00 THEN 'D-CC stopped' WHEN 0x01 THEN 'D-CP stopped' WHEN 0x02 THEN 'C-CV stopped'
    WHEN 0x0A THEN 'D-CC active' WHEN 0x0B THEN 'D-CP active' WHEN 0x0C THEN 'C-CV active'
    WHEN 0x14 THEN 'D-CC complete' WHEN 0x15 THEN 'D-CP complete' WHEN 0x16 THEN 'C-CV complete'
    ELSE printf('unknown 0x%02X', m.mode_code)
  END AS mode_text,
  CASE
    WHEN m.mode_code IN (0x00, 0x01, 0x0A, 0x0B, 0x14, 0x15) THEN 'DISCHARGE'
    WHEN m.mode_code IN (0x02, 0x0C, 0x16) THEN 'CHARGE'
    ELSE NULL
  END AS direction,
  m.current_a,
  CASE WHEN m.mode_code IN (0x00, 0x01, 0x0A, 0x0B, 0x14, 0x15)
       THEN -m.current_a ELSE m.current_a END AS current_signed_a,
  m.voltage_v,
  ROUND(CASE WHEN m.mode_code IN (0x00, 0x01, 0x0A, 0x0B, 0x14, 0x15)
             THEN -m.current_a ELSE m.current_a END * m.voltage_v, 3) AS power_signed_w,
  m.capacity_ah,
  ROUND(m.capacity_ah / NULLIF(b.nominal_capacity_ah, 0) * 100, 1)
    AS capacity_percent_of_nominal,
  m.model_id,
  m.segment_number,
  s.step_number,
  s.cycle_pass,
  s.setpoint_1,
  s.setpoint_2,
  s.setpoint_3,
  r.device_id,
  r.status AS run_status,
  r.started_at_utc AS run_started_at_utc,
  p.plan_name,
  r.plan_version,
  r.battery_id,
  b.label AS battery_label,
  b.manufacturer AS battery_manufacturer,
  b.serial_number AS battery_serial_number,
  b.nominal_capacity_ah AS battery_nominal_capacity_ah,
  b.manufactured_on AS battery_manufactured_on
FROM ebc_measurement m
JOIN ebc_run_segment s
  ON s.run_id = m.run_id AND s.segment_number = m.segment_number
JOIN ebc_test_run r
  ON r.run_id = m.run_id
LEFT JOIN ebc_test_plan p
  ON p.plan_id = r.plan_id
LEFT JOIN ebc_battery b
  ON b.battery_id = r.battery_id;
