# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0
"""Screenshot fixture for the EBCTester V2 manuals.

Serves the real web assets with simulated device data. It never talks to
hardware. Usage: python fixture.py <asset-root> <port> <mode>
mode: factory | maker-inactive | maker-active | login
"""
import json
import math
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit, parse_qs

ROOT = Path(sys.argv[1])
PORT = int(sys.argv[2])
MODE = sys.argv[3]
ACTIVE = MODE in ("factory", "maker-active", "login")
MAKER = MODE.startswith("maker")
WEB = ROOT / "external/ota-suite/components/web_interface/assets"
SD = ROOT / "external/sd-card-manager/components/sd_card_manager/assets"
C = ROOT / "components"
EXT = {
    "dashboard": (C / "battery_dashboard/assets", "dashboard", "Measurements", "tab"),
    "battery": (C / "test_control/assets", "battery", "Battery", "tab"),
    "manual": (C / "test_control/assets", "manual", "Manual", "tab"),
    "auto": (C / "test_control/assets", "auto", "Automatic", "tab"),
    "resistance": (C / "test_control/assets", "resistance", "Internal resistance", "tab"),
    "test-runs": (C / "battery_dashboard/assets", "test_runs", "Test run management", "tab"),
    "sd-card": (SD, "sd_card", "SD card", "tab"),
    "tester": (C / "test_control/assets", "tester", "EBC tester", "settings"),
    "mariadb": (C / "mariadb_sync/assets", "mariadb", "MariaDB backup", "settings"),
    "mqtt": (C / "ha_mqtt/assets", "mqtt", "Home Assistant MQTT", "settings"),
    "debug-settings": (C / "debug_log/assets", "debug_settings", "Debug log", "settings"),
    "debug": (C / "debug_log/assets", "debug", "Debug", "tab"),
    "languages": (C / "ui_localization/assets", "languages", "Language", "settings"),
    "config-reset": (ROOT / "main/assets", "config_reset", "Device reset", "settings"),
    "maker-settings": (ROOT / "main/assets", "maker_settings", "Maker Settings", "tab"),
    "led-settings": (ROOT / "main/assets", "led_settings", "LEDs", "settings"),
}
# Registration order follows main.c: debug and config UI first, then the peripheral services.
ORDER = ["debug-settings", "debug", "config-reset", "maker-settings", "led-settings", "sd-card", "dashboard", "test-runs",
         "mariadb", "mqtt", "battery", "manual", "auto", "resistance", "tester", "languages"]
INACTIVE_OK = {"debug-settings", "debug", "config-reset", "maker-settings", "led-settings"}

BATTERY = "LFP-50Ah #01"
PLAN = "LFP 50 Ah capacity test"
LIMITS = dict(chargeVoltage=18, chargeCurrent=5, dischargeVoltage=30, dischargeCurrent=20, power=85)
STEPS = [[3, 500, 365, 50], [4, 0, 0, 30], [1, 2000, 250, 0], [4, 0, 0, 30], [3, 500, 340, 50]]
T0 = 1793736000000  # 2026-11-03 20:00 UTC (start of the example run)
CLOCK_UTC = 1793780400 + 1325  # device clock in the screenshots


def lfp_ocv(soc):
    """Rough LiFePO4 open-circuit voltage curve (soc 0..1)."""
    if soc < 0.05:
        return 2.80 + 8.0 * soc
    if soc < 0.15:
        return 3.20 + 0.3 * (soc - 0.05)
    if soc < 0.90:
        return 3.23 + 0.12 * (soc - 0.15)
    return 3.32 + 0.6 * (soc - 0.90)


def build_run():
    """Generates segments and averaged measurements every 20 s for the example plan run."""
    dt = 20.0
    points, segs = [], []
    t = 0.0
    soc = 0.38
    cap_ah = 50.6

    def add_seg(n, mode, step, sp, duration_h, fn):
        nonlocal t
        start = t
        seg_cap = 0.0
        samples = 0
        steps = int(duration_h * 3600 / dt)
        for k in range(steps):
            frac = k / max(1, steps - 1)
            current, voltage, dcap = fn(frac)
            seg_cap += dcap
            ms = int(t * 1000)
            points.append([n, ms + 41000, T0 + ms, round(current, 3), round(voltage, 4), round(seg_cap, 4)])
            samples += 1
            t += dt
        segs.append(dict(n=n, mode=mode, step=step, cycle=1, sp1=sp[0], sp2=sp[1], sp3=sp[2],
                         samples=samples, capacityAh=round(seg_cap, 3), startMs=T0 + int(start * 1000),
                         endMs=T0 + int(t * 1000), startUptime=int(start * 1000) + 41000,
                         endUptime=int(t * 1000) + 41000))

    # S1 charge CC/CV to 3.65 V.
    def s1(frac):
        nonlocal soc
        cc = 0.86
        if frac < cc:
            i = 5.0
        else:
            i = 5.0 * math.exp(-(frac - cc) / (1 - cc) * math.log(10))
        soc = min(1.0, soc + i * dt / 3600 / cap_ah)
        base = lfp_ocv(soc) + 0.045
        if frac >= cc:
            v = 3.65
        elif frac > 0.74:
            v = base + (3.65 - base) * ((frac - 0.74) / (cc - 0.74)) ** 2.2
        else:
            v = base
        return i, v, i * dt / 3600
    add_seg(1, "C-CV", 1, (5.00, 3.65, 0.50), 6.8, s1)

    def wait_from(v_start, v_end):
        def fn(frac):
            v = v_end + (v_start - v_end) * math.exp(-frac * 5)
            return 0.0, v, 0.0
        return fn
    add_seg(2, "WAIT", 2, (0, 0, 30), 0.5, wait_from(3.62, 3.37))

    # S3 discharge 20 A to 2.5 V.
    def s3(frac):
        nonlocal soc
        soc = max(0.0, soc - 20 * dt / 3600 / cap_ah)
        v = lfp_ocv(soc) - 0.062 - 0.03 * (1 - frac)
        if soc < 0.035:
            v = 2.5 + (soc / 0.035) * (lfp_ocv(0.035) - 0.062 - 2.5)
        return 20.0, max(2.5, v), 20 * dt / 3600
    add_seg(3, "D-CC", 3, (20.00, 2.50, 0.00), 2.52, s3)
    add_seg(4, "WAIT", 4, (0, 0, 30), 0.5, wait_from(2.52, 3.02))

    # S5 charge to storage level, still running.
    def s5(frac):
        nonlocal soc
        soc = min(1.0, soc + 5 * dt / 3600 / cap_ah)
        return 5.0, lfp_ocv(soc) + 0.045, 5 * dt / 3600
    add_seg(5, "C-CV", 5, (5.00, 3.40, 0.50), 1.32, s5)
    return segs, points


SEGMENTS, POINTS = build_run()
LAST = POINTS[-1]

RUNS = [
    dict(id="r-0009", batteryId=1, battery=BATTERY, plan=PLAN, status="RUNNING", started="2026-11-03 20:00:00", name="", protected=True),
    dict(id="r-0008", batteryId=1, battery=BATTERY, plan="DCIR 50 % SOC", status="COMPLETED", started="2026-11-02 18:42:10", name="LFP-50Ah #01 DCIR 50 %"),
    dict(id="r-0007", batteryId=2, battery="LFP-50Ah #02", plan=PLAN, status="COMPLETED", started="2026-11-01 07:15:42", name=""),
    dict(id="r-0006", batteryId=2, battery="LFP-50Ah #02", plan="", status="STOPPED", started="2026-10-31 21:03:05", name="Manual discharge 10 A"),
    dict(id="r-0005", batteryId=3, battery="NMC-18650 #12", plan="NMC 18650 capacity", status="COMPLETED", started="2026-10-30 09:30:00", name=""),
    dict(id="r-0004", batteryId=3, battery="NMC-18650 #12", plan="NMC 18650 capacity", status="INTERRUPTED", started="2026-10-29 16:20:11", name=""),
    dict(id="r-0003", batteryId=1, battery=BATTERY, plan=PLAN, status="COMPLETED", started="2026-10-27 08:00:00", name="Initial capacity check"),
]
BATTERIES = [dict(id=1, label=BATTERY, nominalAh=50), dict(id=2, label="LFP-50Ah #02", nominalAh=50),
             dict(id=3, label="NMC-18650 #12", nominalAh=3.0)]

DCIR_CONFIG = dict(automatic=True, capacityAh=50, targetSoc=50, chargeCurrent=5, chargeVoltage=3.65,
                   chargeCutoff=.5, prepCurrent=10, cutoffVoltage=2.5, maxVoltage=3.65, maxPower=80,
                   restSeconds=60, baselineSeconds=3, idleCurrent=.02, pulseCurrent=20, pulseSeconds=12,
                   time1=1, time2=5, time3=10, timingTolerance=1.5, maxGap=3, maxChargeSeconds=86400,
                   maxPrepSeconds=86400, minVoltageDrop=.001)
DCIR_RESULTS = [
    dict(requested=1, actual=1.02, captured=True, voltage=3.2262, current=19.98, valid=True, milliohms=0.86, epochMs=1793652157000, capturedUtc="2026-11-02 21:22:37"),
    dict(requested=5, actual=5.01, captured=True, voltage=3.2188, current=19.99, valid=True, milliohms=1.23, epochMs=1793652161000, capturedUtc="2026-11-02 21:22:41"),
    dict(requested=10, actual=10.03, captured=True, voltage=3.2141, current=20.00, valid=True, milliohms=1.46, epochMs=1793652166000, capturedUtc="2026-11-02 21:22:46"),
]
SD_ITEMS = [dict(name="languages", directory=True), dict(name="exports", directory=True),
            dict(name="ebc_a20.db", directory=False, size=14_811_136),
            dict(name="notes.txt", directory=False, size=412)]
DEBUG_LINES = [
    "I (412) ebc_config: Configuration validation: valid",
    "I (413) ebc_config: Firmware mode: Factory; configuration active: 1",
    "I (958) network: Connected to Wi-Fi \"Workshop\" (IP 192.168.1.47)",
    "I (436) rtc_clock: DS3231 time valid: 2026-11-04 08:11:58 UTC",
    "I (2140) sntp: Time synchronized: 2026-11-04 08:12:05 UTC",
    "I (2141) rtc_clock: RTC updated from NTP",
    "I (2381) sd_card: SD card mounted (SDMMC 4-bit, 10 MHz), 29.7 GB free",
    "I (2433) battery_db: Database ready, schema 3",
    "I (5120) ebc_link: Tester connected: EBC-A20 (ident 0x09)",
    "I (5122) test_control: Plan 'LFP 50 Ah capacity test' running, step 5/5",
    "I (25130) test_store: Measurement 2106 stored (U=3.268 V, I=5.00 A, C=6.583 Ah)",
    "I (45136) test_store: Measurement 2107 stored (U=3.268 V, I=5.00 A, C=6.611 Ah)",
    "I (65141) test_store: Measurement 2108 stored (U=3.269 V, I=5.00 A, C=6.639 Ah)",
    "I (85147) test_store: Measurement 2109 stored (U=3.269 V, I=5.00 A, C=6.667 Ah)",
]
FRAMES = {"n": 15420}
STATE = {"idle": False}


def maker_fields():
    # Field list mirrors main/ebc_config_web.cpp; values are the board revision defaults.
    f = []
    def add(section, name, label, type_, value=None, mn=0, mx=0, ml=0, configured=None):
        item = dict(section=section, name=name, label=label, type=type_, min=mn, max=mx, maxLength=ml)
        if type_ == "password":
            item["configured"] = configured
        else:
            item["value"] = value
        f.append(item)
    add("tester", "tester_tx_gpio", "Tester UART TX GPIO", "number", 10, -1, 48, 0)
    add("tester", "tester_rx_gpio", "Tester UART RX GPIO", "number", 11, -1, 48, 0)
    add("tester", "tester_baud", "Tester UART baud rate", "number", 9600, 1200, 115200, 3)
    add("tester", "dongle_tx_gpio", "Dongle UART TX GPIO", "number", 8, -1, 48, 0)
    add("tester", "dongle_rx_gpio", "Dongle UART RX GPIO", "number", 9, -1, 48, 0)
    add("tester", "dongle_baud", "Dongle UART baud rate", "number", 9600, 1200, 115200, 3)
    add("tester", "averaging_length", "Frames per stored average", "number", 20, 1, 120, 1)
    add("tester", "device_id", "Device ID", "text", "EBC-A20", ml=64)
    add("tester", "mqtt_enabled", "Enable Home Assistant MQTT", "checkbox", False, 0, 1)
    add("ota", "ap_password", "Access point password", "password", ml=63, configured=True)
    add("ota", "sntp_server", "SNTP server", "text", "pool.ntp.org", ml=127)
    add("ota", "manifest_url", "Firmware manifest URL", "text", "https://ebctester.gemelon.de/firmware.json", ml=1024)
    add("ota", "allow_http", "Allow unencrypted HTTP firmware updates", "checkbox", False, 0, 1)
    add("ota", "status_led_enabled", "Enable status LED", "checkbox", True, 0, 1)
    add("ota", "led_gpio", "Status LED GPIO", "number", 48, -1, 48)
    add("ota", "web_password", "Web password", "password", ml=63, configured=True)
    add("ota", "settings_require_password", "Confirm settings changes with password", "checkbox", True, 0, 1)
    add("ota", "sd_languages", "Load additional languages from SD", "checkbox", True, 0, 1)
    add("ota", "primary_tabs", "Primary tabs (comma-separated)", "text", "dashboard,battery,manual,auto,resistance", ml=255)
    add("ota", "tab_order", "Tab order (comma-separated)", "text",
        "dashboard,battery,manual,auto,resistance,overview,test-runs,sd-card,updates,settings,debug", ml=255)
    add("sd", "bus", "SD bus", "bus", 0, 0, 1)
    add("sd", "sdmmc_freq_khz", "SDMMC frequency (kHz)", "number", 10000, 400, 20000)
    add("sd", "spi_freq_khz", "SPI frequency (kHz)", "number", 10000, 400, 20000)
    add("sd", "card_detect_gpio", "Card-detect GPIO (-1: not connected)", "number", -1, -1, 48)
    add("sd", "mount_point", "SD mount point", "text", "/sdcard", ml=63)
    for name, label, value in [("clk_gpio", "SDMMC CLK GPIO", 6), ("cmd_gpio", "SDMMC CMD GPIO", 7),
                               ("d0_gpio", "SDMMC D0 GPIO", 5), ("d1_gpio", "SDMMC D1 GPIO", 4),
                               ("d2_gpio", "SDMMC D2 GPIO", 18), ("d3_gpio", "SDMMC D3 GPIO", 17),
                               ("spi_miso_gpio", "SPI MISO GPIO", -1), ("spi_mosi_gpio", "SPI MOSI GPIO", -1),
                               ("spi_cs_gpio", "SPI CS GPIO", -1), ("spi_clk_gpio", "SPI CLK GPIO", -1)]:
        add("sd", name, label, "number", value, -1, 48)
    add("rtc", "enabled", "Enable DS3231 RTC", "checkbox", MODE == "maker-active", 0, 1)
    add("rtc", "int_gpio", "RTC INT GPIO (-1: not connected; reserved input)", "number", 14, -1, 48)
    add("rtc", "sda_gpio", "I2C SDA GPIO", "number", 15, -1, 48)
    add("rtc", "scl_gpio", "I2C SCL GPIO", "number", 16, -1, 48)
    return f


def tester_status():
    FRAMES["n"] += 1
    if STATE["idle"]:
        return dict(
            databaseReady=True, controlsBusy=False, battery=BATTERY,
            link=dict(connected=True, hasFrame=True, active=False, pending=False, autoConnect=True,
                      mode=0x16, modeName="C-CV completed", voltage=3.3371, current=0.0, power=0.0,
                      capacity=0.0, ageMs=380, framesOk=FRAMES["n"], framesBad=3, model=9, hasModel=True,
                      modelKnown=True, modelName="EBC-A20", limits=LIMITS, error="", detectedModel=9,
                      hasDetectedModel=True, detectedModelName="EBC-A20", detectedModelKnown=True,
                      detectedModelEnabled=True, detectedVersionKnown=True, detectedVersionPending=False,
                      detectedVersion=311, controlEnabled=True, modelMismatch=False,
                      setpoint1=5.0, setpoint2=3.65, setpoint3=0.50, setpoint=[5.0, 3.65, 0.50]),
            recorder=dict(active=False, written=0, pending=0, dropped=0, failed=False, error=""),
            plan=dict(state="idle", name=PLAN, step=0, steps=5, cycle=0, stepElapsed=0, message="Ready."))
    return dict(
        databaseReady=True, controlsBusy=True, battery=BATTERY,
        link=dict(connected=True, hasFrame=True, active=True, pending=False, autoConnect=True,
                  mode=0x0C, modeName="C-CV active", voltage=LAST[4], current=5.0, power=round(5 * LAST[4], 2),
                  capacity=LAST[5], ageMs=420, framesOk=FRAMES["n"], framesBad=3, model=9, hasModel=True,
                  modelKnown=True, modelName="EBC-A20", limits=LIMITS, error="", detectedModel=9,
                  hasDetectedModel=True, detectedModelName="EBC-A20", detectedModelKnown=True,
                  detectedModelEnabled=True, detectedVersionKnown=True, detectedVersionPending=False,
                  detectedVersion=311, controlEnabled=True, modelMismatch=False,
                  setpoint1=5.0, setpoint2=3.40, setpoint3=0.50, setpoint=[5.0, 3.40, 0.50]),
        recorder=dict(active=True, written=len(POINTS), pending=0, dropped=0, failed=False, error=""),
        plan=dict(state="running", name=PLAN, step=5, steps=5, cycle=1, stepElapsed=4752,
                  message="Step 5: charging (C-CV) until 0.50 A."))


def status():
    tabs = []
    for tab_id in ORDER:
        if not ACTIVE and tab_id not in INACTIVE_OK:
            continue
        if tab_id == "maker-settings" and not MAKER:
            continue
        d = EXT[tab_id]
        tabs.append(dict(id=tab_id, label=d[2], contentUrl=f"/ui/{tab_id}/content",
                         scriptUrl=f"/ui/{tab_id}/script", placement=d[3]))
    order = "dashboard,battery,manual,auto,resistance,overview,test-runs,sd-card,updates,settings,debug"
    connected = ACTIVE
    return dict(
        project="EBCTester", version="2.0.0", csrfToken="fixture", sessionDays=1, loginEnabled=True,
        settingsPasswordRequired=True,
        tabOrder=order if ACTIVE or not MAKER else "maker-settings,settings,overview",
        primaryTabs="dashboard,battery,manual,auto,resistance" if ACTIVE else "",
        activityUrl="/api/tester/status" if ACTIVE else "",
        configActive=ACTIVE, firmwareMode="maker" if MAKER else "factory",
        ssid="Workshop" if connected else "", apSsid="BatteryTester-3A7F21", ip="192.168.1.47" if connected else "",
        networkMessage="Wi-Fi connected." if connected else "Maker setup: access point only. Activate configuration to enable Wi-Fi station.",
        connected=connected, configuring=False, timeReady=connected, otaConfigured=True, otaInsecureHttp=False,
        bootConfirmed=True, otaReady=True, otaBusy=MODE == "ota", otaState="downloading" if MODE == "ota" else "idle",
        otaMessage="Ready. OTA starts only after clicking Start update.", bytesRead=0, totalBytes=0,
        updateConfigured=True, updateChecking=False, updateAvailable=True, updateStale=False,
        availableVersion="2.0.1", updateNotes="Example release notes: bug fixes and improvements.",
        updateMessage="Firmware 2.0.1 is available.", updateLastSuccess=1793780400, updateLastSuccessUptime=0,
        clockEnabled=ACTIVE, clockUtc=CLOCK_UTC if ACTIVE else 0,
        tabs=tabs)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def respond(self, body, ctype="application/json; charset=utf-8", code=200):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False)
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlsplit(self.path)
        p, q = url.path, parse_qs(url.query)
        if p == "/i18n.js":
            sd = ACTIVE and url.query != "login=1"
            body = ("window.EBCLanguages={en:" + (WEB / "en.json").read_text("utf-8") + ",de:" +
                    (WEB / "de.json").read_text("utf-8") +
                    (",loginPage:true,sdEnabled:false};\n" if url.query == "login=1" else
                     f",sdEnabled:{'true' if sd else 'false'}}};\n") + (WEB / "i18n.js").read_text("utf-8"))
            return self.respond(body, "application/javascript; charset=utf-8")
        base = {"/": "index.html", "/app.js": "app.js", "/app.css": "app.css", "/bootstrap.min.css": "bootstrap.min.css",
                "/login": "login.html", "/login.js": "login.js"}
        if p in base:
            f = WEB / base[p]
            mime = {".html": "text/html", ".js": "application/javascript", ".css": "text/css"}[f.suffix]
            return self.respond(f.read_bytes(), mime + "; charset=utf-8")
        if p == "/ui/dashboard/chart.js":
            return self.respond((C / "battery_dashboard/assets/chart.umd.js").read_bytes(), "application/javascript")
        if p in ("/ui/dashboard/brand-logo.png", "/ui/dashboard/brand-icon.png"):
            name = "brand_logo.png" if "logo" in p else "brand_icon.png"
            return self.respond((C / "battery_dashboard/assets" / name).read_bytes(), "image/png")
        if p == "/ui/tester/validation.js":
            return self.respond((C / "test_control/assets/validation.js").read_bytes(), "application/javascript")
        parts = p.split("/")
        if len(parts) == 4 and parts[1] == "ui" and parts[2] in EXT:
            d, base_name, _, _ = EXT[parts[2]]
            suffix = {"content": ".html", "script": ".js"}.get(parts[3])
            if suffix:
                return self.respond((d / (base_name + suffix)).read_bytes(),
                                    "text/html" if suffix == ".html" else "application/javascript")
        if p == "/fixture/idle":
            STATE["idle"] = q.get("on", ["1"])[0] == "1"
            return self.respond("ok", "text/plain")
        if p == "/api/status":
            return self.respond(status())
        if p == "/api/i18n/languages":
            return self.respond(dict(sdAvailable=True, languages=[dict(code="fil", name="Filipino")], errors=[]))
        if p == "/api/tester/status":
            return self.respond(tester_status())
        if p == "/api/tester/settings":
            return self.respond(dict(model=9, battery=dict(label=BATTERY, manufacturer="EVE", serial="LF50K-2311-0457",
                                                           date="2025-11-14", capacity=50,
                                                           notes="Prismatic cell, 4-wire test fixture B."),
                                     plan=dict(name=PLAN, steps=STEPS),
                                     manual=dict(chargeCurrent=5, chargeVoltage=3.65, chargeCutoff=.5,
                                                 dischargeCurrent=20, dischargeCutoff=2.5, dischargeMinutes=0,
                                                 power=50, powerCutoff=2.5, powerMinutes=0)))
        if p == "/api/tester/catalog":
            return self.respond(dict(databaseReady=True, batteries=[b["label"] for b in BATTERIES],
                                     plans=[PLAN, "NMC 18650 capacity", "DCIR 50 % SOC"]))
        if p == "/api/tester/resistance":
            return self.respond(dict(config=DCIR_CONFIG, busy=False, phase="complete", elapsed=73.4, removedAh=25.3,
                                     targetAh=25.3, baselineVoltage=3.2434, baselineCurrent=0.0, onsetUncertainty=0.42,
                                     results=DCIR_RESULTS, runId="r-0008",
                                     message="Measurement complete. Temperature unknown; target SOC is calculated."))
        if p == "/api/tester/resistance/history":
            return self.respond([dict(runId="r-0008", battery=BATTERY, started="2026-11-02 18:42:10", status="COMPLETED",
                                      notes="automatic preparation, target SOC 50 %, pulse 20 A / 12 s", results=DCIR_RESULTS),
                                 dict(runId="r-0002", battery="LFP-50Ah #02", started="2026-10-25 10:05:33", status="COMPLETED",
                                      notes="prepared SOC 50 %, pulse 20 A / 12 s", results=DCIR_RESULTS)])
        if p == "/api/db/runs":
            runs = [dict(id=r["id"], batteryId=r["batteryId"], plan=r["plan"], status=r["status"],
                         started=r["started"], name=r["name"]) for r in RUNS]
            return self.respond(dict(batteries=BATTERIES, runs=runs))
        if p == "/api/db/run":
            return self.respond(dict(
                run=dict(id="r-0009", batteryLabel=BATTERY, manufacturer="EVE", serial="LF50K-2311-0457", nominalAh=50,
                         status="RUNNING", currentStep=5, cyclePass=1, name="", plan=PLAN, started="2026-11-03 20:00:00",
                         ended=None),
                last=dict(current=LAST[3], voltage=LAST[4], capacity=LAST[5]), segments=SEGMENTS))
        if p == "/api/db/measurements":
            mx = int(q.get("max", ["1500"])[0])
            stride = max(1, math.ceil(len(POINTS) / mx))
            pts = [pt for i, pt in enumerate(POINTS) if i % stride == 0]
            return self.respond(dict(points=pts, total=len(POINTS), stride=stride))
        if p == "/api/test-runs":
            runs = [dict(id=r["id"], name=r["name"], battery=r["battery"], started=r["started"], plan=r["plan"],
                         status=r["status"], protected=r.get("protected", False)) for r in RUNS]
            return self.respond(dict(runs=runs, batteries=BATTERIES, total=len(runs), pageSize=50))
        if p == "/api/mariadb/status":
            return self.respond(dict(settings=dict(host="nas.local", port=3306, database="ebc_a20", user="ebctester",
                                                  enabled=True, intervalMinutes=60, passwordSet=True),
                                     state="idle", message="Everything up to date — no new data", networkConnected=True,
                                     databaseReady=True, busy=False, lastAttempt="2026-11-04 08:00:12",
                                     lastSuccess="2026-11-04 08:00:14", lastRuns=1, lastMeasurements=180,
                                     pendingRuns=0, serverVersion="10.11.6-MariaDB"))
        if p == "/api/mqtt/status":
            return self.respond(dict(settings=dict(enabled=True, host="homeassistant.local", port=1883, username="ebctester",
                                                  passwordSet=True, topicPrefix="ebctester", intervalSeconds=20,
                                                  allowRemoteStart=False),
                                     state="connected", message="Connected to homeassistant.local:1883"))
        if p == "/api/debug/status":
            return self.respond(dict(setting=True, enabled=True))
        if p == "/api/debug/log":
            lines = [dict(seq=i + 1, text=t) for i, t in enumerate(DEBUG_LINES)]
            after = int(q.get("after", ["0"])[0])
            lines = [line for line in lines if line["seq"] > after]
            return self.respond(dict(enabled=True, capacity=500, count=len(DEBUG_LINES), dropped=0, firstSeq=1,
                                     nextSeq=len(DEBUG_LINES) + 1, lines=lines))
        if p == "/api/sd/status":
            return self.respond(dict(mounted=True, cardDetected=True, freeSpaceKnown=True,
                                     totalBytes=31_902_400_512, freeBytes=31_887_589_376, message="SD card ready."))
        if p == "/api/sd/list":
            return self.respond(dict(path=q.get("path", ["/"])[0], items=SD_ITEMS, nextOffset=0, hasMore=False))
        if p == "/api/leds/settings":
            return self.respond(dict(count=2, maxCount=16, hardwareEnabled=ACTIVE,
                                     groups=[dict(id="off", label="Off"), dict(id="tester", label="Tester mode"),
                                             dict(id="frames", label="Message frame status")],
                                     leds=[dict(group="tester", ota=True), dict(group="frames", ota=False)] +
                                          [dict(group="off", ota=False)] * 14))
        if p == "/api/maker/config":
            return self.respond(dict(fields=maker_fields(), active=ACTIVE, mode="maker"))
        return self.respond("Fixture route not found: " + p, "text/plain", 404)

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        return self.respond("OK", "text/plain")


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
