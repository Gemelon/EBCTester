-- SPDX-FileCopyrightText: 2026 Thomas Stoll
-- SPDX-License-Identifier: Apache-2.0

ALTER TABLE ebc_test_run ADD COLUMN display_name TEXT
  CHECK (display_name IS NULL OR
    (length(display_name) BETWEEN 1 AND 100 AND length(CAST(display_name AS BLOB)) <= 400));
UPDATE ebc_schema_metadata SET schema_version = 3 WHERE singleton = 1;
