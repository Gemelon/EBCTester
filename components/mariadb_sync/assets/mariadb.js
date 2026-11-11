// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

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
  const LOCAL_TEXT = {
    en: {
      schemaTitle: "MariaDB database structure needs attention",
      schemaQuestion: "Create or repair the required tables and indexes now?",
      schemaYes: "Set up database structure",
      schemaNo: "No, disable MariaDB",
      migrationTitle: "One-time MariaDB schema setup",
      migrationRights: "Creating tables and indexes requires additional MariaDB privileges, typically CREATE, ALTER, and INDEX.",
      migrationTemporary: "These separate administrator credentials are used only for this migration and are not saved on the device. Existing data is not deleted. Incompatible columns are not rewritten.",
      migrationUser: "Migration user",
      migrationPassword: "Migration password",
      migrationRun: "Create structure",
      cancel: "Cancel",
      disabledConfirm: "Disable MariaDB backups because the database migration was declined?",
      migrationStarted: "One-time schema migration started.",
      disabledMessage: "MariaDB disabled.",
    },
    de: {
      schemaTitle: "Die MariaDB-Datenbankstruktur muss eingerichtet werden",
      schemaQuestion: "Die benoetigten Tabellen und Indizes jetzt anlegen oder ergaenzen?",
      schemaYes: "Datenbankstruktur einrichten",
      schemaNo: "Nein, MariaDB deaktivieren",
      migrationTitle: "Einmalige Einrichtung des MariaDB-Schemas",
      migrationRights: "Das Erstellen von Tabellen und Indizes benoetigt zusaetzliche MariaDB-Rechte, normalerweise CREATE, ALTER und INDEX.",
      migrationTemporary: "Diese separaten Administrator-Zugangsdaten werden nur fuer diese Migration verwendet und nicht auf dem Geraet gespeichert. Bestehende Daten werden nicht geloescht. Inkompatible Spalten werden nicht umgeschrieben.",
      migrationUser: "Migrations-Benutzer",
      migrationPassword: "Migrations-Passwort",
      migrationRun: "Struktur anlegen",
      cancel: "Abbrechen",
      disabledConfirm: "MariaDB-Sicherungen deaktivieren, weil die Datenbankmigration abgelehnt wurde?",
      migrationStarted: "Einmalige Schema-Migration gestartet.",
      disabledMessage: "MariaDB wurde deaktiviert.",
    },
  };
  let formLoaded = false;
  let pollTimer = 0;
  const localText = (key) => (LOCAL_TEXT[I18n.code] || LOCAL_TEXT.en)[key] || LOCAL_TEXT.en[key] || key;

  function applyLocalText() {
    document.querySelectorAll("[data-mdb-text]").forEach((element) => {
      element.textContent = localText(element.dataset.mdbText);
    });
  }

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
    const schemaAlert = $("mdb-schema-alert");
    const showSchemaSetup = ["needsMigration", "incompatible"].includes(status.schemaState);
    schemaAlert.classList.toggle("d-none", !showSchemaSetup);
    $("mdb-schema-message").textContent = I18n.message(status.schemaMessage || "");
    $("mdb-last-success").textContent = formatTime(status.lastSuccess);
    $("mdb-last-result").textContent = status.lastAttempt > 0 || status.lastSuccess > 0
      ? I18n.f("text.d7e948e939ce", {p0: status.lastRuns, p1: status.lastMeasurements}) +
        (status.pendingRuns ? I18n.f("text.8a11ce305408", {p0: status.pendingRuns}) : "")
      : "-";
    $("mdb-server").textContent = status.serverVersion ? `MariaDB ${status.serverVersion}` : "-";
    for (const id of ["mdb-test", "mdb-sync", "mdb-full-sync", "mdb-save",
      "mdb-migration-open", "mdb-migration-decline"]) $(id).disabled = status.busy;
    if (status.schemaState === "disabled") {
      for (const id of ["mdb-test", "mdb-sync", "mdb-full-sync"]) $(id).disabled = true;
    }
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
      if (url === "/api/mariadb/settings" || url === "/api/mariadb/decline-migration") {
        options.headers = await WebAuth.settingsHeaders();
        if (options.headers === null) return;
      }
      if (body) {
        options.headers = { ...options.headers, "Content-Type": "application/json" };
        options.body = JSON.stringify(body);
      }
      showMessage(await api(url, options), "success");
      if (url === "/api/mariadb/settings" || url === "/api/mariadb/decline-migration") {
        formLoaded = false;
        if (url === "/api/mariadb/settings") $("mdb-password").value = "";
      }
    } catch (error) {
      showMessage(error.message, "danger");
    }
    await refresh();
  }

  $("mdb-form").addEventListener("submit", (event) => {
    event.preventDefault();
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

  $("mdb-migration-open").addEventListener("click", () => {
    $("mdb-migration-dialog").showModal();
  });
  $("mdb-migration-cancel").addEventListener("click", () => {
    $("mdb-migration-dialog").close();
  });
  $("mdb-migration-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const user = $("mdb-migration-user").value.trim();
    const password = $("mdb-migration-password").value;
    showMessage("");
    try {
      const headers = await WebAuth.settingsHeaders();
      if (headers === null) return;
      const message = await api("/api/mariadb/migrate", {
        method: "POST",
        headers: { ...headers, "Content-Type": "application/json" },
        body: JSON.stringify({ user, password }),
      });
      $("mdb-migration-dialog").close();
      $("mdb-migration-form").reset();
      showMessage(message || localText("migrationStarted"), "success");
    } catch (error) {
      showMessage(error.message, "danger");
    } finally {
      $("mdb-migration-password").value = "";
    }
    await refresh();
  });
  $("mdb-migration-decline").addEventListener("click", () => {
    if (!window.confirm(localText("disabledConfirm"))) return;
    void post("/api/mariadb/decline-migration", null);
  });

  if (I18n.ready && typeof I18n.ready.then === "function") {
    void I18n.ready.then(() => {
      applyLocalText();
      void refresh();
    });
  } else {
    void refresh();
  }
})();
