# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

"""Local-only UI fixture; simulated selections/connections never access hardware or device storage."""
import json
import os
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import urlsplit, parse_qs

ROOT = Path(__file__).resolve().parents[1]
WEB = Path(os.environ.get("OTA_SUITE_DIR", ROOT / "external" / "ota-suite")) / "components" / "web_interface" / "assets"
SD = Path(os.environ.get("SD_CARD_MANAGER_DIR", ROOT / "external" / "sd-card-manager")) / "components" / "sd_card_manager" / "assets"
EXTENSIONS = {
    "dashboard": (ROOT / "components" / "battery_dashboard" / "assets", "dashboard", "Messdaten"),
    "battery": (ROOT / "components" / "test_control" / "assets", "battery", "Batterie"),
    "manual": (ROOT / "components" / "test_control" / "assets", "manual", "Manuell"),
    "auto": (ROOT / "components" / "test_control" / "assets", "auto", "Automatik"),
    "resistance": (ROOT / "components" / "test_control" / "assets", "resistance", "Innenwiderstand"),
    "sd-card": (SD, "sd_card", "SD Karte"),
    "mariadb": (ROOT / "components" / "mariadb_sync" / "assets", "mariadb", "MariaDB-Sicherung"),
    "languages": (ROOT / "components" / "ui_localization" / "assets", "languages", "Language"),
    "tester": (ROOT / "components" / "test_control" / "assets", "tester", "EBC tester"),
}
CONFIG = dict(automatic=False, capacityAh=20, targetSoc=50, chargeCurrent=.5, chargeVoltage=3.45,
              chargeCutoff=.1, prepCurrent=.5, cutoffVoltage=2.5, maxVoltage=3.6, maxPower=50,
              restSeconds=60, baselineSeconds=3, idleCurrent=.02, pulseCurrent=.5, pulseSeconds=12,
              time1=1, time2=5, time3=10, timingTolerance=1.5, maxGap=3, maxChargeSeconds=86400,
              maxPrepSeconds=86400, minVoltageDrop=.001)
USER_BATTERY = "Laden Batterie ä <b>user</b>"
USER_PLAN = "Warten Plan ä"
USER_NOTES = "<img src=x onerror=alert(1)> Notizen Benutzer"
TESTER = dict(model=0, connected=False, error="")
MODELS = {
    9: ("EBC-A20", dict(chargeVoltage=18, chargeCurrent=5, dischargeVoltage=30, dischargeCurrent=20, power=85)),
    16: ("EBC-A40L", dict(chargeVoltage=5, chargeCurrent=40, dischargeVoltage=5, dischargeCurrent=40, power=200)),
    10: ("EBC-A50L", dict(chargeVoltage=5, chargeCurrent=50, dischargeVoltage=5, dischargeCurrent=50, power=250)),
}


class Handler(BaseHTTPRequestHandler):
    def respond(self, body, content_type="application/json; charset=utf-8", status=200):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False)
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Security-Policy",
                         "default-src 'self'; script-src 'self'; style-src 'self'; "
                         "connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; "
                         "base-uri 'none'; form-action 'self'")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlsplit(self.path)
        path = url.path
        if path == "/i18n.js":
            body = ("window.EBCLanguages={en:" + (WEB / "en.json").read_text(encoding="utf-8")
                    + ",de:" + (WEB / "de.json").read_text(encoding="utf-8")
                    + (",loginPage:true,sdEnabled:false};\n" if url.query == "login=1" else ",sdEnabled:true};\n")
                    + (WEB / "i18n.js").read_text(encoding="utf-8"))
            return self.respond(body, "application/javascript; charset=utf-8")
        base_assets = {"/": "index.html", "/app.js": "app.js", "/app.css": "app.css",
                       "/bootstrap.min.css": "bootstrap.min.css", "/login": "login.html", "/login.js": "login.js"}
        if path in base_assets:
            file = WEB / base_assets[path]
            mime = {".html": "text/html", ".js": "application/javascript", ".css": "text/css"}[file.suffix]
            return self.respond(file.read_bytes(), mime + "; charset=utf-8")
        if path == "/ui/dashboard/chart.js":
            return self.respond((EXTENSIONS["dashboard"][0] / "chart.umd.js").read_bytes(),
                                "application/javascript")
        if path == "/ui/tester/validation.js":
            return self.respond((EXTENSIONS["manual"][0] / "validation.js").read_bytes(),
                                "application/javascript")
        parts = path.split("/")
        if len(parts) == 4 and parts[1] == "ui" and parts[2] in EXTENSIONS:
            directory, basename, _ = EXTENSIONS[parts[2]]
            suffix = {"content": ".html", "script": ".js"}.get(parts[3])
            if suffix:
                return self.respond((directory / (basename + suffix)).read_bytes(),
                                    "text/html" if suffix == ".html" else "application/javascript")
        if path == "/api/status":
            tabs = [dict(id=id, label=item[2], contentUrl=f"/ui/{id}/content", scriptUrl=f"/ui/{id}/script",
                         placement="settings" if id in ("mariadb", "languages", "tester") else "tab")
                    for id, item in EXTENSIONS.items()]
            return self.respond(dict(project="EBCTester", version="fixture", csrfToken="test",
                                     sessionDays=1, settingsPasswordRequired=True,
                                     connected=True, ssid="User WLAN", ip="127.0.0.1", apSsid="Fixture",
                                     timeReady=True, networkMessage="Wi-Fi connected.", updateMessage="No newer firmware available.",
                                     updateAvailable=False, updateStale=False, updateNotes="", otaMessage="Ready. OTA starts only after clicking Start update.",
                                     otaState="idle", otaInsecureHttp=True, otaConfigured=True, bootConfirmed=True,
                                     otaBusy=False, otaReady=True, updateConfigured=True, bytesRead=1234, totalBytes=3000,
                                     primaryTabs="dashboard,battery,manual,auto,resistance",
                                     tabOrder="dashboard,battery,manual,auto,resistance,overview,sd-card,updates,settings",
                                     activityUrl="/api/tester/status", tabs=tabs))
        if path == "/api/i18n/languages":
            return self.respond(dict(sdAvailable=True, languages=[dict(code="fr", name="Français")], errors=[]))
        if path == "/api/i18n/language":
            en = json.loads((WEB / "en.json").read_text(encoding="utf-8"))
            key = next(key for key, value in en["strings"].items() if value == "Battery")
            return self.respond(dict(version=1, code=parse_qs(url.query)["code"][0], name="Français",
                                     strings={key: "Batterie spéciale"}))
        if path == "/api/tester/status":
            model_name, limits = MODELS.get(TESTER["model"],
                ("Unknown", dict(chargeVoltage=5, chargeCurrent=5, dischargeVoltage=5, dischargeCurrent=20, power=85)))
            return self.respond(dict(databaseReady=True, controlsBusy=False, battery=USER_BATTERY,
                                     link=dict(connected=TESTER["connected"], hasFrame=TESTER["connected"], active=False, pending=False,
                                               mode=0, modeName="D-CC stopped", voltage=3.3, current=0, power=0,
                                               capacity=0, ageMs=300, framesOk=10, framesBad=0, model=TESTER["model"],
                                               hasModel=TESTER["model"] != 0, modelKnown=TESTER["model"] in MODELS,
                                               modelName=model_name, limits=limits, error=TESTER["error"],
                                               detectedModel=9, hasDetectedModel=bool(TESTER["error"]) or TESTER["connected"],
                                               detectedModelName="EBC-A20", detectedModelKnown=True,
                                               detectedModelEnabled=True, detectedVersionKnown=False,
                                               detectedVersionPending=False, detectedVersion=0,
                                               controlEnabled=TESTER["connected"],
                                               modelMismatch=bool(TESTER["error"]),
                                               setpoint1=.5, setpoint2=2.5, setpoint3=0),
                                     recorder=dict(active=False, written=0),
                                     plan=dict(state="idle", name=USER_PLAN, message="Plan completed successfully.")))
        if path == "/api/tester/settings":
            return self.respond(dict(model=TESTER["model"], battery=dict(label=USER_BATTERY, manufacturer="User", notes=USER_NOTES),
                                     plan=dict(name=USER_PLAN, steps=[]), manual={}))
        if path == "/api/tester/catalog":
            return self.respond(dict(databaseReady=True, batteries=[USER_BATTERY], plans=[USER_PLAN]))
        if path == "/api/tester/resistance":
            return self.respond(dict(config=CONFIG, busy=False, phase="idle", elapsed=0, removedAh=0, targetAh=10,
                                     baselineVoltage=0, baselineCurrent=0, onsetUncertainty=0, results=[],
                                     message="Temperature unknown; target SOC is calculated. Four-wire measurement."))
        if path == "/api/tester/resistance/history":
            return self.respond([])
        if path == "/api/mariadb/status":
            return self.respond(dict(settings=dict(host="database.example.org", port=3306, database="test",
                                                  user="user", enabled=False, intervalMinutes=15),
                                     state="idle", message="Everything up to date — no new data",
                                     networkConnected=True, databaseReady=True, busy=False))
        if path == "/api/sd/status":
            return self.respond(dict(mounted=True, cardDetected=True, freeSpaceKnown=True,
                                     totalBytes=10000000, freeBytes=9000000, message="SD card ready."))
        if path == "/api/sd/list":
            return self.respond(dict(path="/", items=[], nextOffset=0, hasMore=False))
        if path == "/api/db/runs":
            return self.respond(dict(batteries=[dict(id=1, label=USER_BATTERY, nominalAh=20)], runs=[]))
        return self.respond("Fixture route not found.", "text/plain", 404)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.path == "/api/tester/model":
            model = json.loads(body).get("model")
            if model not in MODELS:
                return self.respond("Unsupported tester model.", "text/plain", 400)
            TESTER.update(model=model, connected=False, error="")
            return self.respond("Tester model saved and applied.", "text/plain")
        if self.path == "/api/tester/command":
            action = json.loads(body).get("action")
            if action == "connect":
                if TESTER["model"] == 0:
                    return self.respond("Select and save a tester model in Settings first.", "text/plain", 409)
                if TESTER["model"] != 9:
                    TESTER.update(connected=False, error="Tester mismatch: selected EBC-A40L, received EBC-A20. "
                                  "Connection closed. Select the correct tester in Settings and reconnect.")
                else:
                    TESTER.update(connected=True, error="")
                return self.respond("Connection request sent.", "text/plain")
            if action == "disconnect":
                TESTER["connected"] = False
                return self.respond("Disconnect sent.", "text/plain")
        self.respond("Invalid values.", "text/plain; charset=utf-8", 400)


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", 8766), Handler).serve_forever()
