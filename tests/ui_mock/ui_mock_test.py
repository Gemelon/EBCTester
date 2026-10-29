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
        self.device.reset()

    def request(self, path, body=None, csrf=mock_server.CSRF_TOKEN):
        headers = {"Content-Type": "application/json"}
        if csrf:
            headers["X-CSRF-Token"] = csrf
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


@unittest.skipUnless(find_chrome(), "Chrome/Chromium not found")
class BrowserTest(MockTestCase):
    """Renders the real pages; DOM attributes show the limits that the scripts applied."""

    def render(self, first_tab, model="A20"):
        self.device.reset(model=model, tabOrder=f"{first_tab},{TABS}")
        with tempfile.TemporaryDirectory() as profile:
            result = subprocess.run(
                [find_chrome(), "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
                 f"--user-data-dir={profile}", "--enable-logging=stderr", "--v=0",
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

    def test_settings_cards_render(self):
        dom = self.render("settings", "A40L")
        self.assertIn('id="tester-settings-model"', dom)
        self.assertRegex(dom, r'id="sd-settings-active">SPI<')
        requested = {path for _, path in self.device.state["requests"]}
        self.assertTrue({"/api/tester/settings", "/api/sd/status"} <= requested)


if __name__ == "__main__":
    unittest.main(verbosity=2)
