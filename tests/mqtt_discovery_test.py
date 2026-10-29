# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MqttDiscoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "components/ha_mqtt/ha_mqtt.c").read_text(encoding="utf-8")
        entities = source[source.index("typedef struct {\n    const char *key;"):
                          source.index("static bool command_name_valid")]
        add_entity = source[source.index("static bool add_discovery_entity"):
                            source.index("static void get_configuration_url")]
        publish = source[source.index("static esp_err_t publish_discovery"):
                         source.index("static bool settings_valid")]
        live = source[source.index("static char live_measurement_payload"):
                      source.index("typedef enum {")]
        measurement = source[source.index("esp_err_t ha_mqtt_publish_measurement"):
                             source.index("\n#else", source.index("esp_err_t ha_mqtt_publish_measurement"))]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <math.h>
#include <string.h>
#include "cJSON.h"
#define HA_DEVICE_ID_MAX 23
#define HA_MEASUREMENT_EXPIRE_MULTIPLIER 3
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_INVALID_STATE -4
#define MQTT_USER_EVENT 100
#define portENTER_CRITICAL(mux) ((void)0)
#define portEXIT_CRITICAL(mux) ((void)0)
#define ESP_LOGD(tag, ...) ((void)0)
#define ESP_LOGW(tag, message) fprintf(stderr, "%s\n", message)
typedef int esp_err_t;
static const char *TAG = "test";
static char device_id[] = "ebctester_206ef1999758";
static char state_topic[] = "ebctester/state";
static char availability_topic[] = "ebctester/availability";
static char discovery_topic[] = "homeassistant/device/ebctester_206ef1999758/config";
static void *mqtt_client;
static bool started = true;
static atomic_bool mqtt_connected = true;
typedef struct { float voltage_v, current_a, capacity_ah; } ha_mqtt_measurement_t;
typedef struct { int event_id, msg_id; } esp_mqtt_event_t;
static esp_mqtt_event_t queued_event;
static int dispatch_result;
static int publish_result;
static int publish_count;
static char published_payload[112];
static int esp_mqtt_dispatch_custom_event(void *client, esp_mqtt_event_t *event)
{
    (void)client;
    assert(event->event_id == MQTT_USER_EVENT);
    queued_event = *event;
    return dispatch_result;
}
static int esp_mqtt_client_publish(void *client, const char *topic, const char *payload,
                                   int len, int qos, int retain)
{
    (void)client;
    assert(strcmp(topic, "ebctester/state") == 0);
    assert(len == 0 && qos == 0 && retain == 0);
    ++publish_count;
    snprintf(published_payload, sizeof(published_payload), "%s", payload);
    return publish_result;
}
static struct { uint32_t measurement_interval_seconds; } active_settings = {20};
static void get_configuration_url(char *url, size_t size)
{
    snprintf(url, size, "http://192.168.0.65/");
}
static int esp_mqtt_client_enqueue(void *client, const char *topic,
                                  const char *payload, int len, int qos,
                                  int retain, bool store)
{
    if (client != mqtt_client || len != 0 || qos != 1 || retain != 1 || store)
        return -1;
    puts(topic);
    puts(payload);
    return 1;
}
'''
        harness += entities + add_entity + publish + live + measurement
        harness += r'''
int main(int argc, char **argv)
{
    if (argc == 1) return publish_discovery() == ESP_OK ? 0 : 1;
    (void)argv;
    mqtt_client = (void *)&publish_count;
    const ha_mqtt_measurement_t sample = {3.3216f, 1.2346f, 0.12346f};
    const ha_mqtt_measurement_t invalid = {NAN, 0, 0};
    assert(ha_mqtt_publish_measurement(NULL) == ESP_ERR_INVALID_ARG);
    assert(ha_mqtt_publish_measurement(&invalid) == ESP_ERR_INVALID_ARG);
    mqtt_connected = false;
    assert(ha_mqtt_publish_measurement(&sample) == ESP_ERR_INVALID_STATE);
    mqtt_connected = true;
    assert(ha_mqtt_publish_measurement(&sample) == ESP_OK);
    assert(publish_count == 0);
    assert(ha_mqtt_publish_measurement(&sample) == ESP_ERR_INVALID_STATE);
    publish_live_measurement(&queued_event);
    assert(publish_count == 1);
    assert(strcmp(published_payload,
        "{\"voltage\":3.322,\"current\":1.235,\"capacity\":0.1235}") == 0);
    assert(!live_measurement_pending);

    assert(ha_mqtt_publish_measurement(&sample) == ESP_OK);
    esp_mqtt_event_t stale_event = queued_event;
    mqtt_connected = false;
    discard_live_measurement();
    mqtt_connected = true;
    assert(ha_mqtt_publish_measurement(&sample) == ESP_OK);
    publish_live_measurement(&stale_event);
    assert(publish_count == 1 && live_measurement_pending);
    publish_live_measurement(&queued_event);
    assert(publish_count == 2 && !live_measurement_pending);

    dispatch_result = ESP_FAIL;
    assert(ha_mqtt_publish_measurement(&sample) == ESP_FAIL);
    assert(!live_measurement_pending);
    dispatch_result = ESP_OK;
    assert(ha_mqtt_publish_measurement(&sample) == ESP_OK);
    mqtt_connected = false;
    publish_live_measurement(&queued_event);
    assert(publish_count == 2 && !live_measurement_pending);
    mqtt_connected = true;
    publish_result = -1;
    assert(ha_mqtt_publish_measurement(&sample) == ESP_OK);
    publish_live_measurement(&queued_event);
    assert(publish_count == 3 && !live_measurement_pending);
    publish_live_measurement(&queued_event);
    assert(publish_count == 3);
    puts("Live measurement checks passed");
    return 0;
}
'''
        cjson = ROOT / "managed_components/espressif__cjson/cJSON"
        with tempfile.TemporaryDirectory(prefix="ebctester-mqtt-") as directory:
            work = Path(directory)
            test_source = work / "discovery.c"
            test_source.write_text(harness, encoding="utf-8")
            subprocess.run(
                ["cl", "/nologo", "/utf-8", "/W4", "/std:c11", "/experimental:c11atomics",
                 f"/I{cjson}", str(test_source), str(cjson / "cJSON.c"),
                 f"/Fe:{work / 'discovery.exe'}"],
                cwd=work, check=True,
            )
            cls.measurement_result = subprocess.run(
                [str(work / "discovery.exe"), "--measurements"],
                check=True, capture_output=True, encoding="utf-8",
            ).stdout.strip()
            result = subprocess.run(
                [str(work / "discovery.exe")], check=True, capture_output=True,
                encoding="utf-8",
            )
        cls.topic, payload = result.stdout.splitlines()
        cls.payload = json.loads(payload)

    def test_nonblocking_live_measurement_handoff_and_disconnects(self):
        self.assertEqual(self.measurement_result, "Live measurement checks passed")

    def test_required_device_and_origin(self):
        self.assertEqual(self.payload["o"], {"name": "EBCTester"})
        self.assertEqual(self.payload["dev"]["name"], "EBCTester")
        self.assertEqual(self.payload["dev"]["ids"], ["ebctester_206ef1999758"])
        self.assertEqual(self.payload["dev"]["cu"], "http://192.168.0.65/")

    def test_only_supported_device_discovery_root_keys(self):
        self.assertEqual(
            set(self.payload), {"dev", "o", "cmps", "avty_t", "pl_avail", "pl_not_avail"}
        )

    def test_stable_topic_and_entity_ids(self):
        self.assertEqual(self.topic, "homeassistant/device/ebctester_206ef1999758/config")
        components = self.payload["cmps"]
        self.assertEqual(len(components), 13)
        for key, component in components.items():
            with self.subTest(entity=key):
                self.assertEqual(component["unique_id"], f"ebctester_206ef1999758_{key}")
                self.assertEqual(component["state_topic"], "ebctester/state")
                self.assertEqual(component["p"], "binary_sensor" if key == "connected" else "sensor")

    def test_availability_and_measurement_expiry(self):
        self.assertEqual(self.payload["avty_t"], "ebctester/availability")
        self.assertEqual(self.payload["pl_avail"], "online")
        self.assertEqual(self.payload["pl_not_avail"], "offline")
        for key, component in self.payload["cmps"].items():
            with self.subTest(entity=key):
                if key in {"voltage", "current", "capacity"}:
                    self.assertEqual(component["expire_after"], 60)
                else:
                    self.assertNotIn("expire_after", component)


if __name__ == "__main__":
    unittest.main()
