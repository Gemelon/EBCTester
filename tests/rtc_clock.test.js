// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "..", "external", "ota-suite",
  "components", "web_interface", "assets", "app.js"), "utf8");
const clockSource = source.slice(source.indexOf("let clockUtc ="), source.indexOf("function toggleFrameLed"));

function fixture() {
  const elements = new Map();
  let now = 0;
  let tick;
  const element = (id) => {
    if (!elements.has(id)) {
      elements.set(id, { textContent: "", classList: {
        toggle(name, hidden) { elements.get(id).hidden = hidden; },
        remove() {},
      } });
    }
    return elements.get(id);
  };
  const context = { byId: element, performance: { now: () => now },
    I18n: { locale: "en-GB" }, Intl, Date, clearTimeout() {},
    setTimeout(callback) { tick = callback; return 1; } };
  vm.createContext(context);
  vm.runInContext(clockSource, context);
  return { element, render: context.renderClock,
    advance(ms) { now += ms; tick(); } };
}

test("device clock uses device UTC rather than the browser clock", () => {
  const page = fixture();
  const utc = Date.UTC(2026, 10, 8, 15, 30, 12) / 1000;
  page.render({ clockEnabled: true, clockUtc: utc });
  const expected = new Date(utc * 1000);
  assert.equal(page.element("device-clock-time").textContent,
    expected.toLocaleTimeString("en-GB", { hour12: false }));
  assert.equal(page.element("device-clock-date").textContent, expected.toLocaleDateString("en-GB"));
  assert.equal(page.element("device-clock").title, Intl.DateTimeFormat().resolvedOptions().timeZone);
  page.advance(1000);
  assert.equal(page.element("device-clock-time").textContent,
    new Date(utc * 1000 + 1000).toLocaleTimeString("en-GB", { hour12: false }));
});

test("disabled RTC without NTP hides clock; invalid RTC shows unavailable", () => {
  const page = fixture();
  page.render({ clockEnabled: false, clockUtc: 0 });
  assert.equal(page.element("device-clock").hidden, true);
  page.render({ clockEnabled: true, clockUtc: 0 });
  assert.equal(page.element("device-clock").hidden, false);
  assert.equal(page.element("device-clock-time").textContent, "--:--:--");
  assert.equal(page.element("device-clock-date").textContent, "----");
});

test("browser time zone handles midnight and daylight-saving transitions", () => {
  const zone = process.env.TZ;
  if (zone !== "Europe/Berlin" && zone !== "Asia/Shanghai") return;
  const page = fixture();
  page.render({ clockEnabled: true, clockUtc: Date.UTC(2026, 10, 8, 23, 30) / 1000 });
  assert.equal(page.element("device-clock-date").textContent, "09/11/2026");
  assert.equal(page.element("device-clock-time").textContent,
    zone === "Europe/Berlin" ? "00:30:00" : "07:30:00");
  page.render({ clockEnabled: true, clockUtc: Date.UTC(2026, 2, 29, 0, 59, 59) / 1000 });
  assert.equal(page.element("device-clock-time").textContent,
    zone === "Europe/Berlin" ? "01:59:59" : "08:59:59");
  page.advance(1000);
  assert.equal(page.element("device-clock-time").textContent,
    zone === "Europe/Berlin" ? "03:00:00" : "09:00:00");
});

test("offline browser does not continue displaying stale device time", () => {
  const page = fixture();
  page.render({ clockEnabled: true, clockUtc: 1794142212 });
  page.advance(14999);
  assert.notEqual(page.element("device-clock-time").textContent, "--:--:--");
  page.advance(1);
  assert.equal(page.element("device-clock-time").textContent, "--:--:--");
});

test("time and date are immediately to the right of EBC RX in the header", () => {
  const html = fs.readFileSync(path.join(__dirname, "..", "external", "ota-suite",
    "components", "web_interface", "assets", "index.html"), "utf8");
  assert.match(html, /id="frame-label"[\s\S]*?id="device-clock"[\s\S]*?id="device-clock-time"[\s\S]*?id="device-clock-date"/);
});
