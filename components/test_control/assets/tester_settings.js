// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

(() => {
  const $ = (id) => document.getElementById(id);

  function showMessage(text, kind = "info") {
    const box = $("tester-settings-message");
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
    if (!$("tester-settings-form")) return;
    try {
      const settings = await fetchJson("/api/tester/settings");
      $("tester-settings-model").value = settings.testerModel || "A20";
    } catch (error) {
      showMessage(error.message, "danger");
    }
  }

  async function save() {
    showMessage("");
    $("tester-settings-save").disabled = true;
    try {
      const csrfToken = (await fetchJson("/api/status")).csrfToken || "";
      const response = await fetch("/api/tester/model", {
        method: "POST",
        cache: "no-store",
        credentials: "same-origin",
        headers: { "Content-Type": "application/json", "X-CSRF-Token": csrfToken },
        body: JSON.stringify({ model: $("tester-settings-model").value }),
      });
      const text = await response.text();
      if (!response.ok) throw new Error(text || `HTTP ${response.status}`);
      showMessage(text, "success");
    } catch (error) {
      showMessage(error.message, "danger");
    } finally {
      await refresh();
      $("tester-settings-save").disabled = false;
    }
  }

  $("tester-settings-form").addEventListener("submit", (event) => {
    event.preventDefault();
    void save();
  });
  void refresh();
})();
