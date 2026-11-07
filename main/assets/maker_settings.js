// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";
(() => {
  const $ = (id) => document.getElementById(id);
  let fields = [];
  function message(text, error = false) {
    $("maker-message").textContent = text;
    $("maker-message").className = `alert mt-3 alert-${error ? "danger" : "info"}`;
  }
  function render(configuration) {
    fields = configuration.fields;
    $("maker-fields").replaceChildren();
    for (const section of ["tester", "ota", "sd"]) {
      const fieldset = document.createElement("fieldset");
      fieldset.className = "row g-3 mb-4";
      const legend = document.createElement("legend");
      legend.className = "h5";
      legend.textContent = { tester: "EBCTester", ota: "Network, OTA and web", sd: "SD card" }[section];
      fieldset.append(legend);
      for (const field of fields.filter((item) => item.section === section)) {
        const wrapper = document.createElement("div");
        wrapper.className = "col-md-6";
        const label = document.createElement("label");
        label.htmlFor = `maker-${field.name}`;
        label.className = "form-label";
        label.textContent = field.label;
        const input = document.createElement(field.type === "bus" ? "select" : "input");
        input.id = `maker-${field.name}`;
        input.className = field.type === "checkbox" ? "form-check-input d-block" : "form-control";
        if (field.type === "bus") {
          for (const [value, text] of [[0, "4-bit SDMMC"], [1, "SPI"]]) {
            const option = document.createElement("option");
            option.value = String(value);
            option.textContent = text;
            input.append(option);
          }
          input.value = String(field.value);
        } else {
          input.type = field.type;
          if (field.type === "checkbox") input.checked = field.value;
          else if (field.type === "password") {
            input.autocomplete = "new-password";
            input.maxLength = field.maxLength;
            input.placeholder = field.configured ? "Configured; leave empty to keep" : "Not configured";
          } else if (field.type === "number") {
            input.min = field.min;
            input.max = field.max;
            input.step = "1";
            input.required = true;
            input.value = field.value;
          } else {
            input.maxLength = field.maxLength;
            input.value = field.value;
          }
        }
        wrapper.append(label, input);
        fieldset.append(wrapper);
      }
      $("maker-fields").append(fieldset);
    }
    $("maker-active").checked = configuration.active;
    $("maker-save").disabled = false;
  }
  async function load() {
    try {
      const response = await fetch("/api/maker/config", { cache: "no-store", credentials: "same-origin" });
      if (!response.ok) throw new Error(await response.text());
      render(await response.json());
    } catch (error) {
      message(error.message, true);
    }
  }
  $("maker-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!window.confirm("Save this hardware configuration and restart the device?")) return;
    $("maker-save").disabled = true;
    let restarting = false;
    try {
      const headers = await WebAuth.settingsHeaders();
      if (headers === null) return;
      const body = { active: $("maker-active").checked, tester: {}, ota: {}, sd: {} };
      for (const field of fields) {
        const input = $(`maker-${field.name}`);
        body[field.section][field.name] = field.type === "checkbox" ? input.checked :
          field.type === "number" || field.type === "bus" ? Number(input.value) : input.value;
      }
      const response = await fetch("/api/maker/config", {
        method: "POST", cache: "no-store", credentials: "same-origin",
        headers: { "Content-Type": "application/json", ...headers }, body: JSON.stringify(body),
      });
      const text = await response.text();
      if (!response.ok) throw new Error(text);
      restarting = true;
      for (const field of fields.filter((item) => item.type === "password")) $(`maker-${field.name}`).value = "";
      message(text);
    } catch (error) {
      message(error.message, true);
    } finally {
      $("maker-save").disabled = restarting;
    }
  });
  void load();
})();
