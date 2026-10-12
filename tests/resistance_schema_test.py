# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

import re
import sqlite3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sql_after(file, marker, terminator):
    source = (ROOT / file).read_text(encoding="utf-8")
    start = source.index(marker)
    end = source.index(terminator, start)
    return "".join(re.findall(r'"([^"]*)"', source[start:end]))


class ResistanceSchemaTests(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.db.execute("PRAGMA foreign_keys=ON")
        self.db.executescript((ROOT / "components/battery_database/schema.sql").read_text())
        self.db.execute(
            "INSERT INTO ebc_battery(battery_id,label,created_at_utc) VALUES(7,'Testzelle','2026-10-12')"
        )
        self.db.execute(
            "INSERT INTO ebc_test_run(run_id,device_id,battery_id,started_at_utc,status,notes) "
            "VALUES('test','EBC-A20',7,'2026-10-12 04:00:00','RUNNING','DCIR v1; phase=starting;')"
        )
        self.db.commit()
        self.migration = (ROOT / "components/battery_database/resistance_schema.sql").read_text()
        self.db.executescript(self.migration)
        self.insert = sql_after(
            "components/test_store/test_store.cpp",
            '"INSERT INTO ebc_resistance_result (',
            "-1, &statement",
        )

    def tearDown(self):
        self.db.close()

    def row(self, number=1, valid=True, captured=True, utc="2026-10-12 04:01:02.123"):
        return (
            "test", number, utc if captured else None, 123456 if captured else None,
            float(number), float(number) if captured else None, 3.3, 0.0,
            3.25 if captured else None, 5.0 if captured else None,
            10.0 if valid else None, 1.0, int(valid), None if valid else "Missing / invalid sample",
        )

    def test_migration_preserves_battery_run_and_is_repeatable(self):
        self.db.executescript(self.migration)
        self.assertEqual(self.db.execute("SELECT schema_version FROM ebc_schema_metadata").fetchone()[0], 2)
        self.assertEqual(self.db.execute("SELECT battery_id FROM ebc_test_run").fetchone()[0], 7)

    def test_three_results_battery_and_millisecond_utc(self):
        for i in range(1, 4):
            self.db.execute(self.insert, self.row(i))
        rows = self.db.execute(
            "SELECT battery_id,battery_label,captured_at_utc,resistance_mohm "
            "FROM ebc_resistance_result_view ORDER BY result_number"
        ).fetchall()
        self.assertEqual(len(rows), 3)
        self.assertTrue(all(row == (7, "Testzelle", "2026-10-12 04:01:02.123", 10.0) for row in rows))

    def test_foreign_key_rejects_unknown_run(self):
        row = list(self.row())
        row[0] = "unknown"
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute(self.insert, row)

    def test_missing_clock_and_missing_sample_are_null_not_zero(self):
        self.db.execute(self.insert, self.row(1, utc=None))
        self.db.execute(self.insert, self.row(2, valid=False, captured=False))
        rows = self.db.execute(
            "SELECT captured_at_utc,device_uptime_ms,resistance_mohm,valid FROM ebc_resistance_result ORDER BY result_number"
        ).fetchall()
        self.assertEqual(rows, [(None, 123456, 10.0, 1), (None, None, None, 0)])

    def test_constraints_cascade_and_atomicity(self):
        with self.assertRaises(sqlite3.IntegrityError):
            self.db.execute(self.insert, self.row(4))
        self.db.rollback()
        self.db.execute(self.insert, self.row())
        self.db.execute("DELETE FROM ebc_test_run WHERE run_id='test'")
        self.assertEqual(self.db.execute("SELECT COUNT(*) FROM ebc_resistance_result").fetchone()[0], 0)
        self.db.rollback()
        with self.assertRaises(sqlite3.IntegrityError):
            with self.db:
                self.db.execute(self.insert, self.row())
                self.db.execute(self.insert, self.row())
        self.assertEqual(self.db.execute("SELECT COUNT(*) FROM ebc_resistance_result").fetchone()[0], 0)

    def test_sync_signature_notices_results_and_query_transfers_all_columns(self):
        sync = ROOT / "components/mariadb_sync/mariadb_sync.cpp"
        query = sql_after(
            str(sync.relative_to(ROOT)), '"SELECT r.run_id,"', ";"
        )
        self.db.execute(
            "CREATE TABLE ebc_sync_state(run_id TEXT PRIMARY KEY,synced_sequence INTEGER,"
            "synced_segment INTEGER,run_signature TEXT)"
        )
        before = self.db.execute(query).fetchone()[4]
        self.db.execute(self.insert, self.row())
        after = self.db.execute(query).fetchone()[4]
        self.assertNotEqual(before, after)
        transfer = sql_after(
            str(sync.relative_to(ROOT)), '"SELECT run_id, result_number, captured_at_utc', "-1, &statement"
        )
        values = self.db.execute(transfer, ("test",)).fetchone()
        self.assertEqual(len(values), 14)
        self.assertEqual(values[2], "2026-10-12 04:01:02.123")
        self.assertEqual(values[10], 10)


if __name__ == "__main__":
    unittest.main()
