// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0
"use strict";

(() => {
  const $ = id => document.getElementById(id);
  let settings, busy = true;

  function message(text, kind = "info") {
    $("led-settings-message").className = `alert alert-${kind}${text ? "" : " d-none"}`;
    $("led-settings-message").textContent = I18n.message(text);
  }

  function lock(locked) {
    busy = locked;
    $("led-settings-count").disabled = locked;
    $("led-settings-save").disabled = locked;
    $("led-settings-assignments").querySelectorAll("select,input").forEach(node => { node.disabled = locked; });
  }

  function collectRows() {
    if (!settings) return;
    for (let i = 0; i < settings.count; ++i) {
      settings.leds[i] = { group: $(`led-group-${i}`).value, ota: $(`led-ota-${i}`).checked };
    }
  }

  function renderRows() {
    const container = $("led-settings-assignments");
    container.replaceChildren();
    for (let i = 0; i < settings.count; ++i) {
      const row = document.createElement("div");
      row.className = "row g-2 align-items-center mb-3";
      const label = document.createElement("label");
      label.className = "col-sm-2 col-form-label";
      label.htmlFor = `led-group-${i}`;
      label.textContent = `LED ${i + 1}`;
      const normal = document.createElement("div");
      normal.className = "col-sm-6";
      const select = document.createElement("select");
      select.id = `led-group-${i}`;
      select.className = "form-select";
      for (const group of settings.groups) {
        const option = new Option(I18n.message(group.label), group.id);
        option.dataset.i18n = `leds.group.${group.id}`;
        select.append(option);
      }
      select.value = settings.leds[i].group;
      normal.append(select);
      const override = document.createElement("div");
      override.className = "col-sm-4";
      const otaLabel = document.createElement("label");
      otaLabel.className = "form-check-label";
      otaLabel.htmlFor = `led-ota-${i}`;
      otaLabel.dataset.i18n = "leds.ota";
      otaLabel.textContent = I18n.t("leds.ota");
      const check = document.createElement("input");
      check.id = `led-ota-${i}`;
      check.type = "checkbox";
      check.className = "form-check-input me-2";
      check.checked = settings.leds[i].ota;
      override.append(check, otaLabel);
      row.append(label, normal, override);
      container.append(row);
    }
    I18n.apply(container);
  }

  async function load() {
    try {
      const response = await fetch("/api/leds/settings", { cache: "no-store", credentials: "same-origin" });
      if (!response.ok) throw new Error(await response.text());
      settings = await response.json();
      $("led-settings-count").max = settings.maxCount;
      $("led-settings-count").value = settings.count;
      $("led-settings-inactive").classList.toggle("d-none", settings.hardwareEnabled);
      renderRows();
      lock(false);
    } catch (error) {
      message(error.message, "danger");
    }
  }

  $("led-settings-count").addEventListener("change", () => {
    if (busy || !settings) return;
    const count = Number($("led-settings-count").value);
    if (!Number.isInteger(count) || count < 1 || count > settings.maxCount) {
      message(I18n.t("leds.invalidCount"), "warning");
      $("led-settings-count").value = settings.count;
      return;
    }
    collectRows();
    settings.count = count;
    renderRows();
  });

  $("led-settings-form").addEventListener("submit", async event => {
    event.preventDefault();
    if (busy || !settings) return;
    collectRows();
    lock(true);
    message("");
    try {
      const headers = await WebAuth.settingsHeaders();
      if (headers === null) return;
      const response = await fetch("/api/leds/settings", {
        method: "POST", cache: "no-store", credentials: "same-origin",
        headers: { "Content-Type": "application/json", ...headers },
        body: JSON.stringify({ count: settings.count, leds: settings.leds }),
      });
      const text = await response.text();
      if (!response.ok) throw new Error(text || `HTTP ${response.status}`);
      message(text, "success");
    } catch (error) {
      message(error.message, "danger");
    } finally {
      lock(false);
    }
  });
  void load();
})();
