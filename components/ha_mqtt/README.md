<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# Optional Home Assistant MQTT component

This ESP-IDF component is disabled by default. Enable **BatteryTester Home
Assistant MQTT → Enable MQTT component** in `menuconfig`, then enter the broker
host, port, optional username/password, topic prefix, and measurement interval.
The application starts the MQTT client after starting device services. It is
independent of EBCTester's recording and test-control internals.

Call `ha_mqtt_settings_defaults()` and provide the broker `host`, `port`,
optional `username` and `password`, and `topic_prefix` before calling
`ha_mqtt_start()`. Remote start permission defaults to off and can be enabled
with **Allow remote start commands**. On connection, the component publishes
one retained Home Assistant device-discovery document to
`homeassistant/device/ebctester_<wifi-station-mac>/config`. Every entity's
`unique_id` includes that same stable MAC-derived device ID. Discovery is
republished when the broker announces `online` on `homeassistant/status`.
The document describes the voltage, current, capacity, mode, plan, connection,
assigned-battery, test-result, and timestamp entities on `<topic_prefix>/state`.
Set `measurement_interval_seconds` to the interval between recorded averages;
measurement entities expire after three such intervals.

The test recorder invokes its sample-persisted callback only after the
transaction commits successfully. The application bridges that callback to
the MQTT API. It queues a non-retained QoS 0
JSON update to `<topic_prefix>/state` containing the voltage, current, and
capacity from that exact averaged database sample, with matching database
precision. When disconnected, measurements are skipped rather than buffered.
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

The MQTT client uses TCP without TLS by default. Store broker credentials only
in local, untracked configuration. Use a dedicated broker account with
restricted topic permissions. Credentials are never logged.

The component publishes retained `online` availability after a successful
connection and configures a retained `offline` Last Will at
`<topic_prefix>/availability`. Failed connections are retried with exponential
backoff from 1 second up to 60 seconds. MQTT runs in its own client task; this
component does not buffer measurements or call recording/test code.
