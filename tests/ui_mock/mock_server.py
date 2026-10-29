# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0
"""Serves the real web UI assets with a simulated device API for testing without hardware.

Run `python3 tests/ui_mock/mock_server.py` and open http://127.0.0.1:8080. The API keeps its
state in memory; nothing is sent to a device. The tester limits mirror
`components/test_control/tester_model.cpp`; keep both in sync.
"""

import argparse
import copy
import json
import os
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(os.environ.get("OTA_SUITE_DIR", ROOT / "external" / "ota-suite"))
SD_MANAGER = Path(os.environ.get("SD_CARD_MANAGER_DIR", ROOT / "external" / "sd-card-manager"))
SHARED = SUITE / "components" / "web_interface" / "assets"
TESTER = ROOT / "components" / "test_control" / "assets"
SD_ASSETS = SD_MANAGER / "components" / "sd_card_manager" / "assets"
CSRF_TOKEN = "mock-csrf-token"

# id, label, placement, html, script
EXTENSIONS = [
    ("battery", "Battery", "tab", TESTER / "battery.html", TESTER / "battery.js"),
    ("manual", "Manual", "tab", TESTER / "manual.html", TESTER / "manual.js"),
    ("auto", "Automatic", "tab", TESTER / "auto.html", TESTER / "auto.js"),
    ("resistance", "Internal resistance", "tab", TESTER / "resistance.html", TESTER / "resistance.js"),
    ("sd-card", "SD card", "tab", SD_ASSETS / "sd_card.html", SD_ASSETS / "sd_card.js"),
    ("tester-settings", "EBC tester", "settings", TESTER / "tester_settings.html", TESTER / "tester_settings.js"),
    ("sd-settings", "SD card bus", "settings", SD_ASSETS / "sd_settings.html", SD_ASSETS / "sd_settings.js"),
]

MAX_CURRENT = {"A20": 20, "A40L": 40}

RESISTANCE_CONFIG = {
    "automatic": False, "chargeCurrent": 0.5, "chargeVoltage": 3.45, "chargeCutoff": 0.1, "capacityAh": 20,
    "targetSoc": 50, "prepCurrent": 0.5, "cutoffVoltage": 2.0, "maxVoltage": 3.8, "maxPower": 150,
    "restSeconds": 60, "baselineSeconds": 3, "idleCurrent": 0.02, "pulseCurrent": 0.5, "pulseSeconds": 12,
    "time1": 1, "time2": 5, "time3": 10, "timingTolerance": 1.5, "maxGap": 3, "maxChargeSeconds": 86400,
    "maxPrepSeconds": 86400, "minVoltageDrop": 0.001,
}


def initial_state():
    return {
        "tabOrder": "manual,auto,resistance,battery,overview,sd-card,updates,settings",
        "model": "A20",
        "connected": True,
        "busy": False,
        "mode": 2,
        "manual": {"chargeCurrent": 0.5, "chargeVoltage": 3.45, "chargeCutoff": 0.1, "dischargeCurrent": 0.5,
                   "dischargeCutoff": 2.5, "dischargeMinutes": 239, "power": 5, "powerCutoff": 2.5,
                   "powerMinutes": 239},
        "plan": {"name": "", "steps": []},
        "resistance": copy.deepcopy(RESISTANCE_CONFIG),
        "battery": {"label": "", "manufacturer": "", "serial": "", "date": "", "notes": "", "capacity": 0},
        "busMode": "spi",
        "configuredBusMode": "spi",
        "restarts": 0,
        "frames": 0,
        "requests": [],
    }


class MockDevice:
    """Simulated device state shared by all request handlers."""

    def __init__(self):
        self.lock = threading.Lock()
        self.state = initial_state()

    def reset(self, **changes):
        with self.lock:
            self.state = initial_state()
            self.state.update(changes)


def limit_text(limit):
    return {
        "charge": f"Current 0.01–{limit} A, charge voltage 0.01–30 V, cutoff current 0.01–{limit} A.",
        "discharge": f"Current 0.01–{limit} A, cutoff voltage 0.01–30 V, time 0–239 min.",
    }


def in_range(value, low, high):
    return isinstance(value, (int, float)) and low <= value <= high


def check_plan(steps, limit):
    for index, step in enumerate(steps, start=1):
        mode, p1, _, p3 = step
        if mode == 1 and p1 > limit * 100:
            return f"Step {index}: " + limit_text(limit)["discharge"]
        if mode == 3 and (p1 > limit * 100 or p3 > limit * 100):
            return f"Step {index}: " + limit_text(limit)["charge"]
    return ""


class Handler(BaseHTTPRequestHandler):
    device = None
    quiet = True

    def log_message(self, fmt, *args):
        if not self.quiet:
            super().log_message(fmt, *args)

    # ---------- responses ----------

    def send(self, status, body, content_type="text/plain; charset=utf-8"):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def send_json(self, value):
        self.send(200, json.dumps(value), "application/json")

    def send_file(self, path, content_type):
        self.send(200, path.read_bytes(), content_type)

    # ---------- GET ----------

    def do_GET(self):
        url = urlparse(self.path)
        with self.device.lock:
            self.device.state["requests"].append(("GET", url.path))
        if url.path == "/":
            html = (SHARED / "index.html").read_text(encoding="utf-8")
            return self.send(200, html.replace("<title>Firmware</title>", "<title>EBCTester (mock)</title>"),
                             "text/html; charset=utf-8")
        static = {
            "/app.js": (SHARED / "app.js", "text/javascript"),
            "/app.css": (SHARED / "app.css", "text/css"),
            "/bootstrap.min.css": (SHARED / "bootstrap.min.css", "text/css"),
        }
        if url.path in static:
            return self.send_file(*static[url.path])
        if url.path == "/i18n.js":
            en = (SHARED / "en.json").read_text(encoding="utf-8")
            de = (SHARED / "de.json").read_text(encoding="utf-8")
            script = (SHARED / "i18n.js").read_text(encoding="utf-8")
            return self.send(200, f"window.EBCLanguages={{en:{en},de:{de},sdEnabled:false}};\n{script}",
                             "text/javascript")
        for ext_id, _, _, html, script in EXTENSIONS:
            if url.path == f"/ui/{ext_id}/content":
                return self.send_file(html, "text/html; charset=utf-8")
            if url.path == f"/ui/{ext_id}/script":
                return self.send_file(script, "text/javascript")
        routes = {
            "/api/status": self.status,
            "/api/tester/status": self.tester_status,
            "/api/tester/settings": self.tester_settings,
            "/api/tester/catalog": lambda: self.send_json({"databaseReady": True, "batteries": [], "plans": []}),
            "/api/tester/resistance": self.resistance_status,
            "/api/tester/resistance/history": lambda: self.send_json([]),
            "/api/sd/status": self.sd_status,
            "/api/sd/list": lambda: self.send_json(
                {"path": parse_qs(url.query).get("path", [""])[0], "items": [], "nextOffset": 0, "hasMore": False}),
        }
        if url.path in routes:
            return routes[url.path]()
        return self.send(404, "Not found.")

    def status(self):
        state = self.device.state
        self.send_json({
            "version": "mock", "project": "EBCTester", "csrfToken": CSRF_TOKEN, "tabOrder": state["tabOrder"],
            "primaryTabs": "manual,auto,resistance,battery", "activityUrl": "/api/tester/status",
            "ssid": "mock", "apSsid": "BatteryTester-MOCK", "ip": "127.0.0.1", "networkMessage": "Wi-Fi connected.",
            "connected": True, "configuring": False, "timeReady": True, "otaConfigured": False,
            "otaInsecureHttp": False, "bootConfirmed": True, "otaReady": False, "otaBusy": False,
            "otaState": "idle", "otaMessage": "", "bytesRead": 0, "totalBytes": 0, "updateConfigured": False,
            "updateChecking": False, "updateAvailable": False, "updateStale": False, "availableVersion": "",
            "updateNotes": "", "updateMessage": "", "updateLastSuccess": 0, "updateLastSuccessUptime": 0,
            "tabs": [{"id": ext_id, "label": label, "contentUrl": f"/ui/{ext_id}/content",
                      "scriptUrl": f"/ui/{ext_id}/script", "placement": placement}
                     for ext_id, label, placement, _, _ in EXTENSIONS],
        })

    def tester_status(self):
        with self.device.lock:
            state = self.device.state
            state["frames"] += 1
            self.send_json({
                "controlsBusy": state["busy"],
                "link": {"connected": state["connected"], "hasFrame": state["connected"], "autoConnect": True,
                         "pending": False, "mode": state["mode"], "modeName": "C-CV stopped", "active": False,
                         "model": 10, "current": 0, "voltage": 3.3, "capacity": 0, "power": 0, "setpoint1": 0,
                         "setpoint2": 0, "setpoint3": 0, "framesOk": state["frames"], "framesBad": 0,
                         "ageMs": 200 if state["connected"] else -1},
                "plan": {"state": "idle", "step": 1, "steps": len(state["plan"]["steps"]), "cycle": 1,
                         "stepElapsed": 0, "message": "", "name": state["plan"]["name"]},
                "recorder": {"active": False, "failed": False, "runId": "", "written": 0, "pending": 0,
                             "dropped": 0, "error": ""},
                "battery": state["battery"]["label"], "databaseReady": True,
                "testerModel": state["model"], "maxCurrent": MAX_CURRENT[state["model"]],
            })

    def tester_settings(self):
        state = self.device.state
        self.send_json({"manual": state["manual"], "autoConnect": True, "testerModel": state["model"],
                        "maxCurrent": MAX_CURRENT[state["model"]], "battery": state["battery"],
                        "plan": state["plan"]})

    def resistance_status(self):
        self.send_json({"config": self.device.state["resistance"], "busy": False, "phase": "idle", "message": "",
                        "runId": "", "elapsed": 0, "removedAh": 0, "targetAh": 0, "baselineVoltage": 0,
                        "baselineCurrent": 0, "onsetUncertainty": 0, "results": []})

    def sd_status(self):
        state = self.device.state
        self.send_json({"mounted": True, "cardDetected": True, "cardDetectLevel": 1, "cardDetectGpio": 7,
                        "freeSpaceKnown": True, "totalBytes": 8 << 30, "freeBytes": 6 << 30,
                        "message": "SD card ready.", "busMode": state["busMode"],
                        "configuredBusMode": state["configuredBusMode"],
                        "restartRequired": state["busMode"] != state["configuredBusMode"],
                        "restartPending": False})

    # ---------- POST ----------

    def do_POST(self):
        url = urlparse(self.path)
        with self.device.lock:
            self.device.state["requests"].append(("POST", url.path))
        if self.headers.get("X-CSRF-Token") != CSRF_TOKEN:
            return self.send(403, "Invalid session token. Reload the page.")
        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length) or b"null")
        except (ValueError, json.JSONDecodeError):
            return self.send(400, "JSON request missing, too large or invalid.")
        if not isinstance(body, dict):
            return self.send(400, "JSON request missing, too large or invalid.")
        routes = {
            "/api/tester/model": self.post_model,
            "/api/tester/command": self.post_command,
            "/api/tester/plan": self.post_plan,
            "/api/tester/plan/action": self.post_plan_action,
            "/api/tester/resistance": self.post_resistance,
            "/api/tester/battery": self.post_battery,
            "/api/sd/bus": self.post_sd_bus,
        }
        if url.path not in routes:
            return self.send(404, "Not found.")
        with self.device.lock:
            return routes[url.path](body)

    def post_model(self, body):
        state = self.device.state
        model = body.get("model")
        if model not in MAX_CURRENT:
            return self.send(400, "Unknown tester model.")
        if state["busy"]:
            return self.send(409, "The tester model cannot be changed during an operation or recording.")
        state["model"] = model
        name = "EBC-A40L (max. 40 A)" if model == "A40L" else "EBC-A20 (max. 20 A)"
        return self.send(200, f"Tester model saved: {name}.")

    def post_command(self, body):
        state = self.device.state
        limit = MAX_CURRENT[state["model"]]
        action = body.get("action")
        if action in ("connect", "disconnect", "auto_connect"):
            return self.send(200, "OK (mock).")
        if action == "stop":
            return self.send(200, "Stop sent.")
        if action == "charge":
            if not (in_range(body.get("current"), 0.01, limit) and in_range(body.get("voltage"), 0.01, 30)
                    and in_range(body.get("cutoff"), 0.01, limit)):
                return self.send(409, limit_text(limit)["charge"])
            state["manual"].update(chargeCurrent=body["current"], chargeVoltage=body["voltage"],
                                   chargeCutoff=body["cutoff"])
            return self.send(200, "Starting charge (C-CV).")
        if action == "discharge_cc":
            if not (in_range(body.get("current"), 0.01, limit) and in_range(body.get("cutoff"), 0.01, 30)
                    and in_range(body.get("minutes"), 0, 239)):
                return self.send(409, limit_text(limit)["discharge"])
            state["manual"].update(dischargeCurrent=body["current"], dischargeCutoff=body["cutoff"],
                                   dischargeMinutes=body["minutes"])
            return self.send(200, "Starting discharge (D-CC).")
        if action == "discharge_cp":
            if not (in_range(body.get("power"), 1, 2000) and in_range(body.get("cutoff"), 0.01, 30)
                    and in_range(body.get("minutes"), 0, 239)):
                return self.send(409, "Power 1–2000 W, cutoff voltage 0.01–30 V, time 0–239 min.")
            return self.send(200, "Starting discharge (D-CP).")
        return self.send(400, "Unknown action.")

    def post_plan(self, body):
        steps = body.get("steps")
        if not isinstance(steps, list) or len(steps) > 10 or not all(
                isinstance(row, list) and len(row) == 4 and all(isinstance(v, int) for v in row) for row in steps):
            return self.send(400, "Invalid plan (name up to 64 characters, up to 10 steps).")
        error = check_plan(steps, MAX_CURRENT[self.device.state["model"]])
        if error:
            return self.send(409, error)
        self.device.state["plan"] = {"name": str(body.get("name", "")), "steps": steps}
        return self.send(200, "Plan saved.")

    def post_plan_action(self, body):
        if body.get("action") == "start":
            error = check_plan(self.device.state["plan"]["steps"], MAX_CURRENT[self.device.state["model"]])
            return self.send(409, error) if error else self.send(200, "Plan started.")
        return self.send(200, "OK (mock).")

    def post_resistance(self, body):
        state = self.device.state
        if body.get("action") == "abort":
            return self.send(200, "Measurement aborted by user.")
        config = body.get("config")
        if not isinstance(config, dict):
            return self.send(400, "Invalid request.")
        if body.get("action") == "save":
            state["resistance"].update(config)
            return self.send(200, "Measurement parameters saved.")
        limit = MAX_CURRENT[state["model"]]
        if config.get("pulseCurrent", 0) > limit or (
                config.get("automatic") and max(config.get("chargeCurrent", 0), config.get("prepCurrent", 0)) > limit):
            return self.send(409, f"Current exceeds the {state['model']} limit of {limit} A.")
        return self.send(200, "Measurement started.")

    def post_battery(self, body):
        self.device.state["battery"].update({k: v for k, v in body.items() if k in self.device.state["battery"]})
        return self.send(200, "Battery saved.")

    def post_sd_bus(self, body):
        state = self.device.state
        mode = body.get("mode")
        if mode not in ("spi", "sdmmc"):
            return self.send(400, "Unknown SD bus mode.")
        state["configuredBusMode"] = mode
        if mode == state["busMode"]:
            return self.send(200, "SD bus mode saved; it is already active.")
        if not body.get("restart"):
            return self.send(200, "SD bus mode saved. It applies after the next restart.")
        if state["busy"]:
            return self.send(409, "SD bus mode saved, but an operation or recording is active. "
                                  "Restart the device later to apply it.")
        # The simulated restart applies the mode immediately.
        state["busMode"] = mode
        state["restarts"] += 1
        return self.send(202, "SD bus mode saved. The device restarts now.")


def make_server(host="127.0.0.1", port=8080, device=None, quiet=True):
    handler = type("BoundHandler", (Handler,), {"device": device or MockDevice(), "quiet": quiet})
    return ThreadingHTTPServer((host, port), handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--model", choices=sorted(MAX_CURRENT), default="A20")
    parser.add_argument("--tab", default="manual", help="tab opened first, e.g. settings, manual, auto")
    args = parser.parse_args()
    device = MockDevice()
    device.reset(model=args.model, tabOrder=f"{args.tab},manual,auto,resistance,battery,overview,sd-card,"
                                            "updates,settings")
    server = make_server(port=args.port, device=device, quiet=False)
    print(f"Mock device UI: http://127.0.0.1:{args.port}  (Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
