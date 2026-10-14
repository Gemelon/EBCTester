// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

(() => {
  const $ = (id) => document.getElementById(id);

  function showMessage(text, kind = "info") {
    const box = $("debug-settings-message");
    if (!box) return;
    box.className = `alert alert-${kind}${text ? "" : " d-none"}`;
    box.textContent = text;
  }

  async function fetchJson(url) {
    const response = await fetch(url, { cache: "no-store", credentials: "same-origin" });
    const text = await response.text();
    if (!response.ok) throw new Error(text || `HTTP ${response.status}`);
    return JSON.parse(text);
  }

  async function refresh() {
    if (!$("debug-settings-form")) return;
    try {
      const status = await fetchJson("/api/debug/status");
      $("debug-settings-enabled").checked = Boolean(status.setting);
    } catch (error) {
      showMessage(error.message, "danger");
    }
  }

  async function save() {
    showMessage("");
    $("debug-settings-save").disabled = true;
    try {
      const csrfToken = (await fetchJson("/api/status")).csrfToken || "";
      const response = await fetch("/api/debug/settings", {
        method: "POST",
        cache: "no-store",
        credentials: "same-origin",
        headers: { "Content-Type": "application/json", "X-CSRF-Token": csrfToken },
        body: JSON.stringify({ enabled: $("debug-settings-enabled").checked }),
      });
      const text = await response.text();
      if (!response.ok) throw new Error(text || `HTTP ${response.status}`);
      showMessage(text, "success");
      await refresh();
    } catch (error) {
      showMessage(error.message, "danger");
    } finally {
      $("debug-settings-save").disabled = false;
    }
  }

  $("debug-settings-form").addEventListener("submit", (event) => {
    event.preventDefault();
    void save();
  });
  void refresh();
})();
