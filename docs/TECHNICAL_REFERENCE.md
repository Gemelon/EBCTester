<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# EBCTester technical reference

Implementation details for firmware 1.3.36. Commands and file paths below
are relative to the project root. For purpose, setup, safety and licensing,
see [README](../README.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).

## UI languages and SD template

The header language selector supports built-in **English** and **Deutsch**.
English is the fallback for every missing translation. On the first visit,
a German browser selects German; other browser languages select English.
The selection is saved per browser, not in device NVS. Switching language
asks for confirmation and reloads the page: unsaved form edits are lost, but
the firmware's running test/plan is not stopped.

All authored tabs, forms, chart labels, confirmations and status/error messages
use the same catalog, including lazily loaded tabs. Backend protocol messages
are emitted in English and translated at their UI display points; old German
protocol messages remain recognized. User-entered battery labels, plan names,
manufacturers, notes and filenames are never translated. Raw stored notes and
third-party diagnostic details remain as recorded.

The complete, ready-to-copy template is
[`sd_card/languages/en-custom.json`](../sd_card/languages/en-custom.json).
Copy it to **`/languages/en-custom.json` at the SD card root** (the ESP mount
path is normally `/sdcard/languages/en-custom.json`). Alternatively, create
the `languages` folder and upload the file using **Administration → SD card**.
Then reload the page or use **Settings → Language → Reload languages**.
It appears as **English (SD template) (SD)** in the selector.

To add a language:

1. Copy the template, for example as `fr.json`.
2. Set `"code": "fr"` and `"name": "Français"`.
3. Translate only values in `"strings"`; keep all keys unchanged.
4. Preserve placeholders such as `{p0}`, `{p1}`, `%u` and `%s`.
   Named placeholders may be reordered; printf placeholders must retain order
   and types. They carry measurement values and diagnostic parameters.
5. Save as UTF-8 without BOM, upload to `/languages/fr.json`, then reload.

Partial files are supported: omitted keys fall back to built-in English,
not to the previously selected language. Files named `en.json` or `de.json`
with matching codes replace that built-in language's texts while present on
the card. Additional codes create additional choices. Remove a file and reload
to restore the built-in language. No firmware rebuild is needed for SD edits.
Unknown keys are ignored for forward compatibility.

A Filipino (Tagalog) SD test catalog is provided as
[`sd_card/languages/fil.json`](../sd_card/languages/fil.json). Upload it as
`/languages/fil.json`, reload languages and select **Filipino (Tagalog) (SD)**.
It translates navigation, common forms, measurement labels and SD operations.
This is a partial catalog: untranslated detailed messages remain in English,
also exercising the per-key fallback. For a quick SD-loading check, the
language label becomes **Wika**, the battery tab **Baterya**, and the primary
section **Tagasubok ng baterya**. Filipino is not embedded in the firmware.

Limits: at most **16 JSON files**, **128 KiB per file**, **1024 string entries**,
**4096 UTF-8 bytes per value**, and **80 bytes for the display name**.
Codes use lowercase ASCII letters, digits and hyphens, start with a letter,
do not end with a hyphen and have at most 24 characters. Use normal language
tags (`fr`, `es`, `en-gb`) for appropriate date/number formatting.
Bad JSON, wrong metadata, non-string values, unavailable selected files and
placeholder mismatches are shown as warnings; an unusable selected language
falls back to English. Translations are assigned using `textContent` and
safe attribute setters, never inserted as HTML or executed as scripts.

English/German resources and the browser runtime are bundled in the shared
`ota-suite/components/web_interface` component. This project enables
`CONFIG_OTA_SUITE_WEB_SD_LANGUAGES=y` and registers `ui_localization` before
starting the server. The shared web component also works standalone with
that option disabled. SD language reads hold the same storage lock as SQLite;
no card is formatted and the database schema is unchanged.

Validation: `node tests\ui_languages_test.js` checks catalog coverage, English
fallback, German, partial overrides, malformed values, placeholders, safe text,
persistence and the standalone shared UI. The optional local browser fixture
`python tests\ui_language_fixture.py` serves only `127.0.0.1:8766` and never
connects to the tester, SD card or MariaDB.

## Home Assistant MQTT commands

The optional MQTT integration subscribes to `<topic_prefix>/command` only
when an application command callback is registered. It accepts exactly one
JSON object using one of these forms:

```json
{"command":"start_plan"}
{"command":"stop"}
{"command":"abort"}
{"command":"select_plan","name":"Plan name"}
{"command":"select_battery","name":"Battery label"}
```

Plan and battery selection loads an existing entry from the SD database and
applies it through the same test-control functions used by the web interface.
Starting the selected plan is ignored unless **Allow remote start commands**
is enabled in **Settings → Home Assistant MQTT** in the device web interface;
the option defaults to off. Stop and Abort are not
gated by that option. MQTT cannot write free setpoints or create database
entries. Invalid, unknown, fragmented or oversized command messages are
ignored. Commands are dispatched on a separate worker task so database
selection does not block the MQTT event task. If its bounded queue fills,
Stop and Abort take priority over queued commands.

Use a dedicated broker account with permissions restricted to the device's
command and state topics. MQTT uses unencrypted TCP by default.

## Debug log over Wi-Fi

The `debug_log` component mirrors the ESP-IDF log output into a ring buffer
of **500 lines** (159 characters each) and exposes it in a **Debug** tab of
the web interface, so a running device can be observed without a USB cable.

- The hook is installed as the first statement of `app_main()`, forwards every
  message to the console unchanged, and then copies it into the buffer. Because
  the log framework emits a message in several chunks, lines are assembled and
  committed on each newline; ANSI colour codes are removed.
- The buffer lives in PSRAM and is protected by a spinlock. The hook never
  allocates, never waits and never calls back into the logging system, so it
  cannot delay recording, test plans, the web UI or SD access. Capturing holds
  the lowest priority of all buffer users: the hook tries the spinlock once and
  discards the chunk when a reader or a clear request holds it instead of
  spinning. Discarded chunks are counted and published as `dropped` by the
  status and log endpoints, and the Debug tab shows the count. When a line is
  longer than the limit it is truncated, and the oldest line is overwritten
  once the buffer is full.
- Capturing is **disabled by default**. The setting lives in NVS namespace
  `debug_log`, key `enabled`, and is applied right after `device_app_start()`.
  Until then capturing is active, so start-up messages are available when the
  feature is enabled. Disabling releases the buffer (about 80 KB).
- Bootloader output and anything logged before `app_main()` — as well as plain
  `printf()` output — is only visible on the USB console.

| Endpoint | Method | Purpose |
| --- | --- | --- |
| `/api/debug/status` | GET | Buffer cursor and the persisted enable flag |
| `/api/debug/settings` | POST | `{"enabled":true\|false}`, saves and applies |
| `/api/debug/log?after=<seq>&max=<n>` | GET | Up to 120 lines newer than `seq` |
| `/api/debug/clear` | POST | Drops buffered lines, returns the new `nextSeq` |

All endpoints require an authenticated session; the POST routes additionally
require the CSRF token. The browser polls once per second and detects buffer
overruns from the reported `firstSeq`. The Debug tab is hidden while the
feature is disabled: `web_interface_extension_t` gained an optional `visible`
callback that removes an extension from `/api/status`, and `app.js` removes
tabs that are no longer reported.

Log lines can contain network names, file names and settings, but never
passwords. Only logged-in users can read them.

## Setup and migration

Use the ESP-IDF VS Code extension, target **esp32s3**. The starter package's
components are imported through `EXTRA_COMPONENT_DIRS`. On first reconfigure,
the project maps saved BatteryTester settings to their OTA Suite symbols,
including the configured AP password, SNTP server, manifest URL, HTTP opt-in,
and LED GPIO. Station credentials and a generated AP password remain in the
same `network` NVS namespace and are not erased. New builds keep the
`BatteryTester-XXXXXX` setup AP name and enable the onboard LED on GPIO 48.

In menuconfig:

- **EBCTester → Home Assistant MQTT → Enable MQTT component** enables MQTT.
  Broker settings and remote-start permission are configured on the device
  under **Settings → Home Assistant MQTT** after the firmware is installed.
- UART pins and baud rates, measurement averaging, and SD recording-buffer
  settings are grouped under their corresponding **EBCTester** submenus.
- **OTA Suite update checks → Fixed firmware manifest URL**:
  your deployment URL, e.g. `https://updates.example.org/BatteryTester/firmware.json`.
- HTTP requires **OTA Suite firmware updates → Allow unencrypted HTTP OTA**.
  Fresh configurations default to HTTP disabled; the existing saved opt-in is
  migrated. HTTPS always retains CA,
  hostname and certificate date validation, including when HTTP is allowed.
- The legacy **Fixed firmware URL** setting is retained for configuration
  compatibility but is not used by the manifest-based updater.
- **OTA Suite network**: optional AP/admin password (8–63 characters)
  and SNTP server. An empty password setting uses the stored password or
  generates and persists one in NVS. NVS errors never silently erase settings.

For initial provisioning, install the partition table and rollback-enabled
bootloader serially. This project requires a **16 MiB flash** module, such as an
ESP32-S3 N16R8. The two OTA app slots each have **0x570000 (5,701,632) bytes**
and the `storage` data partition has **0x500000 (5 MiB)**. It is reserved for
LittleFS but is not mounted or used by the application yet.
Changing the partition table cannot be done through OTA; install the new
partition table and firmware over USB. Back up important device settings before
repartitioning, and verify the exact module marking before flashing.
Existing `sdkconfig` selections take priority over defaults; legacy BatteryTester
settings are imported by Kconfig on the first rebuild.

Connect to `BatteryTester-XXXXXX` using the password printed on the serial
console, open **http://192.168.4.1**, log in as **admin** with the AP password,
and enter WLAN credentials. The protected AP remains a recovery path; channel
changes can briefly interrupt AP access. The UI also displays the station IP.

The project remains compatible with the existing manifest and immutable release
layout. Firmware **1.3.35** removes machine-specific defaults for new
installations; existing local `sdkconfig` and saved NVS settings are preserved.

When the optional MQTT component is compiled in, configure the broker from
**Settings → Home Assistant MQTT** in the device web interface. Host, port,
username, password, topic prefix, measurement expiry interval and remote-start
permission are stored on the device in NVS, so release builds do not contain
the end user's broker credentials. The password is not returned by the settings
API. Saving applies settings and starts or stops the client; **Test connection**
retries the saved settings and the page reports connection status. Broker
traffic is unencrypted TCP by default; restrict the broker account's topic
permissions.

## SD card

The SD-card component defaults to 4-bit SDMMC with CLK=GPIO14, CMD=GPIO15,
D0=GPIO2, D1=GPIO4, D2=GPIO5 and D3=GPIO6. **menuconfig → SD Card Manager
→ Default SD card bus mode** selects **4-bit SDMMC** or **SPI** as the default.
The **SD card bus** card under Settings overrides it at runtime: the selection
is stored in NVS (namespace `sd_card`, key `bus`) and applies after a restart
("Save and restart" restarts only while no operation or recording is active).
Both pin sets are always compiled in. SPI defaults to
MISO=GPIO2, MOSI=GPIO15, CS=GPIO6 and CLK=GPIO14; GPIO 7 remains the separate,
active-high card-detect input in either mode. SPI and SDMMC clock rates and
their pin assignments can be configured in the same menu. Both clocks default
to 10 MHz. Ensure the chosen pins are available on the board and use suitable
external pull-ups on the SD signal lines (3.3 V only). `GET /api/sd/status`
reports `busMode`, `configuredBusMode` and `restartRequired`;
`POST /api/sd/bus` (`{"mode":"spi"|"sdmmc","restart":bool}`, CSRF required)
stores the mode and returns 202 when a restart was scheduled.

Format cards as FAT32. The ESP-IDF 6.0.1 bundled FatFs configuration disables
exFAT, which this component therefore does not support. FatFs API UTF-8 encoding
is enabled for international file names. An unavailable or unformatted card is
reported in the SD card tab and is never automatically formatted; it does not
block OTA or normal device startup. The tab supports folder listing, upload,
download, empty-file/folder creation, rename/move and deletion of files or
empty folders.

## Battery test database

`battery_database` creates `/sdcard/ebc_a20.db` when a card is mounted; if the
device boots without a card, it waits and initializes the database after card
insertion. It never formats the card or deletes an existing database. SQLite
schema follows the current MariaDB base schema after migrations 001–005: the six
tables `ebc_test_plan`, `ebc_test_plan_step`, `ebc_battery`, `ebc_test_run`,
`ebc_run_segment` and `ebc_measurement`, plus `ebc_measurement_view` and the
same column names. A separate `ebc_schema_metadata` table stores SQLite's
application schema version; startup validates it and the expected schema
objects. SQLite and MariaDB require different physical column types, but the
logical fields are kept aligned for the existing synchronizer; DCIR adds schema version 2.

SQLite database and rollback-journal writes now use the SD-backed FATFS VFS.
The database component enables `synchronous=FULL`, real `fsync()` calls, foreign
keys, and SQLite's integrity check. The VFS does not claim SD atomic-write
capabilities, and database operations must continue to hold the shared SD lock.
If the database file has no valid SQLite header, startup preserves it under an
`ebc_a20.db.invalid-XXXXXXXX` name before creating a fresh database; it never
silently truncates or deletes that file.

Database initialization runs in a low-priority background task, so
an unavailable or slow SD/database does not hold up the web interface. The
database task retries while the card is unavailable; a database error on a
mounted card is reported once and stops initialization rather than repeatedly
renaming and recreating the database file.

Measurements from the EBC-A20/A40L are recorded by `test_store` (see "EBC-A20/A40L
tester"); MariaDB backup is described below.

## Measurement dashboard (offline)

The `battery_dashboard` component adds the **Measurements** tab to the web UI. All
data comes from the SQLite database on the SD card and Chart.js 4.4.9 is served
from flash (`/ui/dashboard/chart.js`, MIT license in
`components/battery_dashboard/assets/CHARTJS-LICENSE.md`), so the page works
on the setup AP without internet. It shows battery/run selection, cards for
current, voltage, capacity and status, voltage/current and capacity charts per
segment, capacity per segment, and the segment timeline. Running tests refresh
every 15 seconds.

| Endpoint | Purpose |
| --- | --- |
| GET `/api/db/runs` | Batteries and up to 300 newest test runs |
| GET `/api/db/run?id=` | Run, battery, segments and last measurement |
| GET `/api/db/measurements?id=&max=` | Chunked series `[segment, uptimeMs, epochMs, I, U, Ah]`, stride-sampled to `max` (200–6000, default 1500) keeping segment boundaries |
| POST `/api/db/demo` | Create a simulated 20 Ah LiFePO4 run (charge, rest, discharge, rest, charge; ~1200 points) |
| POST `/api/db/run/delete?id=` | Delete a run with segments and measurements (and its demo battery when unused) |

The tab order is set with `CONFIG_OTA_SUITE_WEB_TAB_ORDER` in
`sdkconfig.defaults` (`dashboard,battery,manual,auto,resistance,overview,sd-card,updates,settings`);
the first tab opens on page load.

The header and browser title use the firmware project name **EBCTester**.
The top menu splits tabs into **Battery tester** (Measurements, Battery, Manual,
Automatic, Internal resistance) and **Administration** (Device information, SD card, Updates,
Settings); switching sections remembers the last selected tab within each
section. Green headers and bundled SVG icons work without an internet connection.
The RX LED in the header toggles for newly counted valid UART frames.
The browser polls `/api/tester/status` every 500 ms while visible; received
frames since the previous poll are replayed as LED toggles, not generated by
a free-running heartbeat. Disconnection turns the LED grey; request errors
are shown in amber.

All endpoints require web login; POST endpoints also require the CSRF token.
Each request opens the database under the shared SD lock and closes it again;
503 means the card or database is not ready or busy.

## MariaDB backup

The `mariadb_sync` component copies local test runs (plan, steps, battery, run,
segments, measurements) to the existing `ebc_a20` MariaDB schema. Configure it
in **Settings → MariaDB backup** (host, port, database, user, password,
automatic interval in minutes; 0 = manual only). Settings are stored in NVS
namespace `mariadb_sync`; the password is never returned to the browser.
There is no default server address and sync is disabled on fresh installations.
The base MariaDB schema from migrations 001-005 must already exist; this
repository includes the additional DCIR migration
[`sql/006_resistance_results_mariadb.sql`](../sql/006_resistance_results_mariadb.sql),
not a replacement for that existing schema.

- Progress per run is tracked locally in `ebc_sync_state`; only new segments
  and measurements are sent (batches of 200, `INSERT IGNORE`), so repeated or
  interrupted syncs never duplicate data. "Resend all" resets it.
- Automatic sync runs only with WLAN, a ready SD database and a stored password.
- Runs deleted locally stay in MariaDB.
- Requires a `mysql_native_password` account with SELECT/INSERT/UPDATE on
  `ebc_test_plan`, `ebc_battery`, `ebc_test_run`, additionally DELETE on
  `ebc_test_plan_step`, and INSERT on `ebc_run_segment`, `ebc_measurement`.
- The connection is plain TCP without TLS: use it only in the trusted LAN.

API (login required, POST with CSRF token): `GET /api/mariadb/status`,
`POST /api/mariadb/settings`, `/api/mariadb/test`, `/api/mariadb/sync`,
`/api/mariadb/full-sync` (409 while a job is running).

## EBC-A20/A40L tester

UART pins and baud rates are set in menuconfig under **EBC battery tester
UARTs** (both 8O1):

| UART | Use | TX | RX | Baud |
| --- | --- | --- | --- | --- |
| UART1 | Tester ("Prozessor") | 17 | 18 | 9600 |
| UART2 | Dongle (PC software) | 8 | 9 | 9600 |

The EBC-A20/A40L use 5 V TTL levels: connect it through a level shifter, the
ESP32-S3 GPIOs are not 5 V tolerant. The dongle↔tester bridge is not
implemented yet; UART2 is initialized and its input discarded.

Select the connected model in the **EBC tester** card under Settings
(stored in NVS namespace `tester`, key `model`; default EBC-A20). It sets the
maximum charge and discharge current: **20 A** for the EBC-A20 and **40 A** for
the EBC-A40L. Manual starts, test plans (when loaded and started) and the
internal resistance measurement are checked against it; the web forms adopt
the limit. The model cannot be changed during an operation or recording.
`GET /api/tester/status` and `/api/tester/settings` report `testerModel` and
`maxCurrent`; `POST /api/tester/model` (`{"model":"A20"|"A40L"}`) changes it.

- **Battery**: label, manufacturer, serial, manufacturing date, nominal
  capacity, notes; stored in NVS and optionally in the SQLite database.
- **Manual**: live values, connect/disconnect, auto-connect, Stop, charging
  C-CV, discharging D-CC/D-CP.
  Connect/disconnect and all start buttons are blocked while a command is
  pending, a manual test is starting/active, or a plan is running (including
  wait steps). The firmware enforces this too; Stop and Abort remain available.
  A manual start awaits an active mode for up to 10 seconds before logging
  an error and requesting Stop.
- **Automatic**: test plan with up to 10 steps (D-CC, D-CP, C-CV, wait, cycle),
  stored in NVS and the database. The plan runs on the ESP32; after a reboot
  a running plan is marked "Interrupted" and must be reset.

Active tests are recorded as test runs with segments; each measurement is the
average of `CONFIG_TEST_CONTROL_AVERAGING_LENGTH` frames (default 20 ≈ 20 s).
The device id is `CONFIG_TEST_STORE_DEVICE_ID`.

API (login required, POST with CSRF token): `GET /api/tester/status`,
`/api/tester/settings`, `/api/tester/catalog`; `POST /api/tester/battery`,
`/api/tester/command`, `/api/tester/plan`, `/api/tester/plan/action`,
`/api/tester/model`.

## DC resistance / last-pulse test

The **Internal resistance** tab uses the separate `resistance_test` component.
Choose **State of charge already prepared** for rest followed by a load pulse,
or **Automatic preparation and measurement** to fully charge in C-CV mode,
then remove `usableCapacityAh * (100 - targetSOC) / 100` Ah before resting.
Capacity removal integrates the **measured current** with monotonic frame
timestamps (trapezoidal integration); target SOC is a calculated estimate, not
an independently measured SOC. A target of 100% skips preparation discharge.
No temperature is assumed: results explicitly report temperature unknown.
Four-wire voltage measurement is assumed at the hardware.

All measurement parameters are editable and persisted in NVS: usable capacity,
target/prepared SOC, charge current/voltage/cutoff, preparation discharge current,
maximum charge/preparation times, rest time, U1 averaging window, permitted idle
current, pulse current/duration, three sampling times, voltage/power limits,
maximum UART gap, timing tolerance and minimum credible voltage drop.
Defaults use separate results at **1, 5, 10 seconds** and a 12-second pulse.
These are examples, not universal battery ratings; verify them before starting.
Changing parameters or preparation method resets the UI safety confirmation.

U1 is averaged from idle raw frames at the end of the rest phase (at least
two samples); U2 and I2 are single **raw frames**, bypassing normal 20-frame
averaging. `R_mOhm = 1000 * (U1 - U2) / measuredI2`.
Time zero is the command-queue timestamp after the final idle frame; the link
sends the command immediately after this callback. The actual load onset lies
between the final idle frame and first loaded frame; the UI displays this
sampling gap. The first raw sample at or after each requested time is used and
its actual time is displayed. Late samples, missing samples, too-small/negative
voltage drops or absent current are invalid, not zero-ohm results. UART sampling
and voltage resolution limit accuracy, particularly at 1 second.

All other starts, connect/disconnect and battery changes are blocked until the
test stops. Stop/Abort remain available. Protection limits, missing frames,
recorder failures and preparation timeouts request Stop. Completion/abort waits
for a confirmed idle frame with acceptable idle current. Unconfirmed start or
Stop times out after 10 seconds; a lost UART cannot guarantee a physical stop,
so check the tester locally. The EBC discharge command also carries a rounded-up
whole-minute runtime limit; UART protection is not a replacement for an
independent battery protection circuit. Tests never resume after reboot.

Raw data is stored as a normal SD test run, visible in **Measurements**.
`DCIR v1;` run notes preserve parameters, results, actual sample times, U1/I1,
onset uncertainty, capacity removal and error information; initial metadata is
stored with the run so interrupted tests remain identifiable after reboot.
The tab lists the newest 20 DCIR runs including their structured results.
MariaDB backup includes raw data, notes and structured results after migration
006 (see below). A finished result remains in RAM while
powered; after reboot use the SD history.

Authenticated API: GET `/api/tester/resistance` (configuration and status),
POST `/api/tester/resistance` (`save`/`start` with complete `config`, or `abort`),
GET `/api/tester/resistance/history`. POST requires CSRF.
This feature is a dedicated test; extracting DCIR from ordinary plan transitions
is not enabled.

### Structured DCIR storage and MariaDB migration

SQLite automatically migrates schema version 1 to **2**, preserving existing
batteries, runs and measurements. `ebc_resistance_result` contains three rows
per completed/aborted DCIR run, keyed by `(run_id, result_number)`. Results and
run completion are committed in the same writer transaction.
Each row stores the requested/actual sampling time, U1/I1, U2/I2, resistance
in mOhm (NULL when invalid), validity, error and onset uncertainty.
`captured_at_utc` is the actual sample timestamp in UTC with millisecond
precision; `device_uptime_ms` remains available without synchronized wall time.
Missing samples have NULL timestamps/loaded values. Without synchronized time,
UTC is NULL, not an invented calendar timestamp. SNTP supplies UTC when WLAN is
available; an offline start without a valid clock cannot know calendar time.

The battery is linked through `ebc_test_run.battery_id`; the view
`ebc_resistance_result_view` exposes battery id/label, device, test start and run
status alongside each result. This avoids duplicating local battery ids in the
MariaDB results; the existing synchronizer resolves the remote battery id.

Before syncing structured results, back up MariaDB, select the existing
database (e.g. `ebc_a20`) and execute
[`sql/006_resistance_results_mariadb.sql`](../sql/006_resistance_results_mariadb.sql)
as a schema administrator. It creates the result table and reporting view,
matching the existing parent `run_id` column type/charset/collation for its
foreign key. No existing tables or data are dropped. Grant SELECT/INSERT/UPDATE
on the new table to the firmware user (example in the SQL file), and SELECT on
the view for reporting accounts. The firmware does not run server-side DDL.

Sync upserts all three result rows before saving the completed run signature.
Retries do not duplicate rows; missing MariaDB migration/permissions explicitly
report a DCIR sync failure. Row count is included in the sync signature so
new structured results are not skipped for previously transferred runs.
Old DCIR tests made before this update retain their notes and raw measurements;
they are not automatically converted to structured rows, since their exact
per-result UTC timestamps were not previously stored.

Example MariaDB report:

```sql
SELECT battery_id, battery_label, test_started_at_utc, captured_at_utc,
       requested_seconds, actual_seconds, resistance_mohm, valid, error
FROM ebc_resistance_result_view
ORDER BY test_started_at_utc DESC, run_id, result_number;
```

Run `python tests/resistance_schema_test.py` to validate fresh/existing SQLite
migrations, battery association, millisecond UTC, NULL handling, constraints,
deletion cascade, atomic writes and the actual synchronization SELECT statements.

Standalone host tests in `tests/resistance_host/dcir_host_test.cpp` compile the
actual component against UART/NVS/clock/recorder stubs. From an MSVC developer
prompt, use `/std:c++17 /EHsc` with include paths `tests/resistance_host/stubs`,
`components/resistance_test/include`, `components/ebc_link/include`,
`components/test_store/include`, and compile that test together with
`components/resistance_test/resistance_test.cpp`. Run the resulting executable.
They cover prepared/automatic paths, measured-current resistance and capacity,
1/5/10-second sampling, invalid results, timeouts, frame gaps, voltage limits,
recording loss, command errors and stop acknowledgement. Real hardware accuracy
and battery safety must still be verified.

## Update behavior

- After WLAN becomes usable and every **six hours**, only `firmware.json` is
  checked. HTTPS waits for synchronized time. Reconnect checks are rate-limited
  to at most once per minute; overdue checks run on reconnection.
  **No automatic binary download, installation or OTA retry occurs.**
- Browser status polling is read-only. **Check updates** sends authenticated,
  CSRF-protected **POST `/api/updates/check`** with an empty body.
- Status exposes check progress/errors, available version, notes, and last
  successful check (Unix seconds, or uptime seconds without a synchronized clock).
  These values reset on reboot. Failed checks invalidate the candidate but
  preserve the last successful check details.
- **Start update**, followed by confirmation of the displayed version, sends
  authenticated, CSRF-protected **POST `/api/ota`** with empty body and
  `X-Update-Version`. It must match the validated candidate. Boot confirmation,
  connected WLAN, idle network configuration, and a candidate younger than six
  hours are required. HTTPS additionally requires synchronized time.
- Concurrent checks/installations, changed versions, stale candidates and
  missing candidates are rejected. After a failed installation, check again
  before retrying. WLAN changes are refused during OTA.
- Only strictly numeric `major.minor.patch` versions are accepted: no leading
  zeros, suffixes or integer overflow. Comparison is numeric (`1.10.0 > 1.9.0`);
  equal/older versions are not installable.
- Both manifest and image must return **HTTP 200 directly**, without redirects.
  HTTPS validates the certificate chain, hostname and dates using the ESP CA
  bundle. Extend the bundle for private CAs; never disable validation.
- OTA streams into the inactive partition. **Exact downloaded binary size and
  SHA256**, not the descriptor's ELF digest, are checked **before**
  `esp_ota_end()` and `esp_ota_set_boot_partition()`. Project/version must match
  the manifest. IDF additionally validates image integrity and chip target.
  Previously rolled-back images and identical ELF images are rejected.
- Chunked images are supported; progress uses the manifest size. Download socket
  timeout is 15 seconds, overall budget five minutes (a blocking operation may
  add its timeout). Incomplete/oversized/corrupt images never select a new boot
  partition. Errors are shown in the UI and logged.
- On success, restart after three seconds. The pending firmware is confirmed
  only after local services initialize and survive a 10-second window.
  Earlier reset/crash triggers bootloader rollback. Internet connectivity is
  not a boot-health requirement. Add battery-critical self-tests when available;
  this startup window is not a complete hardware self-test.

## Manifest and atomic NAS publication

Publication is **disabled in a fresh checkout**. To retain automatic
publication on your own machine, create the Git-ignored
`CMakeUserConfig.cmake` in the project root and reconfigure the project:

```cmake
set(EBCTESTER_PUBLISH_FIRMWARE ON)
set(EBCTESTER_PUBLISH_DIR "D:/firmware-releases")
set(EBCTESTER_PUBLISH_ORIGIN "https://updates.example.org")
set(EBCTESTER_PUBLISH_URL_PATH "/BatteryTester")
```

Use your own existing writable local/NAS directory and server origin. The same
values can be set as CMake cache variables. To disable a local setting, change
the local file to `OFF` and reconfigure. Configure the matching device manifest
URL in menuconfig; that URL is independent of the publisher settings.

When enabled, each successful ESP-IDF build:

1. Copies/verifies the actual generated image to immutable **`EBCTester-1.3.36.bin`**.
   A same-version release with different bytes fails: bump the version, never
   overwrite a published release.
2. Atomically replaces **`EBCTester.bin`** for older firmware.
3. Publishes **`firmware.json` last**, by same-directory `.uploading` + rename.

Size and SHA256 come from `build\EBCTester.bin` and are checked after copying.
Older immutable releases remain for in-progress downloads.
Publication failure explicitly fails the build step. Serve the directory at
your configured origin and URL path. The local copy directory
does not determine the web URL. Existing devices must also have the new
manifest URL configured in menuconfig and installed in their firmware.
For a different origin, change publication and device manifest configuration.
SMB/server rename semantics determine atomicity; use one publisher at a time.
**A build never installs firmware.**

Manifest schema (all seven fields mandatory; no extra or duplicate fields):

```json
{
  "project": "EBCTester",
  "target": "esp32s3",
  "version": "1.3.36",
  "url": "https://updates.example.org/BatteryTester/EBCTester-1.3.36.bin",
  "size": 123456,
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "notes": "Release notes"
}
```

Example size/hash are illustrative; publication inserts real values. The
manifest is limited to **less than 2048 bytes**, notes to **512 UTF-8 bytes**,
positive integer size to OTA partition capacity, and SHA256 to 64 lowercase hex
digits. Project/target must match the device. Image URL must exactly match the
configured manifest origin/directory and `<project>-<version>.bin`, with no
credentials, queries, fragments, traversal or redirects. Manifest socket timeout
is five seconds; overall budget 20 seconds (blocking operations may add timeout).
Availability checks never contact the image URL.

## Security and persistence

Local administration uses Basic authentication over **HTTP**: trusted LAN or
protected AP only, never expose port 80 to the Internet. HTTP does not encrypt
WLAN credentials/admin passwords. AP password grants administration and is
printed on serial; protect physical access. NVS is not encrypted by default.
Consider HTTPS administration, flash/NVS encryption, Secure Boot and signed
images for production. SHA256 protects against accidental corruption, **not**
replacement of both manifest and image over HTTP. HTTP test mode explicitly
warns in the UI/confirmation and must only be used on a trusted test LAN.
No HTTPS-to-HTTP fallback exists. HTTPS transport/image integrity is not a
substitute for firmware signatures.

Bootstrap **5.3.3** CSS is embedded locally by `ota-suite`; its MIT license is
in `ota-suite\components\web_interface\assets\BOOTSTRAP-LICENSE`.

## RGB LED

ESP32-S3-DevKitC-1 uses a single WS2812 (GRB). Default GPIO **48**; select **38**
for board revisions using that pin in **OTA Suite status LED** menuconfig.
The separate power LED is unaffected. Brightness is limited to 24/255.

| State | RGB LED |
| --- | --- |
| Normal/startup | Smooth 30-second color cycle |
| Firmware download | Blue pulse (2 seconds) |
| Validation | Yellow pulse (may be brief) |
| Waiting for reboot | Solid green |
| Failed OTA | Red blink (0.5 seconds on/off) until manual attempt or reboot |

The low-priority LED task updates every 50 ms and never starts OTA.
Initialization errors prevent boot confirmation; runtime LED errors stop LED
output without interrupting OTA. LED behavior does not prove hardware health.

## Verification

Build with **ESP-IDF: Build your project** in VS Code; never flash during the
publication-only workflow. Host UI/publication tests require Node and CMake (no Node packages):

```text
node --test tests\web_interface.test.js tests\publication.test.js
```

Set `CMAKE_EXE` for publication tests if CMake is not on PATH. VS Code's
`Code.exe` can run the tests with `ELECTRON_RUN_AS_NODE=1` when Node is absent.
If using a non-default package checkout for host tests, set the `OTA_SUITE_DIR`
environment variable to its path.
`SD_CARD_MANAGER_DIR` similarly overrides the SD checkout for language tests
and the local browser fixture.
`tests\update_manifest.test.c` tests the actual C parser/policy. Compile with
`update_manifest.c`, `update_policy.c`, managed `cJSON.c` and their include
directories using a host C compiler. Tests cover strict types, numeric versions,
URL confinement, duplicates/missing fields, bounded depth/size, notes/hash limits.

On hardware, verify manifest-only automatic checks, six-hour schedule, reconnect
rate limiting, manual confirmation, stale candidates, unauthorized/CSRF requests,
TLS expiry/untrusted CA/date, redirects, 404, wrong project/target, truncated,
oversized and hash-mismatched downloads. Confirm no boot selection on failures.
Test power loss and pending-image rollback only on a development device, never
during a real battery test. Host tests/build/NAS hash checks cannot verify flash
behavior, WLAN event timing, TLS handshakes or hardware rollback.
