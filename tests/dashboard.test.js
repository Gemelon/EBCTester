// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const root = path.resolve(__dirname, "..");
const dashboard = path.join(root, "components", "battery_dashboard");
const script = fs.readFileSync(path.join(dashboard, "assets", "dashboard.js"), "utf8");
const strings = JSON.parse(fs.readFileSync(path.join(root, "external", "ota-suite",
  "components", "web_interface", "assets", "en.json"), "utf8")).strings;

function page(runs = [], error = null) {
  const elements = new Map();
  const requests = [];
  const charts = [];
  const timers = [];
  function element(id) {
    assert.notEqual(id, "dash-demo");
    assert.notEqual(id, "dash-delete");
    if (!elements.has(id)) elements.set(id, {
      value: "", textContent: "", disabled: false, options: [], listeners: {}, style: {},
      querySelector() { return {}; },
      setAttribute() {},
      replaceChildren(...children) { this.options = children; this.value = children[0]?.value || ""; },
      append(child) { this.options.push(child); if (!this.value) this.value = child.value || ""; },
      addEventListener(name, listener) { this.listeners[name] = listener; },
    });
    return elements.get(id);
  }
  const context = {
    document: {
      getElementById: element, querySelector: () => ({}), head: { append() {} },
      createElement: () => ({ append() {} }), visibilityState: "visible",
    },
    MutationObserver: class { observe() {} },
    I18n: {
      t: (key) => { assert.ok(Object.hasOwn(strings, key), key); return strings[key]; },
      f: (key, values) => strings[key].replace(/\{(p\d+)\}/g, (_, name) => String(values[name])),
      message: (text) => text, locale: "en-GB",
    },
    Chart: class {
      constructor(canvas, options) { charts.push(options); }
      destroy() {}
    },
    setTimeout(callback) { timers.push(callback); return timers.length; },
    clearTimeout() {},
    fetch: async (url, options) => {
      requests.push({ url, options });
      assert.equal(options.method, undefined, "the dashboard must only read data");
      let body;
      if (url === "/api/db/runs") {
        body = { batteries: [{ id: 1, label: "Existing battery", nominalAh: 20 }], runs };
      } else if (url.startsWith("/api/db/run?")) {
        body = { run: { status: "RUNNING", batteryLabel: "Existing battery" }, segments: [], last: {} };
      } else if (url.startsWith("/api/db/measurements?")) {
        body = { points: [], stride: 1, total: 0 };
      } else {
        assert.fail(`Unexpected dashboard endpoint: ${url}`);
      }
      return { ok: !error, status: error ? 503 : 200,
        text: async () => error || JSON.stringify(body) };
    },
  };
  context.window = context;
  vm.createContext(context);
  vm.runInContext(script, context);
  return { element, requests, charts, timers };
}

const settle = () => new Promise(setImmediate);

test("empty dashboard initializes and refreshes without demo/delete controls", async () => {
  const view = page();
  await settle();
  assert.equal(view.element("dash-message").textContent, "No test runs in the database yet.");
  assert.equal(view.element("dash-load").disabled, true);
  view.element("dash-refresh").listeners.click();
  await settle();
  assert.equal(view.requests.length, 2);
  assert.ok(view.requests.every(({ url }) => url === "/api/db/runs"));
});

test("existing runs still load charts and schedule live refresh using GET only", async () => {
  const view = page([{ id: "existing-run", batteryId: 1, status: "RUNNING" }]);
  await settle();
  assert.equal(view.element("dash-battery-label").textContent, "Existing battery");
  assert.equal(view.charts.length, 3);
  assert.equal(view.element("dash-load").disabled, false);
  assert.equal(view.timers.length, 1);
  view.timers.shift()();
  await settle();
  assert.equal(view.charts.length, 6);
  assert.equal(view.requests.length, 5);
});

test("database errors remain visible without generating or deleting data", async () => {
  const view = page([], "Database not ready.");
  await settle();
  assert.match(view.element("dash-message").textContent, /Database not ready/);
  assert.match(view.element("dash-message").className, /alert-danger/);
  assert.equal(view.requests.length, 1);
});

test("demo generation and run deletion are absent from firmware routes and build inputs", () => {
  const html = fs.readFileSync(path.join(dashboard, "assets", "dashboard.html"), "utf8");
  const backend = fs.readFileSync(path.join(dashboard, "battery_dashboard.cpp"), "utf8");
  assert.doesNotMatch(html + script, /dash-demo|dash-delete|createDemo|deleteRun/);
  assert.doesNotMatch(backend, /\/api\/db\/demo|\/api\/db\/run\/delete|delete_operation|demo_handler|delete_handler/);
  const database = path.join(root, "components", "battery_database");
  assert.equal(fs.existsSync(path.join(database, "battery_demo_data.cpp")), false);
  assert.doesNotMatch(fs.readFileSync(path.join(database, "CMakeLists.txt"), "utf8"), /battery_demo_data/);
  assert.doesNotMatch(fs.readFileSync(path.join(database, "include", "battery_database.h"), "utf8"),
    /battery_database_create_demo_run/);
});
