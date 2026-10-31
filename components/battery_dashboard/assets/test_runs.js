// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";
(() => {
  const $ = (id) => document.getElementById(id);
  const statusKeys = {
    RUNNING: "text.f85fd2bc5619", COMPLETED: "text.38ee859a969e",
    STOPPED: "text.641c1c76e38a", INTERRUPTED: "text.d8135c8fc9ca", ERROR: "text.f6cc01cb7edb",
  };
  let page = 0;
  let catalog = { runs: [], batteries: [], total: 0, pageSize: 50 };
  let selectedId = "";
  let busy = false;
  const stateLabel = (run) => statusKeys[run.status] ? I18n.t(statusKeys[run.status]) : run.status;
  const dateLabel = (text) => {
    const ms = Date.parse(text.replace(" ", "T") + "Z");
    return Number.isFinite(ms) ? new Date(ms).toLocaleString(I18n.locale) : "-";
  };
  const defaultName = (run) =>
    `${dateLabel(run.started)} · ${run.battery || "?"} · ${run.plan || I18n.t("text.b01429e91a75")} · ${stateLabel(run)}`;
  const nameLabel = (run) => run.name || defaultName(run);
  const selected = () => catalog.runs.find((run) => run.id === selectedId);

  function message(text, error = false) {
    $("runs-message").textContent = text;
    $("runs-message").className = `alert ${error ? "alert-danger" : "alert-info"}${text ? "" : " d-none"}`;
  }
  function buttons() {
    const run = selected();
    const locked = busy || !run || run.status === "RUNNING" || run.protected === true;
    $("runs-rename").disabled = locked;
    $("runs-delete").disabled = locked;
    $("runs-selection").textContent = run ? (run.status === "RUNNING" || run.protected ? I18n.t("runs.protected") : nameLabel(run)) : "";
    $("runs-previous").disabled = busy || page === 0;
    $("runs-next").disabled = busy || (page + 1) * catalog.pageSize >= catalog.total;
    for (const id of ["runs-apply", "runs-refresh", "runs-reset"]) $(id).disabled = busy;
  }
  function cell(row, text) {
    const td = document.createElement("td");
    td.textContent = text;
    row.append(td);
  }
  function render() {
    const battery = $("runs-battery").value;
    $("runs-battery").replaceChildren();
    const all = document.createElement("option");
    all.value = ""; all.textContent = I18n.t("runs.all");
    $("runs-battery").append(all);
    for (const item of catalog.batteries) {
      const option = document.createElement("option");
      option.value = String(item.id); option.textContent = item.label;
      $("runs-battery").append(option);
    }
    $("runs-battery").value = battery;
    if (!selected()) selectedId = "";
    $("runs-rows").replaceChildren();
    for (const run of catalog.runs) {
      const row = document.createElement("tr");
      row.className = run.id === selectedId ? "table-active" : "";
      const td = document.createElement("td");
      const radio = document.createElement("input");
      radio.type = "radio"; radio.name = "test-run-selection";
      radio.className = "form-check-input";
      radio.checked = run.id === selectedId;
      radio.setAttribute("aria-label", nameLabel(run));
      radio.addEventListener("change", () => {
        if (busy) return;
        selectedId = run.id;
        render();
      });
      row.addEventListener("click", () => {
        if (busy) return;
        selectedId = run.id;
        render();
      });
      td.append(radio); row.append(td);
      cell(row, nameLabel(run)); cell(row, run.battery || "-");
      cell(row, dateLabel(run.started)); cell(row, run.plan || I18n.t("text.b01429e91a75"));
      cell(row, stateLabel(run));
      $("runs-rows").append(row);
    }
    if (!catalog.runs.length) {
      const row = document.createElement("tr"), td = document.createElement("td");
      td.colSpan = 6; td.textContent = I18n.t("runs.empty");
      row.append(td); $("runs-rows").append(row);
    }
    $("runs-page").textContent = I18n.f("runs.page", {
      p0: page + 1, p1: Math.max(1, Math.ceil(catalog.total / catalog.pageSize)), p2: catalog.total,
    });
    buttons();
  }
  function boundary(value, after) {
    if (!value) return "";
    const date = new Date(`${value}T00:00:00`);
    if (after) date.setDate(date.getDate() + 1);
    return date.toISOString().slice(0, 19).replace("T", " ");
  }
  async function request(url, options = {}) {
    const response = await fetch(url, {
      cache: "no-store", credentials: "same-origin", signal: AbortSignal.timeout(10000), ...options,
    });
    if (!response.ok) throw new Error(I18n.message(await response.text()));
    return response;
  }
  async function load() {
    if (busy) return;
    const from = $("runs-from").value, to = $("runs-to").value;
    if (from && to && from > to) { message(I18n.t("runs.invalidDates"), true); return; }
    const search = $("runs-search").value.trim();
    if (new TextEncoder().encode(search).length > 100) {
      message(I18n.t("runs.searchLimit"), true); return;
    }
    busy = true; buttons();
    try {
      const query = new URLSearchParams({
        page: String(page), battery: $("runs-battery").value, status: $("runs-status").value,
        from: boundary(from, false), until: boundary(to, true), search,
      });
      catalog = await (await request(`/api/test-runs?${query}`)).json();
      if (page > 0 && page * catalog.pageSize >= catalog.total) {
        page = Math.max(0, Math.ceil(catalog.total / catalog.pageSize) - 1);
        query.set("page", String(page));
        catalog = await (await request(`/api/test-runs?${query}`)).json();
      }
      render();
    } catch (error) {
      catalog = { runs: [], batteries: [], total: 0, pageSize: 50 };
      selectedId = "";
      $("runs-rows").replaceChildren();
      message(error.message, true);
    } finally { busy = false; buttons(); }
  }
  async function change(action, run, name) {
    if (busy || run.status === "RUNNING" || run.protected) return;
    busy = true; buttons();
    let changed = false;
    try {
      const headers = await WebAuth.settingsHeaders();
      if (headers === null) return;
      headers["Content-Type"] = "application/json";
      const response = await request(`/api/test-runs/${action}`, {
        method: "POST", headers, body: JSON.stringify({ id: run.id, ...(action === "rename" ? { name } : {}) }),
      });
      message(I18n.message(await response.text()));
      changed = true;
      window.dispatchEvent(new CustomEvent("test-runs-changed", { detail: { id: run.id, action } }));
    } catch (error) { message(error.message, true); }
    finally { busy = false; buttons(); }
    if (changed) await load();
  }
  $("runs-filter").addEventListener("submit", (event) => {
    event.preventDefault(); if (busy) return; page = 0; selectedId = ""; message(""); void load();
  });
  $("runs-refresh").addEventListener("click", () => { message(""); void load(); });
  $("runs-reset").addEventListener("click", () => {
    if (busy) return;
    for (const id of ["runs-search", "runs-battery", "runs-status", "runs-from", "runs-to"]) $(id).value = "";
    page = 0; selectedId = ""; message(""); void load();
  });
  for (const [id, delta] of [["runs-previous", -1], ["runs-next", 1]]) {
    $(id).addEventListener("click", () => {
      if ($(id).disabled) return; page += delta; selectedId = ""; message(""); void load();
    });
  }
  $("runs-delete").addEventListener("click", () => {
    const run = selected();
    if ($("runs-delete").disabled || !run) return;
    if (confirm(I18n.f("runs.confirmDelete", { p0: nameLabel(run), p1: dateLabel(run.started) })))
      void change("delete", run);
  });
  let renameRun = null;
  $("runs-rename").addEventListener("click", () => {
    const run = selected();
    if ($("runs-rename").disabled || !run) return;
    renameRun = run;
    $("runs-name").value = nameLabel(run);
    $("runs-name-dialog").showModal(); $("runs-name").focus();
  });
  $("runs-name-cancel").addEventListener("click", () => $("runs-name-dialog").close());
  $("runs-name-form").addEventListener("submit", (event) => {
    event.preventDefault();
    const name = $("runs-name").value.trim();
    if ([...name].length > 100 || /[\u0000-\u001f\u007f-\u009f]/u.test(name)) {
      $("runs-name").setCustomValidity(I18n.t("runs.invalidName")); $("runs-name").reportValidity(); return;
    }
    $("runs-name-dialog").close();
    if (renameRun) void change("rename", renameRun, name);
  });
  $("runs-name").addEventListener("input", () => $("runs-name").setCustomValidity(""));
  document.addEventListener("languagechange", render);
  void load();
})();
