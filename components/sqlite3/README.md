<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# SQLite for the SD card

This component builds the official SQLite 3.53.4 amalgamation from the
SQLite-published archive and verifies the archive's SHA3-256 before extraction.
The archive is kept under the build directory and is not committed.

Its small ESP-IDF VFS uses POSIX calls provided by ESP-IDF's FATFS VFS. Database
and rollback-journal files use real files, `fsync()` is not compiled out, and
FAT/SD atomic-write capabilities are not assumed. The application must serialize
all database use and hold the shared SD-card lock while the database is open.
Database file sizes are obtained with `fstat()` only. Never `lseek()` past EOF:
on a FATFS file opened for writing a seek beyond the end extends the file. Reads
beyond EOF therefore return zeros without seeking, and writes that start beyond
EOF first zero-fill the gap with `ftruncate()`.

SQLite heap allocations are routed to PSRAM via `sqlite_esp_configure_memory()`
(falls back to internal RAM if no PSRAM is present). Call it before
`sqlite3_initialize()`. The firmware enables the module's octal PSRAM
(`CONFIG_SPIRAM_MODE_OCT`) with `CONFIG_SPIRAM_IGNORE_NOTFOUND`.

The SQLite amalgamation and SQLite's reference VFS code are in the public
domain. See https://sqlite.org/copyright.html.
