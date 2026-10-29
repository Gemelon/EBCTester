// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  const states = {
    disabled: ["Disabled", "text-bg-secondary"],
    not_configured: ["Not configured", "text-bg-warning"],
    connecting: ["Connecting", "text-bg-info"],
    connected: ["Connected", "text-bg-success"],
    disconnected: ["Disconnected", "text-bg-warning"],
    error: ["Connection error", "text-bg-danger"],
  };
  let loaded = false;
  let pollTimer = 0;

  function showMessage(text, kind = "info") {
    const box = $("mqtt-message");
    box.className = `alert alert-${kind}${text ? "" : " d-none"}`;
    box.textContent = I18n.message(text || "");
  }

  async function csrfToken() {
    const response = await fetch("/api/status", {
      cache: "no-store",
      credentials: "same-origin",
    });
    if (!response.ok) throw new Error(`Status HTTP ${response.status}`);
    return (await response.json()).csrfToken || "";
  }

  async function api(url, options = {}) {
    const init = { cache: "no-store", credentials: "same-origin", ...options };
    if (init.method === "POST") {
      init.headers = { ...(init.headers || {}), "X-CSRF-Token": await csrfToken() };
    }
    const response = await fetch(url, init);
    const text = await response.text();
    if (!response.ok) throw new Error(text || `HTTP ${response.status}`);
    return text;
  }

  function readForm() {
    return {
      enabled: $("mqtt-enabled").checked,
      host: $("mqtt-host").value.trim(),
      port: Number($("mqtt-port").value),
      username: $("mqtt-username").value,
      password: $("mqtt-password").value,
      topicPrefix: $("mqtt-topic-prefix").value.trim(),
      intervalSeconds: Number($("mqtt-interval").value),
      allowRemoteStart: $("mqtt-remote-start").checked,
    };
  }

  function render(status) {
    if (!loaded) {
      const settings = status.settings;
      $("mqtt-enabled").checked = settings.enabled;
      $("mqtt-host").value = settings.host;
      $("mqtt-port").value = settings.port;
      $("mqtt-username").value = settings.username;
      $("mqtt-password").value = "";
      $("mqtt-password").placeholder = settings.passwordSet ? "Saved; leave blank to keep" : "";
      $("mqtt-topic-prefix").value = settings.topicPrefix;
      $("mqtt-interval").value = settings.intervalSeconds;
      $("mqtt-remote-start").checked = settings.allowRemoteStart;
      loaded = true;
    }
    const [label, badge] = states[status.state] || states.error;
    $("mqtt-state").className = `badge ${badge}`;
    $("mqtt-state").textContent = label;
    $("mqtt-status-text").textContent = status.message;
    clearTimeout(pollTimer);
    if (status.state === "connecting" || status.state === "disconnected" || status.state === "error") {
      pollTimer = setTimeout(refresh, 3000);
    }
  }

  async function refresh() {
    if (!$("mqtt-form")) return;
    try {
      render(JSON.parse(await api("/api/mqtt/status")));
    } catch (error) {
      showMessage(error.message, "danger");
    }
  }

  async function save() {
    showMessage("");
    $("mqtt-save").disabled = true;
    $("mqtt-test").disabled = true;
    try {
      const body = JSON.stringify(readForm());
      const headers = await WebAuth.settingsHeaders();
      if (headers === null) return;
      await api("/api/mqtt/settings", {
        method: "POST",
        headers: { "Content-Type": "application/json", ...headers },
        body,
      });
      $("mqtt-password").value = "";
      loaded = false;
      showMessage("Settings saved and applied.", "success");
      await refresh();
    } catch (error) {
      showMessage(error.message, "danger");
    } finally {
      $("mqtt-save").disabled = false;
      $("mqtt-test").disabled = false;
    }
  }

  $("mqtt-form").addEventListener("submit", (event) => {
    event.preventDefault();
    void save();
  });
  $("mqtt-test").addEventListener("click", async () => {
    showMessage("");
    $("mqtt-test").disabled = true;
    try {
      showMessage(await api("/api/mqtt/test", { method: "POST" }), "success");
      await refresh();
    } catch (error) {
      showMessage(error.message, "danger");
    } finally {
      $("mqtt-test").disabled = false;
    }
  });
  void refresh();
})();
