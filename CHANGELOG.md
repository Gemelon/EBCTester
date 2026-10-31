<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# Changelog

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
