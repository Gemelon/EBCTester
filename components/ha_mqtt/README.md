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
The recorder and database writer do not wait for MQTT. This component does
not yet publish event-state changes or accept commands.

The MQTT client uses TCP without TLS by default. Store broker credentials only
in local, untracked configuration. Use a dedicated broker account with
restricted topic permissions. Credentials are never logged.

The component publishes retained `online` availability after a successful
connection and configures a retained `offline` Last Will at
`<topic_prefix>/availability`. Failed connections are retried with exponential
backoff from 1 second up to 60 seconds. MQTT runs in its own client task; this
component does not buffer measurements or call recording/test code.
