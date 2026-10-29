<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# Optional Home Assistant MQTT component

This ESP-IDF component is disabled by default. Enable **EBCTester → Home
Assistant MQTT → Enable MQTT component** in `menuconfig` to include the
optional feature in the firmware. The end user enters the broker host, port,
username, password, topic prefix and measurement interval in the device web
interface's **Settings → Home Assistant MQTT** page. These user-specific
values are stored in device NVS, not in source control or `sdkconfig`.
Saving applies the settings immediately. The page shows connection state and
the latest connection message, and its **Test connection** button retries
using the saved configuration.

Call `ha_mqtt_settings_defaults()` and provide the broker `host`, `port`,
optional `username` and `password`, and `topic_prefix` before calling
`ha_mqtt_start()`. Remote start permission defaults to off and can be enabled
in the MQTT settings page. On connection, the component publishes
one retained Home Assistant device-discovery document to
`homeassistant/device/ebctester_<wifi-station-mac>/config`. Every entity's
`unique_id` includes that same stable MAC-derived device ID. The message
includes the required device (`dev`) and origin (`o`) mappings; the device
name belongs inside `dev`, not at the discovery document's root. Discovery is
republished when the broker announces `online` on `homeassistant/status`.
The document describes the voltage, current, capacity, mode, plan, connection,
assigned-battery, test-result, and timestamp entities on `<topic_prefix>/state`.
Set `measurement_interval_seconds` to the interval between recorded averages;
measurement entities expire after three such intervals.

The test recorder invokes its sample-persisted callback only after the
transaction commits successfully. The application bridges that callback to
the MQTT API. It hands off one live sample to the MQTT event task, which
publishes a non-retained QoS 0 JSON update to `<topic_prefix>/state`
containing the voltage, current, and
capacity from that exact averaged database sample, with matching database
precision. When disconnected, measurements are skipped rather than buffered. A busy
handoff skips new samples; disconnecting discards any pending handoff.
QoS 0 publishing runs in the MQTT task, not the database writer, and does not
use the MQTT outbox or replay measurements after reconnecting.
The recorder and database writer do not wait for MQTT.

When the application supplies a command callback, the component subscribes to
`<topic_prefix>/command` and accepts one JSON object per message:

| Command | Payload |
| --- | --- |
| Start the selected plan | `{"command":"start_plan"}` |
| Stop the current operation | `{"command":"stop"}` |
| Abort the running plan | `{"command":"abort"}` |
| Select a saved plan | `{"command":"select_plan","name":"Plan name"}` |
| Select a saved battery | `{"command":"select_battery","name":"Battery label"}` |

Commands are validated and handled by a separate worker task, then passed to
the same test-control functions used by the web interface. Plan start is
ignored unless **Allow remote start commands** is enabled (default off).
Stop, abort and selection commands do not require that opt-in. Plan and
battery selections load existing entries from the SD database; MQTT cannot
write arbitrary setpoints or create database entries. Malformed, unknown,
fragmented or overlong commands are ignored and logged only at debug level.
The command queue is bounded; if it is full, Stop and Abort take priority over
queued commands.

The component does not yet publish event-state changes such as mode, plan
status or assigned battery.

The MQTT client uses TCP without TLS by default. The broker password is stored
in device NVS, never returned by the settings API, and never logged. Use a
dedicated broker account with restricted topic permissions. The MQTT client
API remains decoupled from test-control internals; the EBCTester web adapter
owns settings persistence and bridges commands through a callback.

The component publishes retained `online` availability after a successful
connection and configures a retained `offline` Last Will at
`<topic_prefix>/availability`. Failed connections are retried with exponential
backoff from 1 second up to 60 seconds. MQTT runs in its own client task; this
component does not buffer measurements or call recording/test code.

## Discovery troubleshooting and host tests

In Home Assistant, open **Settings → Devices & services → MQTT** and configure
the broker entry. Under **Listen to a topic**, subscribe to `homeassistant/#`,
then use **Test connection** on the tester to republish discovery. The device
should appear under **Settings → Devices & services → Devices** even before
measurements are recorded. If the message arrives but no device appears, check
Home Assistant's system logs for MQTT discovery validation errors.

On Windows, run `python tests\mqtt_discovery_test.py` from a Visual Studio
Developer Command Prompt. It compiles the firmware's discovery functions with
the project's cJSON dependency and captures the retained MQTT message without
connecting to a broker or hardware.
