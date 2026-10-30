// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "..", "components", "test_control",
  "assets", "tester.js"), "utf8");
const settle = () => new Promise(setImmediate);

function fixture() {
  const elements = new Map();
  const listeners = {};
  const requests = [];
  let state = { link: { hasModel: true, model: 9 }, controlsBusy: false, recorder: { active: false } };
  let saveError = "";
  let confirmation = { "X-Admin-Password": "fixture-only" };
  const element = id => {
    if (!elements.has(id)) elements.set(id, {
      value: "", disabled: false, textContent: "", listeners: {},
      classList: { toggle() {} },
      addEventListener(name, callback) { this.listeners[name] = callback; },
    });
    return elements.get(id);
  };
  const context = {
    document: {
      getElementById: element,
      addEventListener(name, callback) { listeners[name] = callback; },
      dispatchEvent(event) { listeners[event.type]?.(event); },
    },
    CustomEvent: class { constructor(type, options) { this.type = type; this.detail = options.detail; } },
    I18n: { message: text => text },
    WebAuth: { settingsHeaders: async () => confirmation },
    fetch: async (url, options) => {
      requests.push({ url, options });
      if (url === "/api/tester/status") return { ok: true, json: async () => state };
      if (url === "/api/status") return { ok: true, json: async () => ({ csrfToken: "fixture-csrf" }) };
      assert.equal(url, "/api/tester/model");
      if (saveError) return { ok: false, text: async () => saveError };
      state = { ...state, link: { hasModel: true, model: JSON.parse(options.body).model } };
      return { ok: true, text: async () => "Tester model saved and applied." };
    },
  };
  vm.runInNewContext(source, context);
  return {
    element, requests,
    status(next) { state = next; listeners["tester-status"]({ detail: state }); },
    error(message) { saveError = message; },
    cancelConfirmation() { confirmation = null; },
    submit() { return element("tester-model-form").listeners.submit({ preventDefault() {} }); },
    statusError() { listeners["tester-status-error"](); },
  };
}

test("offline selection saves with settings authorization and CSRF then refreshes status", async () => {
  const page = fixture();
  await settle();
  assert.equal(page.element("tester-model").value, "9");
  assert.equal(page.element("tester-model-save").disabled, false);
  page.element("tester-model").value = "16";
  await page.submit();
  const post = page.requests.find(request => request.url === "/api/tester/model");
  assert.equal(post.options.method, "POST");
  assert.deepEqual(JSON.parse(post.options.body), { model: 16 });
  assert.equal(post.options.headers["X-CSRF-Token"], "fixture-csrf");
  assert.equal(post.options.headers["X-Admin-Password"], "fixture-only");
  assert.equal(page.element("tester-model").value, "16");
  assert.match(page.element("tester-message").className, /success/);
});

test("polls preserve unsaved choices; active operations and recordings lock changes", async () => {
  const page = fixture();
  await settle();
  page.element("tester-model").value = "16";
  page.status({ link: { hasModel: true, model: 9 } });
  assert.equal(page.element("tester-model").value, "16");
  for (const status of [{ controlsBusy: true }, { recorder: { active: true } }]) {
    page.status(status);
    assert.equal(page.element("tester-model-save").disabled, true);
    await page.submit();
    assert.equal(page.requests.filter(request => request.url === "/api/tester/model").length, 0);
  }
  page.status({ link: { error: "Tester mismatch. Connection closed." } });
  assert.match(page.element("tester-model-error").textContent, /mismatch/);
  page.statusError();
  assert.equal(page.element("tester-model-save").disabled, true);
});

test("save failures stay visible without replacing the unsaved choice", async () => {
  const page = fixture();
  await settle();
  page.element("tester-model").value = "16";
  page.error("Could not save tester model.");
  await page.submit();
  assert.equal(page.element("tester-model").value, "16");
  assert.match(page.element("tester-message").textContent, /Could not save/);
  assert.match(page.element("tester-message").className, /danger/);
  assert.equal(page.element("tester-model-save").disabled, false);
});

test("cancelled settings confirmation sends no model update", async () => {
  const page = fixture();
  await settle();
  page.cancelConfirmation();
  await page.submit();
  assert.equal(page.requests.filter(request => request.url === "/api/tester/model").length, 0);
  assert.equal(page.element("tester-model-save").disabled, false);
});
