// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const root = path.resolve(__dirname, "..");
const script = fs.readFileSync(path.join(root, "components", "test_control", "assets", "battery.js"), "utf8");
const strings = JSON.parse(fs.readFileSync(path.join(root, "external", "ota-suite",
  "components", "web_interface", "assets", "en.json"), "utf8")).strings;
const settle = () => new Promise(setImmediate);

function page({ label = "Current", labels = ["Current", "Other"], confirmResult = true,
  headers = { "X-CSRF-Token": "confirmed" }, failure = false, databaseReady = true } = {}) {
  const elements = new Map(), requests = [], confirmations = [], events = [];
  let battery = { label }, catalog = labels;
  function element(id) {
    if (!elements.has(id)) elements.set(id, {
      value: "", textContent: "", disabled: false, children: [], listeners: {},
      addEventListener(name, handler) { this.listeners[name] = handler; },
      append(item) { this.children.push(item); },
      replaceChildren() { this.children = []; }, focus() {},
    });
    return elements.get(id);
  }
  const context = {
    document: { getElementById: element },
    I18n: {
      t(key) { assert.ok(strings[key], key); return strings[key]; },
      f(key, values) { return this.t(key).replace(/\{(p\d+)\}/g, (_, key) => values[key]); },
      message(text) { return text; },
    },
    WebAuth: { async settingsHeaders() { return headers; } },
    Option: function(text, value) { this.textContent = text; this.value = value; },
    CustomEvent: class { constructor(type, options) { this.type = type; this.detail = options.detail; } },
    confirm(text) { confirmations.push(text); return confirmResult; },
    dispatchEvent(event) { events.push(event); },
    fetch: async (url, options = {}) => {
      requests.push({ url, options });
      if (url === "/api/status") return { ok: true, json: async () => ({ csrfToken: "token" }) };
      if (options.method === "POST") {
        const body = JSON.parse(options.body);
        if (failure) return { ok: false, text: async () => strings["battery.deleteUsed"] };
        if (body.action === "delete_db") {
          catalog = catalog.filter(value => value !== body.label); battery = {};
          return { ok: true, text: async () => strings["battery.deleted"] };
        }
        if (body.action === "save" || body.action === "save_db") battery = body.battery;
        if (body.action === "load_db") battery = { label: body.label };
        return { ok: true, text: async () => "Applied." };
      }
      return { ok: true, text: async () => JSON.stringify(url.endsWith("settings") ?
        { battery } : { batteries: catalog, databaseReady }) };
    },
  };
  context.window = context;
  vm.runInNewContext(script, context);
  return {
    element, requests, confirmations, events,
    click(id) { return element(id).listeners.click(); },
    actions() { return requests.filter(request => request.options.method === "POST"); },
  };
}

test("delete targets the displayed battery, not the saved-list selection, and clears it", async () => {
  const view = page(); await settle();
  view.element("bat-list").value = "Other";
  assert.equal(view.element("bat-delete").disabled, false);
  await view.click("bat-delete");
  assert.deepEqual(JSON.parse(view.actions()[0].options.body), { action: "delete_db", label: "Current" });
  assert.equal(view.actions()[0].options.headers["X-CSRF-Token"], "token");
  assert.match(view.confirmations[0], /"Current"/);
  assert.equal(view.element("bat-label").value, "");
  assert.equal(view.element("bat-delete").disabled, true);
  assert.deepEqual(view.element("bat-list").children.map(item => item.value), ["Other"]);
  assert.equal(view.events[0].type, "test-runs-changed");
});

test("new or unsaved batteries and modified labels cannot be deleted", async () => {
  for (const options of [{ label: "" }, { label: "Unsaved" }, { databaseReady: false }]) {
    const view = page(options); await settle();
    assert.equal(view.element("bat-delete").disabled, true);
    await view.click("bat-delete");
    assert.equal(view.actions().length, 0);
  }
  const view = page(); await settle();
  view.element("bat-label").value = "Other";
  view.element("bat-label").listeners.input();
  assert.equal(view.element("bat-delete").disabled, true);
  view.click("bat-new");
  assert.equal(view.element("bat-delete").disabled, true);
});

test("cancelled deletion or settings authorization never posts a mutation", async () => {
  for (const options of [{ confirmResult: false }, { headers: null }]) {
    const view = page(options); await settle();
    await view.click("bat-delete");
    assert.equal(view.actions().length, 0);
    assert.equal(view.element("bat-label").value, "Current");
    assert.equal(view.element("bat-delete").disabled, false);
  }
});

test("referenced batteries remain selected and an explicit error is shown without retry", async () => {
  const view = page({ failure: true }); await settle();
  await view.click("bat-delete"); await settle();
  assert.equal(view.actions().length, 1);
  assert.equal(view.element("bat-label").value, "Current");
  assert.match(view.element("bat-message").textContent, /has test runs/);
  assert.equal(view.events.length, 0);
  assert.equal(view.element("bat-delete").disabled, false);
});

test("deletion locks editing and duplicate actions while confirmation is pending", async () => {
  const view = page(); await settle();
  const pending = view.click("bat-delete");
  assert.equal(view.element("bat-label").disabled, true);
  assert.equal(view.element("bat-new").disabled, true);
  await view.click("bat-delete");
  view.click("bat-new");
  await pending;
  assert.equal(view.actions().length, 1);
  assert.equal(view.element("bat-label").disabled, false);
});

test("delete button follows New battery and backend guards active and changed selections", () => {
  const html = fs.readFileSync(path.join(root, "components", "test_control", "assets", "battery.html"), "utf8");
  assert.match(html, /id="bat-new"[\s\S]*?<\/button>\s*<button id="bat-delete"/);
  const backend = fs.readFileSync(path.join(root, "components", "test_control", "test_control.cpp"), "utf8");
  const deletion = backend.slice(backend.indexOf("std::string delete_battery"), backend.indexOf("std::string set_plan"));
  assert.match(deletion, /guard\(state_mutex\)/);
  assert.match(deletion, /recorder_status\(\)\.active/);
  assert.match(deletion, /label != settings\.battery\.label/);
  const web = fs.readFileSync(path.join(root, "components", "test_control", "test_control_web.cpp"), "utf8");
  assert.match(web, /action == "delete_db"[\s\S]*?web_interface_authorize_settings_request\(req\)/);
});
