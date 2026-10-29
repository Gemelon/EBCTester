<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# Development language

All original code, identifiers, comments, documentation, README files, commit
messages, issues, pull requests, and other GitHub content must be in English.
The language of the conversation does not change this rule. Ask the user for
explicit approval before changing the development language.

Preserve explicitly requested UI translation catalogs, localization tests, and
user-entered data in their intended languages. English is the base language and
fallback. Preserve third-party license and copyright texts verbatim.

## Release builds

By default, finishing firmware changes includes compiling and publishing a
testable release to the configured publication directory, unless the user
explicitly requests otherwise. A validation-only app build is not sufficient.

Increment the build number in `PROJECT_VER` in `CMakeLists.txt` exactly once
per new release, before its first compilation. Keep that number unchanged
for subsequent builds, fixes, and publication retries until publication
succeeds. Do not increment it for every build or failed attempt. Increment
again only when preparing the next release after successful publication.
Never overwrite an existing published version with different firmware bytes.

Run the full build including the configured publication target (normally
`idf.py build`), not just `idf.py app`. Check the actual process exit code
and publication output. Verify that the versioned BIN exists in the
publication directory, matches the built image, and that the manifest
references that version with the correct size and SHA256 before reporting
success. If compilation or publication fails, fix the cause and retry with
the same pending release number. Report unresolved failures explicitly.
Do not flash hardware or start battery tests automatically.

## MQTT / Home Assistant integration

Optional feature. Nice to have, never a dependency. The SD card database
remains the only source of truth; Home Assistant (HA) gets live values and
control only, never a copy of the database.

### Structure
- Implement as a separate, optional ESP-IDF component (own repository,
  like `ota-suite`) on top of `esp-mqtt`.
- EBCTester talks to it only through a few callbacks/interfaces. The MQTT
  component must not depend on EBCTester internals.
- Disabled by default. Settings: broker host, port, username, password,
  topic prefix, "allow remote start" (default off).

### Reliability
- MQTT must never block recording, test plans, the web UI, or SD access.
- Missing Wi-Fi or broker is a normal state: reconnect with backoff, no
  error spam, no effect on running tests.
- Never buffer measurements for MQTT. Missed live values are acceptable.

### Discovery
- Use HA device discovery: one retained config message at
  `homeassistant/device/<device_id>/config` with all components.
- `device_id` and every `unique_id` derive from the MAC address and must
  stay stable across firmware versions. Renaming must not create duplicates.
- Set `availability_topic` plus Last Will (`online`/`offline`, retained).
- Republish discovery when `homeassistant/status` becomes `online`.
- Set `configuration_url` in the device block to the device web UI.

### State
- Publish measurements only when the recorder stores an averaged
  measurement (same moment and same values as the database write). No own
  timer, no per-frame publishing, no extra averaging in the MQTT component.
- Event-like values (mode, plan status, connected, assigned battery) are
  published on change.
- One JSON state topic with a `value_template` per entity.
- Expose: voltage, current, capacity, mode, plan status, connected,
  assigned battery. After a test: capacity (Ah), energy, internal
  resistance at 1/5/10 s, timestamp. Use correct `device_class`,
  `state_class`, and units so HA builds long-term statistics.
- Set `expire_after` on measurement sensors (about 3x the recording
  interval) so HA shows "unavailable" instead of a stale value if the
  device stops sending.

### Commands
- Allowed: start plan, stop, abort, select plan, select battery.
- Do not expose free setpoint writing.
- Every command goes through the same state machine as the web UI.
  Conflicting starts are rejected. Stop and Abort are always accepted.
- Start commands are ignored unless "allow remote start" is enabled.
- Validate all payloads. Ignore malformed or unknown commands silently
  (log at debug level).

### Security and documentation
- MQTT is unencrypted by default. Document this, and recommend a dedicated
  broker user with restricted topic permissions.
- Never log or publish passwords.
- Document topics, entities, and settings in README and
  `docs/TECHNICAL_REFERENCE.md`.
- Provide an example HA dashboard (YAML) in the repo. Its only extra
  dependency may be `apexcharts-card`; note this in the file header.
- Charts of past runs stay in the device web UI. HA links to it via
  `configuration_url`.

### Tests
- Host tests for topic names, discovery JSON, and command validation.
- Never start a test or flash hardware as part of automated checks.
