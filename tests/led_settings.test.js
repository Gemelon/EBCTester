// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const root = path.resolve(__dirname, "..");
const script = fs.readFileSync(path.join(root, "main", "assets", "led_settings.js"), "utf8");
const strings = JSON.parse(fs.readFileSync(path.join(root, "external", "ota-suite", "components",
  "web_interface", "assets", "en.json"), "utf8")).strings;
const settle = () => new Promise(setImmediate);

function page({ auth = true, loadError = false, saveError = false } = {}) {
  const elements = new Map(), requests = [];
  function node(tag = "div") {
    return {
      tag, value: "", checked: false, disabled: true, dataset: {}, children: [], listeners: {},
      classList: { toggle() {} },
      append(...children) {
        this.children.push(...children);
        for (const child of children) if (child.id) elements.set(child.id, child);
      },
      replaceChildren() { this.children = []; },
      addEventListener(event, fn) { this.listeners[event] = fn; },
      querySelectorAll() {
        const all = [];
        function visit(parent) {
          for (const child of parent.children) {
            if (child.tag === "select" || child.tag === "input") all.push(child);
            visit(child);
          }
        }
        visit(this);
        return all;
      },
    };
  }
  function element(id) {
    if (!elements.has(id)) elements.set(id, node());
    return elements.get(id);
  }
  const settings = {
    count: 2, maxCount: 16, hardwareEnabled: true,
    groups: [{ id: "off", label: "Off" }, { id: "tester", label: "Tester mode" },
      { id: "frames", label: "Message frame status" }],
    leds: Array.from({ length: 16 }, (_, i) => ({ group: i === 0 ? "tester" : i === 1 ? "frames" : "off", ota: i === 0 })),
  };
  const context = {
    document: { getElementById: element, createElement: node },
    Option: function(label, value) { return { ...node("option"), label, value }; },
    I18n: { message: text => text, t: key => { assert.ok(strings[key], key); return strings[key]; }, apply() {} },
    WebAuth: { settingsHeaders: async () => auth ? { "X-CSRF-Token": "confirmed" } : null },
    fetch: async (url, options = {}) => {
      requests.push({ url, options });
      const post = options.method === "POST";
      return {
        ok: !(post ? saveError : loadError),
        json: async () => JSON.parse(JSON.stringify(settings)),
        text: async () => (post ? saveError : loadError) ? "Request failed." : "LED settings saved and applied.",
      };
    },
  };
  vm.runInNewContext(script, context);
  return {
    element, requests,
    changeCount(count) { element("led-settings-count").value = count; element("led-settings-count").listeners.change(); },
    save() { return element("led-settings-form").listeners.submit({ preventDefault() {} }); },
    posts() { return requests.filter(req => req.options.method === "POST"); },
  };
}

test("LED defaults and extension assignments persist across resizing and save", async () => {
  const view = page(); await settle();
  assert.equal(view.element("led-settings-count").disabled, false);
  assert.equal(view.element("led-group-0").value, "tester");
  assert.equal(view.element("led-ota-0").checked, true);
  assert.equal(view.element("led-group-1").value, "frames");
  assert.equal(view.element("led-ota-1").checked, false);
  view.changeCount(4);
  view.element("led-group-2").value = "tester";
  view.element("led-ota-2").checked = true;
  view.element("led-group-3").value = "frames";
  view.changeCount(2);
  view.changeCount(4);
  assert.equal(view.element("led-group-2").value, "tester");
  assert.equal(view.element("led-ota-2").checked, true);
  assert.equal(view.element("led-group-3").value, "frames");
  await view.save();
  const post = view.posts()[0];
  const body = JSON.parse(post.options.body);
  assert.equal(body.count, 4);
  assert.equal(body.leds.length, 16);
  assert.deepEqual(body.leds.slice(0, 4), [
    { group: "tester", ota: true }, { group: "frames", ota: false },
    { group: "tester", ota: true }, { group: "frames", ota: false },
  ]);
  assert.equal(post.options.headers["X-CSRF-Token"], "confirmed");
  assert.equal(view.element("led-settings-save").disabled, false);
});

test("LED count rejects zero, fractions and counts above 16", async () => {
  const view = page(); await settle();
  for (const count of [0, 17, 2.5, "not-a-number"]) {
    view.changeCount(count);
    assert.equal(view.element("led-settings-count").value, 2);
    assert.match(view.element("led-settings-message").textContent, /whole number/);
  }
  view.changeCount(16);
  await view.save();
  assert.equal(JSON.parse(view.posts()[0].options.body).count, 16);
});

test("cancelled authorization never writes and save errors are shown", async () => {
  const cancelled = page({ auth: false }); await settle();
  await cancelled.save();
  assert.equal(cancelled.posts().length, 0);
  assert.equal(cancelled.element("led-settings-save").disabled, false);
  const failed = page({ saveError: true }); await settle();
  await failed.save();
  assert.equal(failed.posts().length, 1);
  assert.equal(failed.element("led-settings-message").textContent, "Request failed.");
});

test("failed loading keeps the settings locked instead of saving assumed defaults", async () => {
  const view = page({ loadError: true }); await settle();
  assert.equal(view.element("led-settings-save").disabled, true);
  assert.equal(view.element("led-settings-message").textContent, "Request failed.");
  await view.save();
  assert.equal(view.posts().length, 0);
});

test("LED localization keys exist in both built-in catalogs", () => {
  const german = JSON.parse(fs.readFileSync(path.join(root, "external", "ota-suite", "components",
    "web_interface", "assets", "de.json"), "utf8")).strings;
  const html = fs.readFileSync(path.join(root, "main", "assets", "led_settings.html"), "utf8");
  for (const match of html.matchAll(/data-i18n="([^"]+)"/g)) {
    assert.ok(strings[match[1]], match[1]);
    assert.ok(german[match[1]], match[1]);
  }
  for (const key of ["leds.ota", "leds.invalidCount", "leds.group.off", "leds.group.tester", "leds.group.frames"]) {
    assert.ok(strings[key] && german[key], key);
  }
});

test("registered LED extension has static lifetime for deferred web-server startup", () => {
  const source = fs.readFileSync(path.join(root, "main", "led_settings.cpp"), "utf8");
  assert.match(source, /static const web_interface_extension_t extension\s*=/);
  assert.match(source, /web_interface_register_extension\(&extension\)/);
  const id = source.match(/\.id = "([^"]+)"/)[1];
  assert.match(id, /^[a-z0-9-]{1,16}$/);
});
