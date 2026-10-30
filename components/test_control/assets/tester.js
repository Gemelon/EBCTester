// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

(() => {
  const $ = id => document.getElementById(id);
  let loaded = false;
  let submitting = false;
  let latestStatus = null;

  function showMessage(text, kind = "info") {
    $("tester-message").className = `alert alert-${kind}${text ? "" : " d-none"}`;
    $("tester-message").textContent = I18n.message(text || "");
  }

  function updateControls() {
    const busy = submitting || !latestStatus || latestStatus.controlsBusy || latestStatus.recorder?.active;
    $("tester-model").disabled = Boolean(busy);
    $("tester-model-save").disabled = Boolean(busy);
  }

  function render(status) {
    latestStatus = status;
    if (!loaded) {
      $("tester-model").value = status.link?.hasModel ? String(status.link.model) : "";
      loaded = true;
    }
    const error = status.link?.error || "";
    $("tester-model-error").textContent = I18n.message(error);
    $("tester-model-error").classList.toggle("d-none", !error);
    updateControls();
  }

  async function refresh() {
    try {
      const response = await fetch("/api/tester/status", { cache: "no-store", credentials: "same-origin" });
      if (!response.ok) throw new Error(await response.text() || `HTTP ${response.status}`);
      const status = await response.json();
      document.dispatchEvent(new CustomEvent("tester-status", { detail: status }));
    } catch (error) {
      latestStatus = null;
      updateControls();
      showMessage(error.message, "danger");
    }
  }

  $("tester-model-form").addEventListener("submit", async event => {
    event.preventDefault();
    if (submitting || !latestStatus || $("tester-model-save").disabled) return;
    submitting = true;
    updateControls();
    try {
      const headers = await WebAuth.settingsHeaders();
      if (headers === null) return;
      const statusResponse = await fetch("/api/status", { cache: "no-store", credentials: "same-origin" });
      if (!statusResponse.ok) throw new Error(`Status HTTP ${statusResponse.status}`);
      const response = await fetch("/api/tester/model", {
        method: "POST", credentials: "same-origin",
        headers: { ...headers, "Content-Type": "application/json",
          "X-CSRF-Token": (await statusResponse.json()).csrfToken || "" },
        body: JSON.stringify({ model: Number($("tester-model").value) }),
      });
      const text = await response.text();
      if (!response.ok) throw new Error(text || `HTTP ${response.status}`);
      loaded = false;
      showMessage(text, "success");
      await refresh();
    } catch (error) {
      showMessage(error.message, "danger");
    } finally {
      submitting = false;
      updateControls();
    }
  });
  document.addEventListener("tester-status", event => render(event.detail));
  document.addEventListener("tester-status-error", () => {
    latestStatus = null;
    updateControls();
  });
  updateControls();
  void refresh();
})();
