<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# Changelog

## [1.4.68] – 2026-10-31

### Changed

- Move Refresh list one position left, before Apply filters, in test-run
  management. Button actions remain unchanged.

### Validation

- All 13 test-run management UI tests pass, including the toolbar order.
- Firmware build and publication succeeded; the downloaded image matches
  the built image's size and SHA256. Physical device testing is pending.
- Schema 3 and the backup limitations documented for 1.4.67 remain unchanged.

## [1.4.67] – 2026-10-31

### Added

- Add an Administration test-run management tab after device information,
  with name, battery, date, test plan and status columns.
- Combine battery, date and status filters with search and 50-run pagination.
- Rename or delete a single selected run with explicit confirmation.
  Active tests are protected; clearing a custom name restores its original
  default name. Custom names also appear in the measurement dashboard.
- Automatically migrate the SD database to schema 3 without changing run IDs
  or existing default names.

### Limitations

- Changes affect the local SD database only; existing MariaDB backups remain
  unchanged. Complete any desired backup before deleting a run, and do not
  delete runs during synchronization.
- Older firmware cannot open schema 3; retain a pre-update database backup
  if a downgrade might be needed.

### Validation

- All 91 UI tests and native SQLite tests pass, including migration rollback,
  active-run protection, filters, pagination and cascading deletion.
- Firmware was built and published; the downloaded image matches its size
  and SHA256. Physical device testing is pending.

## [1.4.66] – 2026-10-31

### Changed

- Check for firmware updates once when opening the web UI, without a
  settings-password prompt. Wait for network/HTTPS readiness and reuse checks
  already in progress. Failed requests remain visible for manual retry.
- Update installation remains manual; opening the UI never installs firmware.

### Validation

- All 70 web-interface tests pass, including startup checks, readiness guards,
  error handling and manual-only OTA installation. Device testing is pending.

## [1.4.65] – 2026-10-31

### Fixed

- Accept the EBC-A20's folded status checksums (`0xF0`–`0xFF` transmitted as
  `0x00`–`0x0F`), preventing false connection loss and interrupted recordings
  at affected measurement values. Invalid checksums remain rejected.
- Read raw marker-valued checksums without prematurely ending or restarting
  a UART status frame.

### Validation

- Host tests cover all 65,536 checksum combinations, fragmented UART streams,
  connection continuity and genuine connection timeouts. Physical battery
  tests were not run automatically.
- Manually tested on an EBC-A20: no further test interruptions and no
  checksum warnings in the device log.

## [1.4.64] – 2026-10-31

### Changed

- Display the manual charge-voltage setting with three decimal places.

## [1.4.63] – 2026-10-31

### Added

- Detect ZKETECH EBC/EBD models using the status-frame identifier and firmware
  version.
- Add EBC-A50L selection and distinguish it from EBC-A40L, which shares its
  protocol identifier.

### Changed

- Keep unknown and disabled models visible as read-only telemetry; block their
  control commands.
- Validate status-frame checksums before parsing.

### Compatibility

- Preserve the legacy A40L selection value `0x10`; its protocol identifier is
  decimal 10 (`0x0A`).
