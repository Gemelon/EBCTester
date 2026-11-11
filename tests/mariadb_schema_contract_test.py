# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCHEMA = (ROOT / "components/mariadb_sync/mariadb_schema.cpp").read_text(encoding="utf-8")
SYNC = (ROOT / "components/mariadb_sync/mariadb_sync.cpp").read_text(encoding="utf-8")


class MariaDbSchemaContractTest(unittest.TestCase):
    def test_created_tables_contain_every_validated_column(self):
        declarations = {
            f"{symbol}_COLUMNS": set(re.findall(r'\{"([a-z0-9_]+)", ColumnKind::', body))
            for symbol, body in re.findall(
                r"const Column (\w+)_COLUMNS\[\] = \{(.*?)\n\};", SCHEMA, re.S
            )
        }
        table_entries = re.findall(
            r'\{"(ebc_[a-z_]+)", (\w+_COLUMNS), sizeof\(', SCHEMA
        )
        create_block = SCHEMA.split("const char *CREATE_TABLES[] = {", 1)[1].split("};", 1)[0]
        sql = "".join(re.findall(r'"((?:\\.|[^"\\])*)"', create_block))
        for table, symbol in table_entries:
            match = re.search(
                rf"CREATE TABLE IF NOT EXISTS {table} \((.*?)\) ENGINE=InnoDB", sql
            )
            self.assertIsNotNone(match, table)
            definition = match.group(1)
            for column in declarations[symbol]:
                self.assertRegex(definition, rf"(?:^|,)\s*{column}\s+", f"{table}.{column}")

    def test_all_remote_insert_tables_are_in_the_schema(self):
        schema_tables = set(
            re.findall(r'\{"(ebc_[a-z_]+)", \w+_COLUMNS,\s*sizeof\(', SCHEMA)
        )
        inserted_tables = set(re.findall(r"INSERT(?: IGNORE)? INTO (ebc_[a-z_]+)", SYNC))
        inserted_tables.discard("ebc_sync_state")
        self.assertEqual(inserted_tables, schema_tables)

    def test_migration_does_not_drop_or_truncate_existing_data(self):
        self.assertNotRegex(SCHEMA, r"\b(?:DROP|TRUNCATE)\s+(?:TABLE|INDEX)\b")


if __name__ == "__main__":
    unittest.main()
