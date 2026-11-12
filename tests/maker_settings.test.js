// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "..", "main", "assets", "maker_settings.js"), "utf8");
const resetSource = fs.readFileSync(path.join(__dirname, "..", "main", "assets", "config_reset.js"), "utf8");
const settle = () => new Promise(setImmediate);

function fixture(script = source) {
  const elements = new Map();
  const requests = [];
  let confirmation = true;
  let headers = { "X-CSRF-Token": "csrf", "X-Admin-Password": "test-only" };
  let failure = "";
  function element(id = "") {
    if (id && elements.has(id)) return elements.get(id);
    const node = {
      value: "", checked: false, disabled: false, textContent: "", children: [], listeners: {},
      append(...children) { this.children.push(...children); },
      replaceChildren(...children) { this.children = children; },
      addEventListener(name, callback) { this.listeners[name] = callback; },
      set id(value) { this._id = value; elements.set(value, this); },
      get id() { return this._id; },
    };
    if (id) { node.id = id; elements.set(id, node); }
    return node;
  }
  const fields = [
    { section: "tester", name: "device_id", label: "Device ID", type: "text", value: "EBC-A20", maxLength: 64 },
    { section: "tester", name: "tester_tx_gpio", label: "TX GPIO", type: "number", value: 17, min: -1, max: 48 },
    { section: "tester", name: "mqtt_enabled", label: "MQTT", type: "checkbox", value: false },
    { section: "ota", name: "web_password", label: "Web password", type: "password", configured: true, maxLength: 63 },
    { section: "sd", name: "bus", label: "SD bus", type: "bus", value: 1 },
    { section: "sd", name: "card_detect_active_high", label: "Card detect active high (off: active low)", type: "checkbox", value: false },
    { section: "rtc", name: "enabled", label: "Enable DS3231 RTC", type: "checkbox", value: false },
    { section: "rtc", name: "sda_gpio", label: "I2C SDA GPIO", type: "number", value: 15, min: -1, max: 48 },
    { section: "rtc", name: "scl_gpio", label: "I2C SCL GPIO", type: "number", value: 16, min: -1, max: 48 },
    { section: "rtc", name: "int_gpio", label: "RTC INT GPIO", type: "number", value: 14, min: -1, max: 48 },
  ];
  vm.runInNewContext(script, {
    document: { getElementById: element, createElement: () => element() },
    window: { confirm: () => confirmation },
    WebAuth: { settingsHeaders: async () => headers },
    fetch: async (url, options = {}) => {
      requests.push({ url, options });
      if (!options.method) return { ok: true, json: async () => ({ active: false, fields }) };
      return { ok: !failure, text: async () => failure || "Saved. Restarting." };
    },
  });
  return {
    element, requests,
    cancel() { confirmation = false; },
    cancelPassword() { headers = null; },
    fail(message) { failure = message; },
    submit() { return element("maker-form").listeners.submit({ preventDefault() {} }); },
    reset(all) { element(all ? "config-reset-all" : "config-reset").listeners.click(); return settle(); },
  };
}

test("renders Maker fields without exposing stored passwords", async () => {
  const page = fixture();
  await settle();
  assert.equal(page.element("maker-web_password").value, "");
  assert.match(page.element("maker-web_password").placeholder, /Configured/);
  assert.equal(page.element("maker-tester_tx_gpio").min, -1);
  assert.equal(page.element("maker-bus").value, "1");
  assert.equal(page.element("maker-card_detect_active_high").checked, false);
  assert.equal(page.element("maker-active").checked, false);
  assert.equal(page.element("maker-save").disabled, false);
  assert.equal(page.element("maker-enabled").checked, false);
  assert.equal(page.element("maker-sda_gpio").value, 15);
  assert.equal(page.element("maker-scl_gpio").value, 16);
  assert.equal(page.element("maker-int_gpio").value, 14);
});

test("saves typed configuration with authorization, activation and restart", async () => {
  const page = fixture();
  await settle();
  page.element("maker-active").checked = true;
  page.element("maker-tester_tx_gpio").value = "12";
  page.element("maker-web_password").value = "new-password";
  page.element("maker-enabled").checked = true;
  page.element("maker-card_detect_active_high").checked = true;
  page.element("maker-sda_gpio").value = "1";
  await page.submit();
  const post = page.requests.find(request => request.options.method === "POST");
  const body = JSON.parse(post.options.body);
  assert.equal(body.active, true);
  assert.equal(body.tester.tester_tx_gpio, 12);
  assert.equal(body.tester.mqtt_enabled, false);
  assert.equal(body.sd.bus, 1);
  assert.equal(body.sd.card_detect_active_high, true);
  assert.deepEqual(body.rtc, { enabled: true, sda_gpio: 1, scl_gpio: 16, int_gpio: 14 });
  assert.equal(post.options.headers["X-CSRF-Token"], "csrf");
  assert.equal(post.options.headers["X-Admin-Password"], "test-only");
  assert.equal(page.element("maker-web_password").value, "");
  assert.equal(page.element("maker-save").disabled, true);
});

test("cancelled confirmations do not write configuration", async () => {
  for (const cancel of ["cancel", "cancelPassword"]) {
    const page = fixture();
    await settle();
    page[cancel]();
    await page.submit();
    assert.equal(page.requests.filter(request => request.options.method === "POST").length, 0);
    assert.equal(page.element("maker-save").disabled, false);
  }
});

test("save failure remains visible and permits correction", async () => {
  const page = fixture();
  await settle();
  page.fail("GPIO conflict");
  await page.submit();
  assert.equal(page.element("maker-message").textContent, "GPIO conflict");
  assert.match(page.element("maker-message").className, /danger/);
  assert.equal(page.element("maker-save").disabled, false);
});

test("reset buttons distinguish configuration and complete NVS reset", async () => {
  for (const all of [false, true]) {
    const page = fixture(resetSource);
    await page.reset(all);
    assert.equal(page.requests.length, 1);
    const post = page.requests[0];
    assert.equal(post.url, "/api/config/reset");
    assert.deepEqual(JSON.parse(post.options.body), { scope: all ? "all" : "config", confirm: true });
    assert.equal(post.options.headers["X-CSRF-Token"], "csrf");
    assert.equal(page.element("config-reset").disabled, true);
  }
});
