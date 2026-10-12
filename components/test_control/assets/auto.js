"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  // scale converts UI units to the integer encoding stored in the plan.
  const MODES = {
    1: { name: "D-CC", fields: [[I18n.t("text.bb39ee18acd6"), 100, 0.01, 20], [I18n.t("text.7cf603e56ff8"), 100, 0.01, 30], [I18n.t("text.2cba9409833c"), 1, 0, 239]],
      hint: I18n.t("text.9bb42763fbad") },
    2: { name: "D-CP", fields: [[I18n.t("text.c9d1515a58db"), 1, 1, 2000], [I18n.t("text.7cf603e56ff8"), 100, 0.01, 30], [I18n.t("text.2cba9409833c"), 1, 0, 239]],
      hint: I18n.t("text.e97e2f42401b") },
    3: { name: "C-CV", fields: [[I18n.t("text.bb39ee18acd6"), 100, 0.01, 20], [I18n.t("text.276ed7b6e344"), 100, 0.01, 30], [I18n.t("text.b2c884412725"), 100, 0.01, 20]],
      hint: I18n.t("text.db5db847724b") },
    4: { name: I18n.t("text.233631694afc"), fields: [null, null, ["Dauer (min)", 1, 1, 239]],
      hint: I18n.t("text.380be0c6ecf0") },
    5: { name: I18n.t("text.dfd72aef2879"), fields: [null, null, [I18n.t("text.3ffc99050907"), 1, 1, 239]],
      hint: I18n.t("text.67ffa0001c95") },
  };
  const STATES = {
    idle: [I18n.t("text.8d4d4830b4da"), "secondary"], running: [I18n.t("text.f85fd2bc5619"), "success"], complete: [I18n.t("text.38ee859a969e"), "primary"],
    interrupted: [I18n.t("text.d8135c8fc9ca"), "warning"], failed: [I18n.t("text.f6cc01cb7edb"), "danger"],
  };
  let plan = { name: "", steps: [] };
  let selected = -1;
  let running = false;
  let timer = 0;
  let submitting = false;
  let latestStatus = null;

  function updateStartButton() {
    const link = latestStatus?.link;
    $("auto-start").disabled = submitting || !latestStatus || latestStatus.controlsBusy ||
      link?.active || link?.pending || !link?.connected || !link?.hasFrame ||
      latestStatus.plan?.state === "running" || latestStatus.plan?.state === "interrupted";
  }

  function showMessage(text, kind = "info") {
    const box = $("auto-message");
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

  const post = (url, body) => api(url, { method: "POST", body: JSON.stringify(body) });

  function describe(step, index) {
    const fields = MODES[step[0]]?.fields || [];
    if (!fields.length) return "?";
    const field = fields[index];
    if (!field) return "";
    const value = step[index + 1] / field[1];
    return `${field[1] === 100 ? value.toFixed(2) : value} ${field[0].replace(/ *\(([^,)]*).*$/, " $1")}`.trim();
  }

  function renderSteps() {
    const body = $("auto-steps");
    body.replaceChildren();
    plan.steps.forEach((step, index) => {
      const row = document.createElement("tr");
      row.className = index === selected ? "table-active" : "";
      row.dataset.index = String(index);
      const cells = [String(index + 1), MODES[step[0]]?.name || "?", describe(step, 0), describe(step, 1), describe(step, 2)];
      for (const text of cells) {
        const cell = document.createElement("td");
        cell.textContent = text;
        row.append(cell);
      }
      row.addEventListener("click", () => select(index));
      body.append(row);
    });
    $("auto-steps-empty").classList.toggle("d-none", plan.steps.length > 0);
    $("auto-name").value = document.activeElement === $("auto-name") ? $("auto-name").value : plan.name;
    updateButtons();
  }

  function updateButtons() {
    const has = selected >= 0 && selected < plan.steps.length;
    for (const id of ["auto-add", "auto-replace", "auto-up", "auto-down", "auto-delete", "auto-clear", "auto-db-load", "auto-name"]) {
      $(id).disabled = running;
    }
    $("auto-replace").disabled ||= !has;
    $("auto-up").disabled ||= !has || selected === 0;
    $("auto-down").disabled ||= !has || selected === plan.steps.length - 1;
    $("auto-delete").disabled ||= !has;
    $("auto-add").disabled ||= plan.steps.length >= 10;
  }

  function applyMode() {
    const mode = MODES[$("auto-mode").value];
    mode.fields.forEach((field, index) => {
      const n = index + 1;
      $(`auto-field-${n}`).classList.toggle("d-none", !field);
      if (!field) return;
      const input = $(`auto-value-${n}`);
      $(`auto-label-${n}`).textContent = field[0];
      input.min = String(field[2]);
      input.max = String(field[3]);
      input.step = field[1] === 100 ? "0.01" : "1";
    });
    $("auto-hint").textContent = mode.hint;
  }

  function select(index) {
    selected = index === selected ? -1 : index;
    const step = plan.steps[selected];
    if (step) {
      $("auto-mode").value = String(step[0]);
      applyMode();
      MODES[step[0]].fields.forEach((field, i) => {
        if (field) $(`auto-value-${i + 1}`).value = String(step[i + 1] / field[1]);
      });
    }
    renderSteps();
  }

  function readEditor() {
    const modeId = Number($("auto-mode").value);
    const step = [modeId, 0, 0, 0];
    for (const [index, field] of MODES[modeId].fields.entries()) {
      if (!field) continue;
      const input = $(`auto-value-${index + 1}`);
      const value = Number(input.value);
      if (input.value === "" || !Number.isFinite(value) || value < field[2] || value > field[3]) {
        throw new Error(I18n.f("text.91bfbfe9b35f", {p0: field[0], p1: field[2], p2: field[3]}));
      }
      step[index + 1] = Math.round(value * field[1]);
    }
    return step;
  }

  async function savePlan(steps, nextSelection) {
    const name = $("auto-name").value.trim();
    try {
      await post("/api/tester/plan", { name, steps });
      plan = { name, steps };
      selected = nextSelection;
      showMessage("");
    } catch (error) {
      showMessage(error.message, "danger");
    }
    renderSteps();
  }

  function edit(change) {
    try {
      const steps = plan.steps.map((step) => step.slice());
      const next = change(steps);
      void savePlan(steps, next);
    } catch (error) {
      showMessage(error.message, "warning");
    }
  }

  async function loadSettings() {
    try {
      const settings = JSON.parse(await api("/api/tester/settings"));
      plan = { name: settings.plan?.name || "", steps: settings.plan?.steps || [] };
      selected = -1;
      renderSteps();
    } catch (error) {
      showMessage(I18n.f("text.21e3b47979e4", {p0: error.message}), "danger");
    }
  }

  async function loadCatalog() {
    try {
      const catalog = JSON.parse(await api("/api/tester/catalog"));
      const list = $("auto-db-list");
      const current = list.value;
      list.replaceChildren();
      for (const name of catalog.plans || []) list.append(new Option(name, name, false, name === current));
      if (!list.options.length) list.append(new Option(I18n.t("text.144faf32f476"), ""));
    } catch (error) {
      showMessage(I18n.f("text.e0cfeec229a7", {p0: error.message}), "danger");
    }
  }

  function render(status) {
    latestStatus = status;
    const run = status.plan || {};
    const link = status.link || {};
    const [label, color] = STATES[run.state] || STATES.idle;
    $("auto-state").className = `badge text-bg-${color} fs-6`;
    $("auto-state").textContent = label;
    const wasRunning = running;
    running = run.state === "running";
    $("auto-run-name").textContent = run.name || "-";
    $("auto-run-step").textContent = run.steps ? `${Math.min(run.step, run.steps)} / ${run.steps}` : "-";
    $("auto-run-cycle").textContent = run.state === "idle" ? "-" : I18n.f("text.90fb3f25323b", {p0: Math.max(1, run.cycle)});
    const elapsed = Number(run.stepElapsed) || 0;
    $("auto-run-elapsed").textContent = running
      ? `${Math.floor(elapsed / 3600)}:${String(Math.floor(elapsed / 60) % 60).padStart(2, "0")}:${String(elapsed % 60).padStart(2, "0")}` : "-";
    $("auto-run-message").textContent = I18n.message(run.message || "-");
    $("auto-run-battery").textContent = status.battery || I18n.t("text.e182b5b43dce");
    $("auto-run-ebc").textContent = link.connected ? (link.hasFrame ? I18n.message(link.modeName) : I18n.t("text.a4deced01e44")) : I18n.t("text.259c748db809");
    $("auto-run-values").textContent = link.connected && link.hasFrame
      ? `${link.voltage.toFixed(3)} V \u00b7 ${link.current.toFixed(2)} A \u00b7 ${link.power.toFixed(2)} W \u00b7 ${link.capacity.toFixed(3)} Ah` : "-";
    const recorder = status.recorder || {};
    $("auto-run-recorder").textContent = recorder.failed ? I18n.f("text.eea6efc6d899", {p0: I18n.message(recorder.error)})
      : recorder.active ? I18n.f("text.79968001775e", {p0: recorder.written}) : I18n.t("text.355e909366ca");
    updateStartButton();
    $("auto-abort").disabled = !running;
    $("auto-reset").disabled = running || run.state === "idle";
    for (const row of $("auto-steps").children) {
      const active = running && Number(row.dataset.index) === run.step - 1;
      row.classList.toggle("table-success", active);
    }
    if (wasRunning !== running) updateButtons();
  }

  async function refresh() {
    clearTimeout(timer);
    if (!$("auto-title")) return;
    if (!document.hidden && !$("tab-auto")?.classList.contains("d-none")) {
      try {
        render(JSON.parse(await api("/api/tester/status")));
      } catch (error) {
        latestStatus = null;
        updateStartButton();
        showMessage(I18n.f("text.3082d3bc31da", {p0: error.message}), "danger");
      }
    }
    timer = setTimeout(refresh, 1000);
  }

  async function action(body, reload) {
    if (submitting && body.action !== "abort") return;
    submitting = true;
    updateStartButton();
    showMessage("");
    try {
      showMessage(await post("/api/tester/plan/action", body), "success");
      if (reload) await loadSettings();
      if (body.action === "save_db") await loadCatalog();
    } catch (error) {
      showMessage(error.message, "danger");
    } finally {
      submitting = false;
      latestStatus = null;
      updateStartButton();
    }

    void refresh();
  }

  document.addEventListener("tester-status", (event) => render(event.detail));
  document.addEventListener("tester-status-error", () => {
    latestStatus = null;
    updateStartButton();
  });

  $("auto-mode").addEventListener("change", applyMode);
  $("auto-editor").addEventListener("submit", (event) => {
    event.preventDefault();
    edit((steps) => {
      const step = readEditor();
      const at = selected >= 0 ? selected + 1 : steps.length;
      steps.splice(at, 0, step);
      return at;
    });
  });
  $("auto-replace").addEventListener("click", () => edit((steps) => {
    steps[selected] = readEditor();
    return selected;
  }));
  $("auto-up").addEventListener("click", () => edit((steps) => {
    [steps[selected - 1], steps[selected]] = [steps[selected], steps[selected - 1]];
    return selected - 1;
  }));
  $("auto-down").addEventListener("click", () => edit((steps) => {
    [steps[selected + 1], steps[selected]] = [steps[selected], steps[selected + 1]];
    return selected + 1;
  }));
  $("auto-delete").addEventListener("click", () => edit((steps) => {
    steps.splice(selected, 1);
    return -1;
  }));
  $("auto-clear").addEventListener("click", () => {
    if (window.confirm(I18n.t("text.5f8e53533c32"))) edit((steps) => (steps.splice(0), -1));
  });
  $("auto-name").addEventListener("change", () => void savePlan(plan.steps, selected));
  $("auto-db-refresh").addEventListener("click", () => void loadCatalog());
  $("auto-db-load").addEventListener("click", () => {
    const name = $("auto-db-list").value;
    if (!name) {
      showMessage(I18n.t("text.3ff26edd42cd"), "warning");
      return;
    }
    void action({ action: "load_db", name }, true);
  });
  $("auto-db-save").addEventListener("click", () => void action({ action: "save_db" }, false));
  $("auto-start").addEventListener("click", () => void action({ action: "start" }, false));
  $("auto-abort").addEventListener("click", () => {
    if (window.confirm(I18n.t("text.f1629dc47ff0"))) void action({ action: "abort" }, false);
  });
  $("auto-reset").addEventListener("click", () => void action({ action: "reset" }, false));

  applyMode();
  updateStartButton();
  void loadSettings();
  void loadCatalog();
  void refresh();
})();
