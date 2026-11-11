// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const scriptPath = path.join(__dirname, "..", "components", "mariadb_sync", "assets", "mariadb.js");

function createElements() {
  const entries = new Map();
  return (id) => {
    if (!entries.has(id)) {
      entries.set(id, {
        value: "",
        checked: false,
        disabled: false,
        listeners: {},
        classList: { toggle() {} },
        addEventListener(name, callback) { this.listeners[name] = callback; },
      });
    }
    return entries.get(id);
  };
}

test("manual MariaDB actions stay disabled until automatic backup is saved off", async () => {
  const element = createElements();
  const status = {
    settings: { enabled: true },
    state: "idle",
    busy: false,
    schemaState: "ready",
    networkConnected: true,
    databaseReady: true,
  };
  const i18n = {
    code: "en",
    locale: "en-GB",
    ready: Promise.resolve(),
    t: (key) => key,
    message: (text) => text,
    f: (key) => key,
  };
  const context = {
    document: {
      getElementById: element,
      querySelectorAll: () => [],
    },
    I18n: i18n,
    WebAuth: { settingsHeaders: async () => ({}) },
    fetch: async (url, options = {}) => {
      if (url === "/api/mariadb/settings") {
        status.settings.enabled = JSON.parse(options.body).enabled;
        return { ok: true, text: async () => "Settings saved." };
      }
      if (url === "/api/status") return { ok: true, json: async () => ({ csrfToken: "csrf" }) };
      return { ok: true, text: async () => JSON.stringify(status) };
    },
    setTimeout: () => 1,
    clearTimeout() {},
  };
  context.window = context;
  vm.createContext(context);
  vm.runInContext(fs.readFileSync(scriptPath, "utf8"), context);
  await new Promise(setImmediate);

  const manualButtons = ["mdb-test", "mdb-sync", "mdb-full-sync"].map(element);
  assert.ok(manualButtons.every((button) => button.disabled));

  element("mdb-enabled").checked = false;
  element("mdb-enabled").listeners.change();
  assert.ok(manualButtons.every((button) => button.disabled));

  element("mdb-form").listeners.submit({ preventDefault() {} });
  await new Promise(setImmediate);
  await new Promise(setImmediate);
  assert.ok(manualButtons.every((button) => !button.disabled));

  element("mdb-enabled").checked = true;
  element("mdb-enabled").listeners.change();
  assert.ok(manualButtons.every((button) => button.disabled));
});
