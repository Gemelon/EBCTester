// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "..", "components", "test_control",
  "assets", "validation.js"), "utf8").replaceAll("export function ", "function ");

function fixture() {
  const formListeners = new Map();
  const inputs = [];
  const form = {
    querySelectorAll: () => inputs,
    addEventListener: (name, callback) => formListeners.set(name, callback),
  };
  const context = vm.createContext({
    document: { createElement: () => ({ textContent: "", setAttribute() {} }) },
  });
  vm.runInContext(source, context);
  function input(id, maximum = 5) {
    const listeners = new Map();
    const attributes = new Map([["aria-describedby", "existing-help"]]);
    const classes = new Set();
    let max = String(maximum);
    const field = {
      id, value: "0.5", min: "0.01", step: "0.01", required: true, disabled: false,
      writes: 0,
      get max() { return max; },
      set max(value) { this.writes++; max = value; },
      get validity() {
        const value = Number(this.value);
        const stepCount = (value - Number(this.min)) / Number(this.step);
        return { valid: this.disabled || (this.value !== "" && Number.isFinite(value) &&
          value >= Number(this.min) && value <= Number(this.max) &&
          Math.abs(stepCount - Math.round(stepCount)) < 1e-8) };
      },
      get validationMessage() { return this.validity.valid ? "" : `Invalid value; maximum ${this.max}.`; },
      getAttribute: name => attributes.get(name),
      setAttribute: (name, value) => attributes.set(name, value),
      classList: { toggle(name, enabled) { enabled ? classes.add(name) : classes.delete(name); } },
      insertAdjacentElement(position, message) {
        assert.equal(position, "afterend");
        this.feedback = message;
      },
      addEventListener: (name, callback) => listeners.set(name, callback),
      fire: name => listeners.get(name)(),
      focus() { this.focused = true; },
    };
    inputs.push(field);
    return field;
  }
  function submit() {
    const event = { prevented: false, stopped: false,
      preventDefault() { this.prevented = true; },
      stopImmediatePropagation() { this.stopped = true; } };
    formListeners.get("submit")(event);
    return event;
  }
  return { input, form, submit, bind: () => context.bindNumericValidation(form),
    bounds: context.setNumericBounds };
}

test("blur exposes a persistent accessible error; valid correction clears it", () => {
  const page = fixture();
  const current = page.input("charge-current");
  const validation = page.bind();
  current.value = "6";
  current.fire("input");
  assert.equal(current.feedback.textContent, "");
  current.fire("blur");
  assert.match(current.feedback.textContent, /maximum 5/);
  assert.equal(current.getAttribute("aria-invalid"), "true");
  assert.equal(current.getAttribute("aria-describedby"), "existing-help charge-current-error");
  const feedback = current.feedback;
  for (let poll = 0; poll < 100; poll++) {
    page.bounds(current, { max: 5 });
    validation.refresh();
  }
  assert.equal(current.writes, 0, "polling must not rewrite unchanged constraints");
  assert.equal(current.feedback, feedback);
  assert.match(feedback.textContent, /maximum 5/);
  assert.equal(current.value, "6");
  current.value = "4.5";
  current.fire("input");
  assert.equal(feedback.textContent, "");
  assert.equal(current.getAttribute("aria-invalid"), "false");
});

test("submit blocks invalid requests, focuses the field and allows valid forms", () => {
  const page = fixture();
  const current = page.input("charge-current");
  page.bind();
  assert.equal(page.form.noValidate, true);
  for (const value of ["", "6", "0", "1.001"]) {
    current.value = value;
    const event = page.submit();
    assert.equal(event.prevented, true);
    assert.equal(event.stopped, true, "the command submit handler must not run");
    assert.equal(current.focused, true);
    assert.notEqual(current.feedback.textContent, "");
  }
  current.value = "5";
  assert.equal(page.submit().prevented, false);
  assert.equal(current.feedback.textContent, "");
});

test("dependent cutoff, power and changed model limits revalidate touched fields", () => {
  const page = fixture();
  const cutoff = page.input("cutoff-current");
  const power = page.input("power", 85);
  power.min = "1";
  power.step = "1";
  cutoff.value = "1";
  power.value = "60";
  const validation = page.bind();
  cutoff.fire("blur");
  power.fire("blur");
  page.bounds(cutoff, { max: 0.5 });
  page.bounds(power, { max: Math.min(85, 20 * 2.5) });
  validation.refresh();
  assert.match(cutoff.feedback.textContent, /maximum 0.5/);
  assert.match(power.feedback.textContent, /maximum 50/);
  page.bounds(power, { max: Math.min(200, 40 * 2.5) });
  validation.refresh();
  assert.equal(power.feedback.textContent, "");
});

test("disabled mode fields neither block submission nor retain visible errors", () => {
  const page = fixture();
  const field = page.input("unused-field");
  const validation = page.bind();
  field.value = "";
  field.fire("blur");
  assert.notEqual(field.feedback.textContent, "");
  field.disabled = true;
  validation.refresh();
  assert.equal(field.feedback.textContent, "");
  assert.equal(page.submit().prevented, false);
});
