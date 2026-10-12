"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  const STATES = {
    idle: [I18n.t("text.8d4d4830b4da"), "text-bg-secondary"],
    testing: [I18n.t("text.16df7fa183be"), "text-bg-info"],
    syncing: [I18n.t("text.da7f91ee2818"), "text-bg-primary"],
    ok: ["OK", "text-bg-success"],
    error: [I18n.t("text.f6cc01cb7edb"), "text-bg-danger"],
  };
  let formLoaded = false;
  let pollTimer = 0;

  function showMessage(text, kind = "info") {
    const box = $("mdb-message");
    if (!box) return;
    box.className = `alert alert-${kind}${text ? "" : " d-none"}`;
    box.textContent = I18n.message(text || "");
  }

  async function csrfToken() {
    const response = await fetch("/api/status", { cache: "no-store", credentials: "same-origin" });
    if (!response.ok) throw new Error(`Status HTTP ${response.status}`);
    return (await response.json()).csrfToken || "";
  }

  async function api(url, options = {}) {
    const init = { cache: "no-store", credentials: "same-origin", ...options };
    if (init.method === "POST") init.headers = { ...(init.headers || {}), "X-CSRF-Token": await csrfToken() };
    const response = await fetch(url, init);
    const text = await response.text();
    if (!response.ok) throw new Error(I18n.message(text || `HTTP ${response.status}`));
    return text;
  }

  const formatTime = (epoch) => (epoch > 0 ? new Date(epoch * 1000).toLocaleString(I18n.locale) : "-");

  function fillForm(settings) {
    $("mdb-host").value = settings.host || "";
    $("mdb-port").value = settings.port || 3306;
    $("mdb-database").value = settings.database || "";
    $("mdb-user").value = settings.user || "";
    $("mdb-enabled").checked = Boolean(settings.enabled);
    $("mdb-interval").value = settings.intervalMinutes ?? 15;
    $("mdb-password").value = "";
    $("mdb-password").placeholder = settings.passwordSet ? I18n.t("text.53f27e0581d7") : I18n.t("text.19e6b450a04d");
  }

  function render(status) {
    if (!formLoaded) {
      fillForm(status.settings || {});
      formLoaded = true;
    }
    const [label, badge] = STATES[status.state] || STATES.idle;
    $("mdb-state").className = `badge ${badge}`;
    $("mdb-state").textContent = label;
    let note = I18n.message(status.message || "");
    if (!status.networkConnected) note = note ? I18n.f("text.cb57b2480ed4", {p0: note}) : I18n.t("text.d101b884e7cc");
    if (!status.databaseReady) note = I18n.f("text.a8aa67a4542a", {p0: note});
    $("mdb-status-text").textContent = note;
    $("mdb-last-success").textContent = formatTime(status.lastSuccess);
    $("mdb-last-result").textContent = status.lastAttempt > 0 || status.lastSuccess > 0
      ? I18n.f("text.d7e948e939ce", {p0: status.lastRuns, p1: status.lastMeasurements}) +
        (status.pendingRuns ? I18n.f("text.8a11ce305408", {p0: status.pendingRuns}) : "")
      : "-";
    $("mdb-server").textContent = status.serverVersion ? `MariaDB ${status.serverVersion}` : "-";
    for (const id of ["mdb-test", "mdb-sync", "mdb-full-sync", "mdb-save"]) $(id).disabled = status.busy;
    clearTimeout(pollTimer);
    if (status.busy) pollTimer = setTimeout(refresh, 1500);
  }

  async function refresh() {
    if (!$("mdb-form")) return;
    try {
      render(JSON.parse(await api("/api/mariadb/status")));
    } catch (error) {
      showMessage(I18n.f("text.96fdcc657f7a", {p0: error.message}), "danger");
    }
  }

  async function post(url, body, confirmText) {
    if (confirmText && !window.confirm(confirmText)) return;
    showMessage("");
    try {
      const options = { method: "POST" };
      if (body) {
        options.headers = { "Content-Type": "application/json" };
        options.body = JSON.stringify(body);
      }
      showMessage(await api(url, options), "success");
    } catch (error) {
      showMessage(error.message, "danger");
    }
    await refresh();
  }

  $("mdb-form").addEventListener("submit", (event) => {
    event.preventDefault();
    formLoaded = false;
    void post("/api/mariadb/settings", {
      enabled: $("mdb-enabled").checked,
      host: $("mdb-host").value.trim(),
      port: Number($("mdb-port").value),
      database: $("mdb-database").value.trim(),
      user: $("mdb-user").value.trim(),
      password: $("mdb-password").value,
      intervalMinutes: Number($("mdb-interval").value),
    });
  });
  $("mdb-test").addEventListener("click", () => void post("/api/mariadb/test"));
  $("mdb-sync").addEventListener("click", () => void post("/api/mariadb/sync"));
  $("mdb-full-sync").addEventListener("click", () => void post("/api/mariadb/full-sync", null,
    I18n.t("text.9808be8ac09f")));

  void refresh();
})();
