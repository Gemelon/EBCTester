<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# Changelog

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
