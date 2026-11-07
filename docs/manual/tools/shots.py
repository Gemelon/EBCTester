# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0
"""Creates the manual screenshots from the fixture server (simulated data only).

Usage: python docs/manual/tools/shots.py [repo-root] [output-dir] [file.png ...]
Requires Playwright with Chromium (pip install playwright; playwright install chromium).
Set CHROME to use an installed browser instead. Never accesses hardware.
"""
import subprocess
import sys
import time
from pathlib import Path
from playwright.sync_api import sync_playwright

import os
REPO = Path(__file__).resolve().parents[3]
ROOT = sys.argv[1] if len(sys.argv) > 1 else str(REPO)
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else REPO / "docs" / "images" / "manual"
ONLY = set(sys.argv[3:])
OUT.mkdir(parents=True, exist_ok=True)
HERE = Path(__file__).parent
PORTS = {"factory": 8801, "maker-inactive": 8802, "maker-active": 8803}


def start(mode):
    proc = subprocess.Popen([sys.executable, str(HERE / "fixture.py"), ROOT, str(PORTS[mode]), mode])
    time.sleep(0.8)
    return proc


HIDE = "#action-result{display:none!important}"


def idle(page, on):
    page.evaluate(f"fetch('/fixture/idle?on={1 if on else 0}')")
    page.wait_for_timeout(1200)


def open_tab(page, tab, wait=1500):
    page.click(f"#tab-button-{tab}")
    page.wait_for_timeout(wait)


def section(page, selector, name, pad=8):
    el = page.locator(selector).first
    el.scroll_into_view_if_needed()
    page.wait_for_timeout(300)
    el.screenshot(path=str(OUT / name))


def full(page, name):
    page.screenshot(path=str(OUT / name), full_page=True)


def want(name):
    return not ONLY or name in ONLY


def factory(browser):
    ctx = browser.new_context(viewport=dict(width=1280, height=900), device_scale_factor=1.5,
                              locale="en-US", timezone_id="Europe/Berlin")
    page = ctx.new_page()
    page.on("dialog", lambda d: d.dismiss())
    base = f"http://127.0.0.1:{PORTS['factory']}"
    if want("login.png"):
        page.goto(base + "/login")
        page.wait_for_timeout(800)
        page.fill("input[type=password]", "")
        section(page, ".card", "login.png", 24)
    page.goto(base + "/")
    page.add_style_tag(content=HIDE)
    page.wait_for_timeout(2500)
    if want("header.png"):
        section(page, "header.app-header", "header.png", 0)
    if want("dashboard.png"):
        page.wait_for_timeout(1500)
        full(page, "dashboard.png")
    for tab, name in [("battery", "battery.png"), ("auto", "auto.png")]:
        if want(name):
            open_tab(page, tab, 2000)
            full(page, name)
    idle(page, True)
    if want("manual.png"):
        open_tab(page, "manual", 2000)
        full(page, "manual.png")
    if want("resistance.png"):
        open_tab(page, "resistance", 2500)
        full(page, "resistance.png")
    idle(page, False)
    page.click("[data-section=administration]")
    page.wait_for_timeout(1200)
    for tab, name in [("overview", "overview.png"), ("test-runs", "test-runs.png"), ("sd-card", "sd-card.png"),
                      ("updates", "updates.png"), ("debug", "debug.png")]:
        if want(name):
            open_tab(page, tab, 2000)
            full(page, name)
    open_tab(page, "settings", 3000)
    if want("settings.png"):
        full(page, "settings.png")
    for sel, name in [("#tab-settings section:nth-of-type(1) .card", "settings-login.png"),
                      ("#tab-settings section:nth-of-type(2) .card", "settings-wifi.png"),
                      ("#settings-ext-tester", "settings-tester.png"),
                      ("#settings-ext-mariadb", "settings-mariadb.png"),
                      ("#settings-ext-mqtt", "settings-mqtt.png"),
                      ("#settings-ext-debug-settings", "settings-debug.png"),
                      ("#settings-ext-languages", "settings-languages.png"),
                      ("#settings-ext-config-reset", "settings-reset.png")]:
        if want(name):
            section(page, sel, name)
    if want("password-dialog.png"):
        page.evaluate("document.getElementById('settings-password-dialog').showModal()")
        page.wait_for_timeout(300)
        section(page, "#settings-password-dialog", "password-dialog.png", 16)
    ctx.close()


def maker(browser, mode):
    ctx = browser.new_context(viewport=dict(width=1280, height=900), device_scale_factor=1.5,
                              locale="en-US", timezone_id="Europe/Berlin")
    page = ctx.new_page()
    page.on("dialog", lambda d: d.dismiss())
    page.goto(f"http://127.0.0.1:{PORTS[mode]}/")
    page.add_style_tag(content=HIDE)
    page.wait_for_timeout(2500)
    if mode == "maker-inactive":
        if want("maker-inactive.png"):
            page.screenshot(path=str(OUT / "maker-inactive.png"), clip=dict(x=0, y=0, width=1280, height=760))
        if want("maker-settings.png"):
            full(page, "maker-settings.png")
        if want("maker-inactive-settings.png"):
            open_tab(page, "settings", 2500)
            full(page, "maker-inactive-settings.png")
    else:
        page.click("[data-section=administration]")
        page.wait_for_timeout(1000)
        if want("maker-active.png"):
            open_tab(page, "maker-settings", 2000)
            page.screenshot(path=str(OUT / "maker-active.png"), clip=dict(x=0, y=0, width=1280, height=640))
    ctx.close()


if __name__ == "__main__":
    procs = [start(m) for m in PORTS]
    try:
        with sync_playwright() as p:
            chrome = os.environ.get("CHROME")
            browser = p.chromium.launch(executable_path=chrome) if chrome else p.chromium.launch()
            factory(browser)
            maker(browser, "maker-inactive")
            maker(browser, "maker-active")
            browser.close()
    finally:
        for proc in procs:
            proc.terminate()
    # Palette reduction keeps the repository and the PDFs small without blurring text.
    from PIL import Image
    for image in OUT.glob("*.png"):
        if not ONLY or image.name in ONLY:
            Image.open(image).convert("RGB").quantize(256, method=Image.Quantize.MEDIANCUT,
                                                      dither=Image.Dither.NONE).save(image, optimize=True)
