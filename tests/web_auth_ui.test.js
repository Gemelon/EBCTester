// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const root = path.resolve(__dirname, "..");
const web = path.join(root, "external", "ota-suite", "components", "web_interface", "assets");

function elements() {
  const entries = new Map();
  return (id) => {
    if (!entries.has(id)) entries.set(id, {
      value: "", checked: false, disabled: false, textContent: "", className: "", listeners: {},
      classList: { add() {}, remove() {} },
      addEventListener(name, callback) { this.listeners[name] = callback; },
      focus() {},
    });
    return entries.get(id);
  };
}

const settle = () => new Promise(setImmediate);
const i18n = { t: (key) => key, message: (text) => text, f: (key) => key, locale: "en-GB" };

for (const success of [false, true]) {
  test(`login ${success ? "success" : "failure"} sends credentials only to the login endpoint`, async () => {
    const element = elements();
    const requests = [];
    const redirects = [];
    const context = {
      document: { getElementById: element },
      I18n: i18n, AbortSignal,
      location: { replace: (url) => redirects.push(url) },
      fetch: async (url, options) => {
        requests.push({ url, options });
        return { ok: success, text: async () => "Incorrect username or password." };
      },
    };
    vm.createContext(context);
    vm.runInContext(fs.readFileSync(path.join(web, "login.js"), "utf8"), context);
    element("login-username").value = "admin";
    element("login-password").value = "fixture-only-password";
    const submit = () => element("login-form").listeners.submit({ preventDefault() {} });
    const first = submit();
    await submit();
    await first;
    assert.equal(requests.length, 1);
    assert.equal(requests[0].url, "/api/auth/login");
    assert.equal(requests[0].options.headers["X-Web-Login"], "1");
    assert.equal(requests[0].options.credentials, "same-origin");
    assert.deepEqual(JSON.parse(requests[0].options.body),
      { username: "admin", password: "fixture-only-password" });
    assert.equal(element("login-password").value, "");
    assert.equal(element("login-submit").disabled, false);
    assert.deepEqual(redirects, success ? ["/"] : []);
    if (!success) assert.match(element("login-error").textContent, /Incorrect/);
  });
}

for (const [component, script, form, url] of [
  ["debug_log", "debug_settings.js", "debug-settings-form", "/api/debug/settings"],
  ["ha_mqtt", "mqtt.js", "mqtt-form", "/api/mqtt/settings"],
  ["mariadb_sync", "mariadb.js", "mdb-form", "/api/mariadb/settings"],
]) {
  for (const outcome of ["cancel", "success", "forbidden"]) {
    test(`${component} settings ${outcome} uses the shared password confirmation`, async () => {
      const element = elements();
      const mutations = [];
      let prompts = 0;
      const context = {
        document: { getElementById: element },
        I18n: i18n, setTimeout() {}, clearTimeout() {},
        WebAuth: {
          async settingsHeaders() {
            prompts++;
            return outcome === "cancel" ? null :
              { "X-CSRF-Token": "csrf", "X-Admin-Password": "fixture-only-password" };
          },
        },
        fetch: async (requestedUrl, options = {}) => {
          if (options.method === "POST") {
            mutations.push({ url: requestedUrl, options });
            return {
              ok: outcome !== "forbidden",
              text: async () => outcome === "forbidden" ? "Incorrect admin password." : "Settings saved.",
            };
          }
          const status = requestedUrl === "/api/status" ? { csrfToken: "csrf" } : {
            setting: false, settings: {
              enabled: false, host: "broker", port: 1883, username: "user",
              topicPrefix: "test", intervalSeconds: 10,
            },
            state: "disabled", message: "", busy: false,
          };
          return { ok: true, json: async () => status, text: async () => JSON.stringify(status) };
        },
      };
      context.window = context;
      vm.createContext(context);
      vm.runInContext(fs.readFileSync(path.join(root, "components", component, "assets", script), "utf8"), context);
      await settle();
      element("mqtt-password").value = "unsaved-broker-password";
      element("mdb-password").value = "unsaved-database-password";
      element(form).listeners.submit({ preventDefault() {} });
      await settle();
      assert.equal(prompts, 1);
      assert.equal(mutations.length, outcome === "cancel" ? 0 : 1);
      if (mutations.length) {
        assert.equal(mutations[0].url, url);
        assert.equal(mutations[0].options.headers["X-CSRF-Token"], "csrf");
        assert.equal(mutations[0].options.headers["X-Admin-Password"], "fixture-only-password");
      }
      if (outcome !== "success") {
        assert.equal(element("mqtt-password").value, "unsaved-broker-password");
        assert.equal(element("mdb-password").value, "unsaved-database-password");
      }
      if (outcome === "forbidden") {
        const message = component === "ha_mqtt" ? "mqtt-message" :
          component === "mariadb_sync" ? "mdb-message" : "debug-settings-message";
        assert.match(element(message).textContent, /Incorrect admin password/);
      }
    });
  }
}
