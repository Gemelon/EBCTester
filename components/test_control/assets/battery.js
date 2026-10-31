// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  let displayedLabel = "", savedLabels = [], busy = false;

  function updateDelete() {
    for (const id of ["bat-save", "bat-save-db", "bat-new", "bat-load", "bat-refresh",
      "bat-label", "bat-manufacturer", "bat-serial", "bat-date", "bat-capacity", "bat-notes", "bat-list"]) {
      $(id).disabled = busy;
    }
    $("bat-delete").disabled = busy || !displayedLabel ||
      $("bat-label").value.trim() !== displayedLabel || !savedLabels.includes(displayedLabel);
  }

  function showMessage(text, kind = "info") {
    const box = $("bat-message");
    box.className = `alert alert-${kind}${text ? "" : " d-none"}`;
    box.textContent = I18n.message(text || "");
  }

  async function api(url, options = {}) {
    const init = { cache: "no-store", credentials: "same-origin", ...options };
    if (init.method === "POST") {
      const status = await fetch("/api/status", { cache: "no-store", credentials: "same-origin" });
      if (!status.ok) throw new Error(`Status HTTP ${status.status}`);
      init.headers = { ...(init.headers || {}), "Content-Type": "application/json",
        "X-CSRF-Token": (await status.json()).csrfToken || "" };
    }
    const response = await fetch(url, init);
    const text = await response.text();
    if (!response.ok) throw new Error(I18n.message(text || `HTTP ${response.status}`));
    return text;
  }

  function fill(battery) {
    displayedLabel = battery.label || "";
    $("bat-label").value = battery.label || "";
    $("bat-manufacturer").value = battery.manufacturer || "";
    $("bat-serial").value = battery.serial || "";
    $("bat-date").value = battery.date || "";
    $("bat-capacity").value = battery.capacity > 0 ? battery.capacity : "";
    $("bat-notes").value = battery.notes || "";
    $("bat-notes-count").textContent = $("bat-notes").value.length;
    updateDelete();
  }

  function collect() {
    const capacity = Number($("bat-capacity").value);
    return {
      label: $("bat-label").value.trim(),
      manufacturer: $("bat-manufacturer").value.trim(),
      serial: $("bat-serial").value.trim(),
      date: $("bat-date").value,
      notes: $("bat-notes").value.trim(),
      capacity: Number.isFinite(capacity) && capacity > 0 ? capacity : 0,
    };
  }

  async function loadSettings() {
    try {
      fill(JSON.parse(await api("/api/tester/settings")).battery || {});
    } catch (error) {
      showMessage(I18n.f("text.0204f4bad84e", {p0: error.message}), "danger");
    }
  }

  async function loadCatalog() {
    const list = $("bat-list");
    try {
      const catalog = JSON.parse(await api("/api/tester/catalog"));
      savedLabels = catalog.databaseReady ? catalog.batteries || [] : [];
      const selected = list.value;
      list.replaceChildren();
      for (const label of catalog.batteries || []) list.append(new Option(label, label, false, label === selected));
      if (!catalog.databaseReady) showMessage(I18n.t("text.56b2a2978734"), "warning");
    } catch (error) {
      savedLabels = [];
      showMessage(I18n.f("text.368e46e190fa", {p0: error.message}), "danger");
    }
    updateDelete();
  }

  async function post(body, reload) {
    if (busy) return;
    showMessage("");
    try {
      showMessage(await api("/api/tester/battery", { method: "POST", body: JSON.stringify(body) }), "success");
      if (reload || body.action === "save" || body.action === "save_db") await loadSettings();
      await loadCatalog();
    } catch (error) {
      showMessage(error.message, "danger");
    }
  }

  $("bat-form").addEventListener("submit", (event) => {
    event.preventDefault();
    void post({ action: "save", battery: collect() }, false);
  });
  $("bat-save-db").addEventListener("click", () => {
    if (!$("bat-label").value.trim()) {
      showMessage(I18n.t("text.c4bed19989ed"), "warning");
      return;
    }
    void post({ action: "save_db", battery: collect() }, false);
  });
  $("bat-new").addEventListener("click", () => {
    if (busy) return;
    fill({});
    showMessage(I18n.t("text.25023e5f547b"), "info");
    $("bat-label").focus();
  });
  $("bat-label").addEventListener("input", updateDelete);
  $("bat-delete").addEventListener("click", async () => {
    if ($("bat-delete").disabled) return;
    const label = displayedLabel;
    busy = true;
    updateDelete();
    try {
      if (!confirm(I18n.f("battery.deleteConfirm", { p0: label }))) return;
      const headers = await WebAuth.settingsHeaders();
      if (headers === null) return;
      showMessage(await api("/api/tester/battery", {
        method: "POST", headers, body: JSON.stringify({ action: "delete_db", label }),
      }), "success");
      fill({});
      await loadCatalog();
      window.dispatchEvent(new CustomEvent("test-runs-changed", { detail: {} }));
    } catch (error) {
      showMessage(error.message, "danger");
    } finally {
      busy = false;
      updateDelete();
    }
  });
  $("bat-load").addEventListener("click", () => {
    if (!$("bat-list").value) {
      showMessage(I18n.t("text.fecfdb98d2e6"), "warning");
      return;
    }
    void post({ action: "load_db", label: $("bat-list").value }, true);
  });
  $("bat-list").addEventListener("dblclick", () => $("bat-load").click());
  $("bat-refresh").addEventListener("click", () => void loadCatalog());
  $("bat-notes").addEventListener("input", () => {
    $("bat-notes-count").textContent = $("bat-notes").value.length;
  });

  void loadSettings();
  void loadCatalog();
})();
