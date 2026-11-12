<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# Changelog

## [2.0.9] - 2026-11-12

### Fixed

- Retry failed dashboard run-list requests every 15 seconds while the page is
  visible, and clear the error once the SD card database becomes available.
- Successful manual refreshes also clear stale errors and cancel pending retries;
  empty databases show the normal empty-list message instead.

### Validation

- All 25 dashboard and run-management host tests passed. Full ESP-IDF build and
  local publication succeeded; the versioned image matches the built firmware
  and the manifest's version, size and SHA256.
- No hardware test, flashing or public OTA upload was performed.

## [2.0.8] - 2026-11-12

### Changed

- Add configurable SD card-detect polarity in menuconfig and Maker Settings.
  New/reset configurations default to active low; existing saved configurations
  retain active-high behavior until changed.
- Keep GPIO `-1` available to bypass card detection in both settings interfaces.

### Validation

- Full ESP-IDF build and local package verification succeeded, including image
  size, SHA256 and manifest checks. Host tests passed for both polarities,
  insertion/removal debounce, configuration migration and Maker Settings.
- No hardware test, flashing or public OTA upload was performed.

## [2.0.7] – 2026-11-11

### Fixed

- Keep SQLite scratch buffers on the configured heap/PSRAM instead of the
  recorder task stack, preventing stack overflows and device restarts while
  recording measurements.

### Validation

- Full ESP-IDF build and local package verification succeeded. Host tests
  verified heap-backed scratch buffers, 5,000 indexed writes, database integrity,
  and existing run-management operations. No hardware test or local upload was run.

## [2.0.6] – 2026-11-11

### Changed

- Disable manual MariaDB connection tests and synchronization while automatic
  backup is enabled, and reject those requests through the API as well.

### Validation

- Full ESP-IDF build and SFTP publication succeeded. The published
  `EBCTester-2.0.6.bin` is 2,884,512 bytes with SHA256
  `3fdea39f02783b2e5dec1c52d17d81063037051149371b47c4b51d5b829efa58`.

## [2.0.5] – 2026-11-11

### Fixed

- Configure clean firmware builds to check the production OTA manifest at
  `https://ebctester.gemelon.de/firmware.json`.

## [2.0.4] – 2026-11-11

### Fixed

- Keep MariaDB settings usable after failed administrator confirmation and
  refresh the displayed configuration after saving or declining migration.

### Validation

- Full ESP-IDF build and SFTP publication succeeded. The published
  `EBCTester-2.0.4.bin` is 2,883,152 bytes with SHA256
  `66e73c9de500f5ce69ec45c3f29c964ed9a8238b3d4ed86bcd28dad130742bf9`.

## [2.0.3] – 2026-11-11

### Fixed

- Retry the non-blocking startup schema check when network connectivity becomes
  available, including installations with saved MariaDB settings.

### Validation

- Full ESP-IDF build and SFTP publication succeeded. The published
  `EBCTester-2.0.3.bin` is 2,882,976 bytes with SHA256
  `bacd82946c8b5296e9b1b5670c70f9c8807c9f86e0d0c4f0d668576ec9060b33`.

## [2.0.2] – 2026-11-11

### Fixed

- Make schema setup compatible with existing MariaDB identifier types while
  adding missing non-key fields and unique indexes without deleting data.
- Declining schema setup now disables both automatic and manual MariaDB
  operations until the integration is re-enabled in settings.

### Validation

- Full ESP-IDF build and SFTP publication succeeded. The published
  `EBCTester-2.0.2.bin` is 2,882,832 bytes with SHA256
  `d5d7c3e5930b99b6727c16396dd97320d845c45e9353482eb7eb11f695d04f4e`.

### Limitations

- Automatic setup does not add foreign keys; incompatible existing key fields
  still require manual repair after a database backup.

## [2.0.1] – 2026-11-11

### Added

- Check the required MariaDB backup schema during startup without blocking
  device services. Offer one-time creation and repair from MariaDB settings
  using separate administrator credentials that are not saved on the device.

### Fixed

- MariaDB initialization failures and declined schema setup no longer stop
  startup or affect testing, recording, or the web interface.

### Validation

- Full ESP-IDF build and SFTP publication succeeded. The published
  `EBCTester-2.0.1.bin` is 2,883,344 bytes with SHA256
  `3e52c067e73cca4d8e6ec89322d95b85741518ff47d54223d3f172c1c59388b0`.

### Limitations

- Incompatible existing key fields or field types are not rewritten
  automatically; they require manual repair after a database backup.

## [2.0.0] – 2026-11-09

### Changed

- Promote EBCTester V2 to the stable main release line.
- Update firmware versioning baseline from `1.99.21` to `2.0.0` for the first
  production V2 release.

### Fixed

- Include the confirmed startup stability fixes from the final V2 pre-release
  cycle (configuration migration stack usage, DS3231 default handling and
  persisted Maker-mode RTC settings).

### Validation

- V2 branch runtime validation completed over an extended period without new
  instability reports.
- Host and web regression tests from the 1.99.21 release line remain passing
  for this promotion release.

## [1.99.21] – 2026-11-08

### Fixed

- Fix the confirmed main-task stack overflow during v1 configuration migration,
  which prevented startup and triggered OTA rollback to 1.99.17.
- Move configuration read/write blobs and save candidates to the heap and free
  the read buffer before writing migrated settings. Keep NVS values and RTC
  mode behavior unchanged; report allocation failures without resetting NVS.

### Validation

- Native migration, configuration, Maker API and RTC tests passed, including
  allocation failures, cleared-buffer cleanup and NVS-handle cleanup.
- Added a regression check of actual ESP32-S3 stack frames. Previous read/write
  frames were 2,032 bytes each; the new initialization frame is 96 bytes and
  persistence frame is 64 bytes. All configuration frames are at most 128 bytes,
  below the enforced 256-byte limit; the main-task stack remains 3,584 bytes.
- Full ESP-IDF build and SFTP publication succeeded. Local and downloaded BIN
  and manifest match at 2,859,360 bytes with SHA256
  `9d9cc4800f0f0dee045bf1477e311ad1ebd9f644041abb062bf76e30d3874815`.

### Limitations

- Startup on the user's device still requires confirmation; automated checks
  never flash hardware. Physical RTC and backup-cell tests remain pending.

## [1.99.20] – 2026-11-08

### Fixed

- Enforce the onboard DS3231
  enable state and original-board RTC pins (INT/SDA/SCL = GPIO14/GPIO15/GPIO16)
  whenever configurations are loaded, migrated, or saved.
- Keep MakerMode behavior unchanged: RTC defaults to off, can be enabled in
  Maker Settings, and persists user selection.

### Validation

- Native configuration tests passed for FactoryMode enforcement, version-1/2
  migration, persisted saves, MakerMode selections and pin validation.
- RTC fallback and Maker/API web tests passed.
- Full ESP-IDF build and SFTP publication succeeded. Local and downloaded BIN
  and manifest match at 2,859,104 bytes with SHA256
  `4c785e6bed980d96ecc8749ab8b0ea90987f947dd27fc25c831204961c5abf87`.

### Limitations

- Missing RTC hardware is logged and does not block startup; NTP remains the
  fallback. Without RTC and NTP, calendar time remains unavailable.
- Physical RTC and backup-cell validation remain pending.
- The attempted pin correction did not fix the startup failure; the main-task
  stack overflow was subsequently diagnosed and addressed in 1.99.21.

## [1.99.19] – 2026-11-08

### Fixed

- Always enable the onboard DS3231 in FactoryMode, including existing saved
  configurations and upgrades. Normal users need no Maker Settings access.
- Keep RTC off by default in MakerMode while preserving the user's saved
  selection and allowing activation through Maker Settings.

### Validation

- Native configuration tests passed for initial setup, v1 migration, existing
  v2 FactoryMode settings, enforced saves, GPIO conflicts and Maker selections.
- RTC missing-hardware fallback, Maker API and targeted clock UI tests passed.
- Full ESP-IDF build and SFTP publication succeeded. Local and downloaded BIN
  and manifest match at 2,859,024 bytes with SHA256
  `1a5bf24bf63bad71262d4cf48ea7b87759de41984a1d44e7684d5eb01c1fd913`.

### Limitations

- Physical RTC and backup-cell validation remain pending. Missing RTC hardware
  is logged and falls back to NTP; without either source, calendar time remains
  unavailable and recording uses the existing uptime behavior.
- Inactive configurations still disable all application peripherals.

## [1.99.18] – 2026-11-08

### Added

- Optional DS3231 RTC in Maker Settings, disabled by default, with configurable
  INT/SDA/SCL pins (original-board defaults: GPIO14/GPIO15/GPIO16).
- Startup and six-hour UTC synchronization with NTP priority and offline RTC
  fallback. Invalid calendar/oscillator-stop time is rejected; missing RTC
  hardware does not block normal device services.
- Header clock beside EBC RX, with time above date, using the browser's detected
  time zone and daylight-saving rules.

### Changed

- Preserve existing configuration during migration to blob version 2; RTC stays
  disabled on upgrade. RTC-disabled devices keep their existing NTP behavior.

### Validation

- Native RTC, configuration/migration and Maker API tests passed.
- Targeted web tests and exact Berlin/UTC+8 date-rollover and DST tests passed.
- Full ESP-IDF build and SFTP publication succeeded. Local and downloaded BIN
  and manifest match at 2,858,672 bytes with SHA256
  `24ccc1a140964eb5cbc2bc23923d8ed1a4736266a38733227050a92ed902d44e`.

### Limitations

- Physical RTC operation, backup-cell retention and six-hour hardware tests
  remain pending. RTC INT is reserved; alarms and wakeup are not implemented.
- Existing unrelated checks still fail: the SD language template lacks LED
  catalog entries, and the license metadata test reads binary files as UTF-8.

## [1.99.17] – 2026-11-07

### Changed

- Tester-mode LED color for **automatic plan running** changed from red to
  orange to reduce confusion with error indications.

### Validation

- LED policy and LED settings tests passed.
- Full ESP-IDF build and SFTP publication succeeded.
- Published image and manifest match the build at 2,839,280 bytes with SHA256
  `f0b44a40e0292e02ede52801cc1109ba4efe58e434ca7a699220e658fc897685`.

### Limitations

- Physical LED output on hardware has not been revalidated yet.

## [1.99.16] – 2026-11-07

### Fixed

- Rename the LED web-extension ID from `led_settings` to `led-settings`.
  The underscore violated web-server registration rules and caused
  `ESP_ERR_INVALID_ARG` at startup, aborting 1.99.15 and triggering OTA rollback.
- Test extension-ID validation in addition to descriptor lifetime. Keep the
  existing rollback protection enabled.

### Validation

- LED host tests and all six LED UI tests passed.
- Full ESP-IDF build and SFTP publication succeeded. BIN and manifest match
  the build at 2,839,216 bytes with SHA256
  `c54cc2cb293136e92ec618fcbc6911bc7f65c853aef3129318549f5e00160785`.

### Limitations

- Startup on the physical device still requires verification. Do not install
  1.99.14 or 1.99.15; use 1.99.16 or later.

## [1.99.15] – 2026-11-07

### Fixed

- Keep the LED settings web-extension descriptor alive for the entire web-server
  lifetime. Version 1.99.14 registered a stack-local descriptor, leaving an
  invalid pointer at server startup that could cause a crash and OTA rollback.
- Retain the existing OTA startup confirmation and rollback protection.

### Validation

- LED policy, driver, settings-handler and six LED UI regression tests passed,
  including deferred access to the registered extension after initialization.
- Full ESP-IDF 6.0.1 build and SFTP publication succeeded. Versioned BIN, build
  and manifest match at 2,839,216 bytes with SHA256
  `0fe9668b2d7de21858362a7bb69c0ea1e30dc8fff5a6e70cce50874b1cf7bce3`.

### Limitations

- Superseded by 1.99.16: the invalid LED extension ID prevents startup.
- Device startup and physical LED output have not been verified on hardware.

## [1.99.14] – 2026-11-07

### Added

- Configurable WS2812 LED function groups in normal Settings for Maker and
  Factory firmware, supporting two board LEDs and extensions up to 16 LEDs.
- Independent Tester mode and Message frame status displays, with optional
  OTA priority per LED. Defaults: LED1 = Tester mode + OTA; LED2 = Message
  frame status. Changes apply immediately and persist across restarts.
- English/German LED settings and host tests for colors, message flashes,
  communication errors, silence timeout, OTA priority, and saved assignments.

### Validation

- Full ESP-IDF 6.0.1 build and SFTP publication succeeded.
- Versioned BIN, build, and manifest match at 2,839,200 bytes with SHA256
  `839c6cc336249845324596d65d840450758eb1b28d5b2295a1c0883ff2c10301`.
- LED policy/driver/settings and UART/controller host tests passed;
  all 62 selected LED UI and web-interface tests passed.

### Limitations

- Superseded by 1.99.15: a dangling LED web-extension descriptor can cause a
  startup crash and OTA rollback. Do not install 1.99.14.
- Physical LED output has not been tested on hardware. The extension must
  continue the board's WS2812 data chain and have a suitable power supply.

## [1.99.13] – 2026-11-07

### Fixed

- Avoid recording a transition frame as a one-sample segment when an automatic
  plan switches to the next active step. Recording now starts a new segment only
  after the EBC reports the expected active mode for that step.

### Limitations

- No known limitations.

### Validation

- ESP-IDF 6.0.1 full build and SFTP publication succeeded for version
  `1.99.13`.
- Published image and manifest match the build at 2,827,104 bytes with SHA256
  `36a0196011cbf50e9fcb20478703f0acad428e105e69069e85285ab0c9ec3140`.
- `python tests/ebc_link_test.py` passed.
- `node tests/battery.test.js` passed.

## [1.99.12] – 2026-11-06

### Fixed

- Allow saving a new battery to the SD database when `previousLabel` is absent.
  The web interface now sends that field only for a battery known to be saved,
  and the handler skips the rename step when no previous label is provided.

### Limitations

- No known limitations.

### Validation

- ESP-IDF 6.0.1 full build and SFTP publication succeeded for version
  `1.99.12`.
- Published image and manifest match the build at 2,827,024 bytes with SHA256
  `493d281a4027409ede64b95b6dd350c219471c66831a5aec15e664c68b9d2d13`.
- `node tests/battery.test.js` passed.

## [1.99.11] – 2026-11-06

### Changed

- Allow renaming a saved battery by loading it, editing the label, and using
  **Apply and save to database**. The firmware now renames the existing
  `ebc_battery` row so linked test runs keep their battery association.

### Limitations

- No known limitations.

### Validation

- ESP-IDF 6.0.1 full build and SFTP publication succeeded for version
  `1.99.11`.
- Published image and manifest match the build at 2,826,992 bytes with SHA256
  `adfff3e8efcd4dc3966b071fde24cc31444d19444af15e748b1b389a6cdbd82e`.
- `node tests/battery.test.js` passed.

## [1.99.8] – 2026-11-05

### Fixed

- Keep both SDMMC and SPI defaults available in Maker Settings, regardless of
  the selected SD bus, so switching bus modes does not replace the other mode's
  settings with `-1`.

### Limitations

- No known limitations.

### Validation

- ESP-IDF 6.0.1 full build and SFTP publication succeeded. The published image
  matches the build at 2,825,440 bytes with SHA256
  `0868b991097d07054b580eeaa1ab0ebf3e1f5b0b0a7aadbc99d1516db700fc24`.
- Host configuration tests passed for both SD bus modes.

## [1.99.3] – 2026-11-05

### Changed

- Introduce centralized NVS-based startup configuration in a new `ebc_config`
  component. First boot seeds defaults from `sdkconfig`, validates and stores a
  versioned CRC-protected blob, and keeps Factory/Maker mode plus `ConfigAktive`
  in dedicated keys.
- Add GPIO47 bootstrap detection (pull-up/pull-down sampling) with logged mode
  selection and strict fallback to MakerMode on invalid combinations.
- Add Maker-only configuration workflow with a new **Maker Settings** tab and
  protected reset actions in **Settings**:
  - reset only `ebc_cfg` and restart
  - erase full NVS (including Wi-Fi credentials) and restart
- Move runtime settings from direct `CONFIG_*` usage to NVS-fed runtime values
  across tester UART, averaging, device ID, OTA/network/web, SD bus/pins,
  localization path, and MQTT activation.
- Enforce inactive Maker behavior (`ConfigAktive = 0`): no tester/dongle UART,
  SD, status LED, station Wi-Fi/SNTP/update-check/MQTT, or recorder startup.
  Only AP + web remain available for setup.

### Limitations

- Hardware acceptance criteria (IO47 wiring variants and on-device GPIO inactivity)
  still require manual board verification after flashing.

### Validation

- ESP-IDF 6.0.1 full build and SFTP publication succeeded for version `1.99.3`.
- Published image and manifest match the build at 2,825,440 bytes with SHA256
  `7867245711e3dae7292e1f250c6a947edbabd2879e34296b10f5178713fad3d4`.
- Host tests passed for NVS/IO47 config core, Maker configuration backend, EBC
  UART link/controller, authentication, web UI flows, and MQTT discovery.

## [1.99.2] – 2026-11-05

### Changed

- Intermediate beta publication during staged refactoring. Superseded by `1.99.3`.

## [1.99.1] – 2026-11-04

### Changed

- First V2 beta build, identical to 1.4.72 in functionality. It is published to
  the separate beta channel `https://ebctester.gemelon.de/beta/` and checks
  `/beta/firmware.json` for updates. Devices on the stable channel never see it.
- Known limitation: a device must be flashed once over USB to switch channels.

## [1.4.72] – 2026-11-02

### Changed

- Replace Login duration with Login Settings: change the web password, keep
  daily/weekly sessions, or disable browser login independently of settings
  password confirmation.
- Default the initial web password to `ebctester` through
  `CONFIG_OTA_SUITE_WEB_PASSWORD`. Saved passwords survive restarts and updates;
  the setup AP password is unchanged.
- Persist login settings together and invalidate sessions after password or
  login-policy changes. Settings saves still require the current login password
  when browser login is disabled.

### Limitations

- Existing devices without a saved web password use `ebctester` after updating;
  change this shared default on first use. Disabling login opens data and
  controls to network users. HTTP is unencrypted; physical device testing is pending.

### Validation

- 94 host UI/localization tests and both C authentication configurations passed.
  Browser checks verified password confirmation with login disabled.
- ESP-IDF 6.0.1 full build and SFTP publication succeeded. The public image and
  manifest match the build at 2,739,408 bytes and SHA256
  `d1e7d5b055e2035a6603369126d64ef31d36c43a9e2eb7fafa524f2cd0a1d84f`.

## [1.4.71] – 2026-11-01

### Included

- Publish the current main-branch firmware for GitHub, including the
  improvements added since 1.4.65: startup update checks, test-run management,
  schema 3 migration, and protected battery deletion.
- Firmware behavior is unchanged from 1.4.70; this release advances the version
  for a distinct, immutable release image.

### Limitations

- Schema 3 databases cannot be opened by older firmware; retain a pre-update
  backup if a downgrade may be needed. Physical device testing is pending.

### Validation

- ESP-IDF 6.0.1 full build and SFTP publication succeeded. The public image
  matches the local image at 2,734,352 bytes and SHA256
  `e1267cb4268d54f8c9dfb8fbed6a7f67b0a50d2636535e5d7f21f9ca9de24aea`.

## [1.4.70] – 2026-11-01

### Changed

- Publish the merged main-branch firmware under a new version to avoid
  overwriting the existing 1.4.69 release with different firmware bytes.
- Test-run management and protected battery deletion remain unchanged.

### Validation

- Full firmware build and SFTP publication succeeded. The downloaded image
  matches the built image and published manifest in size and SHA256.
- Physical device testing is pending. Schema 3 backup and downgrade
  limitations documented for 1.4.67 remain unchanged.

## [1.4.69] – 2026-10-31

### Added

- Add Delete battery immediately after New battery. Delete the saved battery
  currently displayed in the form after confirmation and settings authorization.
- Protect active recordings, pending starts, running plans and DCIR tests.
  Batteries referenced by test runs cannot be deleted; remove those runs in
  test-run management first.
- Clear the current battery selection after deletion and refresh saved batteries.
  Existing MariaDB backups remain unchanged; no database migration is needed.

### Validation

- Battery UI, test-run management and localization tests pass (20 tests).
  Native SQLite tests verify referenced-battery protection, literal bound labels,
  missing batteries and successful deletion without removing test runs.
- Firmware build and publication succeeded; the downloaded image matches the
  built image's size and SHA256. Physical device testing is pending.
- Schema 3 backup and downgrade limitations documented for 1.4.67 remain unchanged.

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
