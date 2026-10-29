# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0
"""Host tests for the tester model and SD bus settings in the web UI, without hardware.

Starts mock_server.py in-process, checks the API contract the scripts rely on, and loads the
real pages in headless Chrome to verify that the forms adopt the model's current limit and that
the settings cards render without script errors. The Chrome part is skipped when no Chrome or
Chromium binary is found (set CHROME to override).
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mock_server  # noqa: E402

TABS = mock_server.TAB_ORDER


def find_chrome():
    for name in (os.environ.get("CHROME"), "google-chrome", "chromium", "chromium-browser"):
        if name and shutil.which(name):
            return shutil.which(name)
    return None


class MockTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = mock_server.MockDevice()
        cls.server = mock_server.make_server(port=0, device=cls.device)
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        self.device.reset(loginRequired=False)

    def request(self, path, body=None, csrf=mock_server.CSRF_TOKEN, password=mock_server.MOCK_PASSWORD,
                headers=None):
        headers = {"Content-Type": "application/json", **(headers or {})}
        if csrf:
            headers["X-CSRF-Token"] = csrf
        if password:
            headers["X-Admin-Password"] = password
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=5) as response:
                return response.status, response.read().decode()
        except urllib.error.HTTPError as error:
            with error:
                return error.code, error.read().decode()

    def get_json(self, path):
        status, text = self.request(path)
        self.assertEqual(status, 200, text)
        return json.loads(text)


class ApiContractTest(MockTestCase):
    def test_settings_cards_are_registered_and_served(self):
        tabs = {tab["id"]: tab for tab in self.get_json("/api/status")["tabs"]}
        for ext_id in ("tester-settings", "sd-settings"):
            self.assertEqual(tabs[ext_id]["placement"], "settings")
            for url in (tabs[ext_id]["contentUrl"], tabs[ext_id]["scriptUrl"]):
                self.assertEqual(self.request(url)[0], 200, url)

    def test_post_requires_csrf_token(self):
        self.assertEqual(self.request("/api/tester/model", {"model": "A40L"}, csrf=None)[0], 403)
        self.assertEqual(self.request("/api/sd/bus", {"mode": "sdmmc"}, csrf="wrong")[0], 403)

    def test_login_session_is_required(self):
        self.device.reset()
        self.assertEqual(self.request("/api/status")[0], 401)
        self.assertEqual(self.request("/api/auth/login", {"username": "admin", "password": "wrong"},
                                      headers={"X-Web-Login": "1"})[0], 403)
        req = urllib.request.Request(self.base + "/api/auth/login", method="POST",
                                     data=json.dumps({"username": "admin", "password": mock_server.MOCK_PASSWORD}).encode(),
                                     headers={"Content-Type": "application/json", "X-Web-Login": "1"})
        with urllib.request.urlopen(req, timeout=5) as response:
            cookie = response.headers["Set-Cookie"].split(";")[0]
        self.assertEqual(self.request("/api/status", headers={"Cookie": cookie})[0], 200)

    def test_settings_require_admin_password(self):
        for path, body in (("/api/tester/model", {"model": "A40L"}), ("/api/sd/bus", {"mode": "sdmmc"})):
            self.assertEqual(self.request(path, body, password=None)[0], 403, path)
            self.assertEqual(self.request(path, body, password="wrong")[0], 403, path)
            self.assertIn(self.request(path, body)[0], (200, 202), path)
        self.assertEqual(self.get_json("/api/tester/status")["testerModel"], "A40L")
        # Test controls need no admin password, only the session and the CSRF token.
        self.assertEqual(self.request("/api/tester/command", {"action": "stop"}, password=None)[0], 200)

    def test_model_change_reaches_status_and_settings(self):
        self.assertEqual(self.get_json("/api/tester/status")["maxCurrent"], 20)
        self.assertEqual(self.request("/api/tester/model", {"model": "A40L"})[0], 200)
        self.assertEqual(self.get_json("/api/tester/status")["maxCurrent"], 40)
        settings = self.get_json("/api/tester/settings")
        self.assertEqual((settings["testerModel"], settings["maxCurrent"]), ("A40L", 40))
        for invalid in ("a40l", "A40", "", None, 40):
            self.assertEqual(self.request("/api/tester/model", {"model": invalid})[0], 400)

    def test_sd_bus_change_requires_restart(self):
        self.assertEqual(self.request("/api/sd/bus", {"mode": "sdmmc", "restart": False})[0], 200)
        status = self.get_json("/api/sd/status")
        self.assertEqual((status["busMode"], status["configuredBusMode"], status["restartRequired"]),
                         ("spi", "sdmmc", True))
        self.assertEqual(self.request("/api/sd/bus", {"mode": "sdmmc", "restart": True})[0], 202)
        self.assertFalse(self.get_json("/api/sd/status")["restartRequired"])
        self.assertEqual(self.request("/api/sd/bus", {"mode": "SPI"})[0], 400)

    def test_a20_limit_messages_are_translated(self):
        """The firmware sends the original catalog texts for the A20 so that the UI translates them."""
        assets = mock_server.SHARED
        english = json.loads((assets / "en.json").read_text(encoding="utf-8"))["strings"]
        german = json.loads((assets / "de.json").read_text(encoding="utf-8"))["strings"]
        texts = mock_server.limit_text(20)
        for text in texts.values():
            keys = [key for key, value in english.items() if value == text]
            self.assertEqual(len(keys), 1, text)
            self.assertNotEqual(german[keys[0]], text)


class CatalogTest(unittest.TestCase):
    """Every translation key used by the web assets must exist in the English and German catalogs."""

    def test_all_used_keys_exist(self):
        catalog = {lang: json.loads((mock_server.SHARED / f"{lang}.json").read_text(encoding="utf-8"))["strings"]
                   for lang in ("en", "de")}
        self.assertEqual(set(catalog["en"]), set(catalog["de"]))
        assets = [path for *_, html, script in mock_server.EXTENSIONS for path in (html, script)]
        used = set()
        for path in assets:
            source = path.read_text(encoding="utf-8")
            used |= set(re.findall(r'data-i18n="(text\.[0-9a-f]{12})"', source))
            used |= set(re.findall(r'I18n\.[tf]\("(text\.[0-9a-f]{12})"', source))
        self.assertGreater(len(used), 100)
        self.assertEqual(sorted(used - set(catalog["en"])), [])

    def test_new_texts_are_translated(self):
        english = json.loads((mock_server.SHARED / "en.json").read_text(encoding="utf-8"))["strings"]
        german = json.loads((mock_server.SHARED / "de.json").read_text(encoding="utf-8"))["strings"]
        for text in ("Tester model", "Bus mode", "Save and restart", "Enable MQTT", "Follow output",
                     "SD bus mode saved. The device restarts now.", "MQTT is disabled", "Debug log",
                     "Current exceeds the %s limit of %d A."):
            key = next(k for k, v in english.items() if v == text)
            self.assertNotEqual(german[key], text, text)


@unittest.skipUnless(find_chrome(), "Chrome/Chromium not found")
class BrowserTest(MockTestCase):
    """Renders the real pages; DOM attributes show the limits that the scripts applied."""

    def render(self, first_tab, model="A20", lang="en-GB", **state):
        self.device.reset(model=model, tabOrder=f"{first_tab},{TABS}", loginRequired=False, **state)
        with tempfile.TemporaryDirectory() as profile:
            result = subprocess.run(
                [find_chrome(), "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
                 f"--user-data-dir={profile}", f"--lang={lang}", f"--accept-lang={lang}",
                 "--enable-logging=stderr", "--v=0",
                 "--virtual-time-budget=8000", "--dump-dom", self.base + "/"],
                capture_output=True, text=True, timeout=90)
        errors = [line for line in result.stderr.splitlines()
                  if "CONSOLE" in line and re.search(r"Uncaught|Unknown translation key|TypeError", line)]
        self.assertEqual(errors, [], f"script errors on {first_tab} ({model})")
        return result.stdout

    def max_of(self, dom, element_id):
        match = re.search(rf'<(?:input|select)[^>]*\bid="{re.escape(element_id)}"[^>]*>', dom)
        self.assertIsNotNone(match, element_id)
        value = re.search(r'\bmax="([^"]*)"', match.group(0))
        return value.group(1) if value else None

    def test_manual_tab_adopts_limit(self):
        for model, limit in (("A20", "20"), ("A40L", "40")):
            dom = self.render("manual", model)
            for element_id in ("man-charge-current", "man-charge-cutoff", "man-discharge-value"):
                self.assertEqual(self.max_of(dom, element_id), limit, f"{element_id} ({model})")
            self.assertEqual(self.max_of(dom, "man-charge-voltage"), "30")

    def test_automatic_tab_adopts_limit(self):
        for model, limit in (("A20", "20"), ("A40L", "40")):
            dom = self.render("auto", model)
            self.assertEqual(self.max_of(dom, "auto-value-1"), limit, model)

    def test_resistance_tab_adopts_limit(self):
        for model, limit in (("A20", "20"), ("A40L", "40")):
            dom = self.render("resistance", model)
            for name in ("chargeCurrent", "chargeCutoff", "prepCurrent", "pulseCurrent"):
                self.assertEqual(self.max_of(dom, f"res-{name}"), limit, f"{name} ({model})")
            self.assertEqual(self.max_of(dom, "res-maxPower"), "2000")

    def test_every_tab_renders_without_script_errors(self):
        for tab in TABS.split(","):
            with self.subTest(tab=tab):
                self.assertIn(f'id="tab-{tab}"', self.render(tab))

    def visible_text(self, dom):
        dom = re.sub(r"<(script|style)\b.*?</\1>", " ", dom, flags=re.S)
        return re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", dom))

    def test_new_and_settings_cards_follow_the_language(self):
        catalog = {lang: json.loads((mock_server.SHARED / f"{lang}.json").read_text(encoding="utf-8"))["strings"]
                   for lang in ("en", "de")}
        sources = ["EBC tester", "Tester model", "Save setting", "SD card bus", "Bus mode", "Save and restart",
                   "Enable debug log and Debug tab", "Enable MQTT", "MQTT broker host", "Topic prefix",
                   "Allow remote plan start commands", "Save settings"]
        keys = {text: next(k for k, v in catalog["en"].items() if v == text) for text in sources}
        for lang, code in (("de", "de-DE"), ("en", "en-GB")):
            with self.subTest(lang=lang):
                text = self.visible_text(self.render("settings", "A40L", lang=code))
                for source, key in keys.items():
                    self.assertIn(catalog[lang][key], text, source)
                if lang == "de":
                    for source in ("Tester model", "Bus mode", "Enable MQTT", "Topic prefix", "Save setting"):
                        self.assertNotIn(source, text)

    def test_debug_tab_follows_the_language(self):
        text = self.visible_text(self.render("debug", lang="de-DE"))
        for german in ("Puffer leeren", "Herunterladen", "Ausgabe folgen", "Zeilen im Puffer"):
            self.assertIn(german, text)
        self.assertNotIn("Follow output", text)

    def test_login_page_renders(self):
        self.device.reset()
        with tempfile.TemporaryDirectory() as profile:
            result = subprocess.run(
                [find_chrome(), "--headless=new", "--disable-gpu", "--no-first-run", f"--user-data-dir={profile}",
                 "--lang=de-DE", "--enable-logging=stderr", "--v=0", "--virtual-time-budget=4000",
                 "--dump-dom", self.base + "/"], capture_output=True, text=True, timeout=90)
        self.assertIn('id="login-form"', result.stdout)
        self.assertEqual([line for line in result.stderr.splitlines() if "CONSOLE" in line and "Uncaught" in line], [])

    def test_settings_cards_render(self):
        dom = self.render("settings", "A40L")
        self.assertIn('id="tester-settings-model"', dom)
        self.assertRegex(dom, r'id="sd-settings-active">SPI<')
        requested = {path for _, path in self.device.state["requests"]}
        self.assertTrue({"/api/tester/settings", "/api/sd/status"} <= requested)


if __name__ == "__main__":
    unittest.main(verbosity=2)
