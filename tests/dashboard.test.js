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
  const observers = new Map();
  const translations = { ...strings };
  function element(id) {
    assert.notEqual(id, "dash-demo");
    assert.notEqual(id, "dash-delete");
    if (!elements.has(id)) elements.set(id, {
      value: "", textContent: id === "page-subtitle" ? translations["text.690c0f1d7edf"] : "",
      disabled: false, options: [], listeners: {}, style: {},
      querySelector() { return {}; },
      setAttribute() {},
      replaceChildren(...children) { this.options = children; this.value = children[0]?.value || ""; },
      append(child) { this.options.push(child); if (!this.value) this.value = child.value || ""; },
      addEventListener(name, listener) { this.listeners[name] = listener; },
      insertAdjacentElement(position, child) { this.insertedPosition = position; this.insertedElement = child; },
      parentElement: { insertAdjacentElement(position, child) { elements.set(child.id, child); } },
    });
    return elements.get(id);
  }
  const context = {
    document: {
      getElementById: element, querySelector: () => ({}), head: { append() {} },
      createElement: () => ({ append() {}, setAttribute() {} }), visibilityState: "visible",
      addEventListener(name, listener) { documentListeners[name] = listener; },
    },
    MutationObserver: class {
      constructor(callback) { this.callback = callback; }
      observe(target) { observers.set(target, this.callback); }
    },
    I18n: {
      t: (key) => { assert.ok(Object.hasOwn(translations, key), key); return translations[key]; },
      f: (key, values) => strings[key].replace(/\{(p\d+)\}/g, (_, name) => String(values[name])),
      message: (text) => text, locale: "en-GB",
    },
    Chart: class {
      constructor(canvas, options) { charts.push(options); }
      destroy() {}
    },
    setTimeout(callback) { timers.push(callback); return timers.length; },
    clearTimeout() {},
    addEventListener(name, listener) { documentListeners[name] = listener; },
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
  const documentListeners = {};
  context.window = context;
  vm.createContext(context);
  vm.runInContext(script, context);
  return { element, requests, charts, timers, documentListeners, observers, translations };
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

test("custom run names appear unchanged in the dashboard selection", async () => {
  const name = "<b>Cell capacity check</b>";
  const view = page([{ id: "named-run", batteryId: 1, status: "COMPLETED", name }]);
  await settle();
  assert.equal(view.element("dash-run").options[0].textContent, name);
  assert.equal(view.element("dash-run").options[0].innerHTML, undefined);
});

test("management deletion clears the selected dashboard run and refreshes the catalog", async () => {
  const view = page([{ id: "existing-run", batteryId: 1, status: "COMPLETED" }]);
  await settle();
  const before = view.requests.length;
  await view.documentListeners["test-runs-changed"]({
    detail: { id: "existing-run", action: "delete" },
  });
  assert.equal(view.element("dash-battery-label").textContent, "-");
  assert.equal(view.requests.length, before + 1);
  assert.equal(view.requests.at(-1).url, "/api/db/runs");
});

test("database errors remain visible without generating or deleting data", async () => {
  const view = page([], "Database not ready.");
  await settle();
  assert.match(view.element("dash-message").textContent, /Database not ready/);
  assert.match(view.element("dash-message").className, /alert-danger/);
  assert.equal(view.requests.length, 1);
});

test("the header shows the saved tester model while disconnected and preserves mismatch errors", () => {
  const view = page();
  const indicator = view.element("app-brand").insertedElement;
  const subtitle = view.element("page-subtitle");
  assert.equal(indicator.textContent, "Unknown");
  assert.match(subtitle.textContent, /Unknown/);
  assert.equal(view.element("app-brand").insertedPosition, "afterend");
  view.documentListeners["tester-status"]({ detail: { link: {
    connected: true, hasModel: false,
  } } });
  assert.equal(indicator.textContent, "Unknown");
  view.documentListeners["tester-status"]({ detail: { link: {
    connected: false, hasModel: true, model: 0x10, modelKnown: true, modelName: "EBC-A40L",
  } } });
  assert.equal(indicator.textContent, "EBC-A40L");
  assert.equal(indicator.className, "small text-white");
  assert.match(indicator.title, /Selected EBC-A40L/);
  assert.match(subtitle.textContent, /EBC-A40L/);
  view.documentListeners["tester-status"]({ detail: { link: {
    hasModel: true, model: 0x10, modelKnown: true, modelName: "EBC-A40L",
    hasDetectedModel: true, detectedModel: 10, detectedModelName: "EBC-A50L",
    detectedModelKnown: true, detectedModelEnabled: true, detectedVersionKnown: true,
    detectedVersion: 311, detectedVersionPending: false,
  } } });
  assert.equal(indicator.textContent, "EBC-A50L");
  assert.match(indicator.title, /Detected EBC-A50L/);
  view.documentListeners["tester-status"]({ detail: { link: {
    hasDetectedModel: true, detectedModel: 6, detectedModelName: "EBC-A10H",
    detectedModelKnown: true, detectedModelEnabled: false,
  } } });
  assert.equal(indicator.textContent, "EBC-A10H");
  assert.match(indicator.className, /text-bg-warning/);
  assert.match(indicator.title, /control commands are disabled/);
  view.documentListeners["tester-status"]({ detail: { link: {
    hasDetectedModel: true, detectedModel: 99, detectedModelKnown: false,
    detectedVersionKnown: true, detectedVersion: 302,
  } } });
  assert.equal(indicator.textContent, "Unknown device (ident 0x63, version V3.02)");
  const mismatch = "Tester mismatch: selected EBC-A40L, received EBC-A20. Connection closed.";
  view.documentListeners["tester-status"]({ detail: { link: {
    connected: false, hasModel: true, model: 0x10, modelKnown: true, modelName: "EBC-A40L",
    modelMismatch: true, detectedModel: 0x09, error: mismatch,
  } } });
  assert.equal(indicator.textContent, "EBC-A40L", "a rejected device must not replace the saved model");
  assert.equal(view.element("tester-connection-error").textContent, mismatch);
  assert.doesNotMatch(view.element("tester-connection-error").className, /d-none/);
  view.documentListeners["tester-status"]({ detail: { link: {
    connected: true, hasModel: true, model: 0x17, modelKnown: false,
  } } });
  assert.equal(indicator.textContent, "Unknown (0x17)");
  assert.match(indicator.className, /text-bg-warning/);
  assert.match(subtitle.textContent, /0x17/);
  view.documentListeners["tester-status"]({ detail: { link: { connected: false, hasModel: false } } });
  assert.equal(indicator.textContent, "Unknown");
  assert.match(view.element("tester-connection-error").className, /d-none/);
  assert.match(subtitle.textContent, /Unknown/);
  view.documentListeners["tester-status-error"]();
  assert.equal(indicator.textContent, "Unknown");
});

test("shell refreshes cannot restore a stale model or overwrite administration subtitles", () => {
  const view = page();
  const subtitle = view.element("page-subtitle");
  const updateHeading = view.observers.get(subtitle);
  assert.equal(typeof updateHeading, "function");
  const status = view.documentListeners["tester-status"];
  for (const link of [
    { connected: false },
    { connected: true, hasModel: true, modelKnown: true, modelName: "EBC-A40L", model: 0x10 },
    { connected: true, hasModel: true, modelKnown: false, model: 0x17 },
  ]) {
    status({ detail: { link } });
    const expected = subtitle.textContent;
    for (let poll = 0; poll < 20; poll++) {
      subtitle.textContent = view.translations["text.690c0f1d7edf"];
      updateHeading();
      assert.equal(subtitle.textContent, expected);
      updateHeading();
      assert.equal(subtitle.textContent, expected, "observer updates must stabilize");
    }
    subtitle.textContent = strings["text.9735448f0c23"];
    updateHeading();
    status({ detail: { link } });
    assert.equal(subtitle.textContent, strings["text.9735448f0c23"]);
    subtitle.textContent = view.translations["text.690c0f1d7edf"];
    updateHeading();
    assert.equal(subtitle.textContent, expected);
  }
  view.translations["text.690c0f1d7edf"] = "Measurements for EBC-A20";
  subtitle.textContent = view.translations["text.690c0f1d7edf"];
  updateHeading();
  assert.equal(subtitle.textContent, "Measurements for Unknown (0x17)");
  view.documentListeners["tester-status-error"]();
  assert.equal(subtitle.textContent, "Measurements for Unknown");
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
