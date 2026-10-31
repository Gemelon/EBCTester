// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const root = path.resolve(__dirname, "..");
const script = fs.readFileSync(path.join(root, "components", "battery_dashboard", "assets", "test_runs.js"), "utf8");
const strings = JSON.parse(fs.readFileSync(path.join(root, "external", "ota-suite",
  "components", "web_interface", "assets", "en.json"), "utf8")).strings;
const done = { id: "done", name: "", battery: "Cell A", batteryId: 1,
  started: "2026-10-31 12:00:00", plan: "Cycle plan", status: "COMPLETED" };
const settle = () => new Promise(setImmediate);

function page({ runs = [done], confirmResult = true, headers = { "X-CSRF-Token": "token" } } = {}) {
  const elements = new Map(), requests = [], confirmations = [], events = [];
  let responseRuns = runs, failure = false, total = runs.length;
  function element(id = "") {
    if (id && elements.has(id)) return elements.get(id);
    const el = {
      value: "", textContent: "", disabled: false, children: [], listeners: {}, className: "",
      append(child) { this.children.push(child); },
      replaceChildren(...children) { this.children = children; },
      addEventListener(name, callback) { this.listeners[name] = callback; },
      setAttribute(name, value) { this[name] = value; },
      showModal() { this.open = true; }, close() { this.open = false; }, focus() {},
      setCustomValidity(value) { this.invalid = value; }, reportValidity() {},
    };
    if (id) elements.set(id, el);
    return el;
  }
  const context = {
    document: { getElementById: element, createElement: () => element(), addEventListener() {} },
    I18n: {
      locale: "en-GB", t(key) { assert.ok(strings[key], key); return strings[key]; },
      f(key, values) { return this.t(key).replace(/\{(p\d+)\}/g, (_, key) => values[key]); },
      message(value) { return value; },
    },
    WebAuth: { async settingsHeaders() { return headers === null ? null : { ...headers }; } },
    AbortSignal: { timeout: () => undefined }, URLSearchParams, TextEncoder,
    CustomEvent: class { constructor(type, options) { this.type = type; this.detail = options.detail; } },
    dispatchEvent(event) { events.push(event); },
    confirm(text) { confirmations.push(text); return confirmResult; },
    fetch: async (url, options) => {
      requests.push({ url, options });
      if (failure) return { ok: false, text: async () => "Operation failed." };
      if (options.method === "POST") {
        const data = JSON.parse(options.body);
        responseRuns = url.endsWith("/delete") ? responseRuns.filter(run => run.id !== data.id) :
          responseRuns.map(run => run.id === data.id ? { ...run, name: data.name } : run);
        return { ok: true, text: async () => "Saved." };
      }
      return { ok: true, json: async () => ({
        runs: responseRuns, batteries: [{ id: 1, label: "Cell A" }], total, pageSize: 50,
      }) };
    },
  };
  context.window = context;
  vm.runInNewContext(script, context);
  return {
    element, requests, confirmations, events,
    select(index = 0) { element("runs-rows").children[index].children[0].children[0].listeners.change(); },
    click(id) { element(id).listeners.click(); },
    submit(id) { element(id).listeners.submit({ preventDefault() {} }); },
    fail() { failure = true; },
    total(value) { total = value; },
    actions() { return requests.filter(request => request.options.method === "POST"); },
  };
}

test("table preserves the old default name and safely displays custom/user-entered text", async () => {
  const view = page({ runs: [done, { ...done, id: "other", name: "<img src=x onerror=alert(1)>" }] });
  await settle();
  const rows = view.element("runs-rows").children;
  assert.match(rows[0].children[1].textContent, /Cell A · Cycle plan · Completed/);
  assert.equal(rows[1].children[1].textContent, "<img src=x onerror=alert(1)>");
  assert.equal(rows[1].children[1].innerHTML, undefined);
  assert.equal(view.element("runs-delete").disabled, true);
  view.select(); assert.equal(view.element("runs-delete").disabled, false);
  view.select(1);
  assert.equal(view.element("runs-rows").children[0].children[0].children[0].checked, false);
  assert.equal(view.element("runs-rows").children[1].children[0].children[0].checked, true);
});

for (const run of [{ ...done, status: "RUNNING" }, { ...done, protected: true }]) {
  test(`active runs block rename and delete: ${JSON.stringify(run)}`, async () => {
    const view = page({ runs: [run] }); await settle(); view.select();
    assert.equal(view.element("runs-rename").disabled, true);
    assert.equal(view.element("runs-delete").disabled, true);
    view.click("runs-rename"); view.click("runs-delete"); await settle();
    assert.equal(view.actions().length, 0);
    assert.equal(view.element("runs-name-dialog").open, undefined);
  });
}

test("rename trims input, uses settings authorization and CSRF, and refreshes both views", async () => {
  const view = page(); await settle(); view.select(); view.click("runs-rename");
  assert.match(view.element("runs-name").value, /Cycle plan/);
  view.element("runs-name").value = "  New name  ";
  view.submit("runs-name-form"); view.submit("runs-name-form"); await settle();
  assert.equal(view.actions().length, 1);
  const action = view.actions()[0];
  assert.equal(action.url, "/api/test-runs/rename");
  assert.equal(action.options.headers["X-CSRF-Token"], "token");
  assert.deepEqual(JSON.parse(action.options.body), { id: "done", name: "New name" });
  assert.equal(view.element("runs-rows").children[0].children[1].textContent, "New name");
  assert.equal(view.events[0].type, "test-runs-changed");
});

test("empty rename restores the default name and invalid names cannot submit", async () => {
  const view = page(); await settle(); view.select(); view.click("runs-rename");
  view.element("runs-name").value = "a".repeat(101); view.submit("runs-name-form");
  assert.equal(view.actions().length, 0); assert.ok(view.element("runs-name").invalid);
  view.element("runs-name").value = ""; view.submit("runs-name-form"); await settle();
  assert.equal(JSON.parse(view.actions()[0].options.body).name, "");
});

test("delete confirmation identifies the run and warns about unchanged backups", async () => {
  const view = page(); await settle(); view.select(); view.click("runs-delete"); await settle();
  assert.equal(view.actions()[0].url, "/api/test-runs/delete");
  assert.deepEqual(JSON.parse(view.actions()[0].options.body), { id: "done" });
  assert.match(view.confirmations[0], /Cycle plan/);
  assert.match(view.confirmations[0], /cannot be undone.*MariaDB/);
  assert.equal(view.element("runs-delete").disabled, true);
});

test("cancelling deletion or settings confirmation never changes data", async () => {
  const no = page({ confirmResult: false }); await settle(); no.select(); no.click("runs-delete");
  assert.equal(no.actions().length, 0);
  const cancel = page({ headers: null }); await settle(); cancel.select(); cancel.click("runs-delete"); await settle();
  assert.equal(cancel.actions().length, 0);
});

test("combined filters use local calendar boundaries and reset selection", async () => {
  const view = page(); await settle(); view.select();
  view.element("runs-battery").value = "1"; view.element("runs-status").value = "INTERRUPTED";
  view.element("runs-search").value = "  cell check  ";
  view.element("runs-from").value = "2026-10-30"; view.element("runs-to").value = "2026-10-31";
  view.submit("runs-filter"); await settle();
  const query = new URL(view.requests.at(-1).url, "http://device").searchParams;
  assert.equal(query.get("battery"), "1"); assert.equal(query.get("status"), "INTERRUPTED");
  assert.equal(query.get("search"), "cell check"); assert.equal(query.get("page"), "0");
  const expected = new Date("2026-11-01T00:00:00").toISOString().slice(0, 19).replace("T", " ");
  assert.equal(query.get("until"), expected);
  assert.equal(view.element("runs-delete").disabled, true);
  view.click("runs-reset"); await settle();
  const reset = new URL(view.requests.at(-1).url, "http://device").searchParams;
  assert.equal(reset.get("status"), ""); assert.equal(reset.get("search"), "");
});

test("invalid date ranges and overlong searches do not query the database", async () => {
  const view = page(); await settle();
  view.element("runs-from").value = "2026-11-01"; view.element("runs-to").value = "2026-10-31";
  view.submit("runs-filter"); await settle(); assert.equal(view.requests.length, 1);
  view.element("runs-from").value = ""; view.element("runs-to").value = "";
  view.element("runs-search").value = "\u00e4".repeat(51);
  view.submit("runs-filter"); await settle(); assert.equal(view.requests.length, 1);
});

test("pagination requests the next page without selecting or changing a run", async () => {
  const view = page(); view.total(101); await settle();
  assert.equal(view.element("runs-next").disabled, false);
  view.click("runs-next"); await settle();
  assert.equal(new URL(view.requests.at(-1).url, "http://device").searchParams.get("page"), "1");
  assert.equal(view.actions().length, 0);
});

test("failed changes stay visible, preserve data, and are never silently retried", async () => {
  const view = page(); await settle(); view.select(); view.fail();
  view.click("runs-delete"); await settle();
  assert.match(view.element("runs-message").textContent, /Operation failed/);
  assert.equal(view.actions().length, 1);
  assert.equal(view.element("runs-rows").children[0].children[1].textContent.includes("Cycle plan"), true);
  assert.equal(view.events.length, 0);
});

test("refresh precedes apply and reset in the filter toolbar", () => {
  const html = fs.readFileSync(path.join(root, "components", "battery_dashboard", "assets", "test_runs.html"), "utf8");
  assert.deepEqual([...html.matchAll(/<button id="(runs-(?:refresh|apply|reset))"/g)].map(match => match[1]),
    ["runs-refresh", "runs-apply", "runs-reset"]);
});

test("mutation routes require settings authorization and tab order follows device information", () => {
  const backend = fs.readFileSync(path.join(root, "components", "battery_dashboard", "run_management_web.cpp"), "utf8");
  assert.match(backend, /web_interface_authorize_settings_request\(req\)/);
  const defaults = fs.readFileSync(path.join(root, "sdkconfig.defaults"), "utf8");
  assert.match(defaults, /overview,test-runs,sd-card/);
  assert.doesNotMatch(defaults.match(/CONFIG_OTA_SUITE_WEB_PRIMARY_TABS=.*/)[0], /test-runs/);
});
