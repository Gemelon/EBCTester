"use strict";
(() => {
  const $ = id => document.getElementById(id);
  const groups = [
    [I18n.t("text.749080e53cd5"), true, [
      ["capacityAh", I18n.t("text.6b429b27a2d6"), .001, 10000, .001],
      ["chargeCurrent", I18n.t("text.e68484365032"), .01, 20, .01],
      ["chargeVoltage", I18n.t("text.276ed7b6e344"), .01, 30, .01],
      ["chargeCutoff", I18n.t("text.453fd7f5bed1"), .01, 20, .01],
      ["prepCurrent", I18n.t("text.dd7980245e42"), .01, 20, .01],
      ["maxChargeSeconds", I18n.t("text.8c7ee9e6aa9d"), 10, 172800, 1],
      ["maxPrepSeconds", I18n.t("text.3c718cb73233"), 10, 172800, 1],
    ]],
    [I18n.t("text.d4e0e9b4aa62"), false, [
      ["targetSoc", I18n.t("text.1d43b7fae7b8"), 0, 100, .1],
      ["restSeconds", I18n.t("text.323c804ef7d8"), 1, 86400, 1],
      ["baselineSeconds", I18n.t("text.3a66823dc8e5"), 1, 60, .1],
      ["idleCurrent", I18n.t("text.6708b0550d9c"), 0, .5, .01],
      ["pulseCurrent", I18n.t("text.5e99f14cd255"), .01, 20, .01],
      ["pulseSeconds", I18n.t("text.fed0e3cec9b1"), 1, 120, .1],
      ["time1", I18n.t("text.72f8c39b6e75"), .1, 120, .1],
      ["time2", I18n.t("text.fa926cb65d8b"), .1, 120, .1],
      ["time3", I18n.t("text.57a3845fd3a0"), .1, 120, .1],
    ]],
    [I18n.t("text.99cf614cdeb1"), false, [
      ["cutoffVoltage", I18n.t("text.13a15d99837c"), .01, 30, .01],
      ["maxVoltage", I18n.t("text.710c047e361f"), .01, 30, .01],
      ["maxPower", I18n.t("text.ab38386a09d8"), .01, 2000, .01],
      ["maxGap", I18n.t("text.2035b911edfc"), .1, 5, .1],
      ["timingTolerance", I18n.t("text.286a930c4b70"), .1, 5, .1],
      ["minVoltageDrop", I18n.t("text.4a7e8f5ca4db"), .0001, 1, .0001],
    ]],
  ];
  const phases = {
    idle: I18n.t("text.8d4d4830b4da"), charge_start: I18n.t("text.fe1b3059ac74"), charging: I18n.t("text.01a410769123"),
    discharge_start: I18n.t("text.a42fcd1ab926"), discharging: I18n.t("text.956dddd39448"),
    prep_stop: I18n.t("text.96ac18066d16"), rest: I18n.t("text.1d2c06152d16"),
    pulse_start: I18n.t("text.097ef6c54716"), pulse: I18n.t("text.4339ffacd227"), stopping: I18n.t("text.d95b91f4b7d1"),
    complete: I18n.t("text.38ee859a969e"), failed: I18n.t("text.f6cc01cb7edb"), interrupted: I18n.t("text.ab1de1b94881"),
  };
  let loaded = false, sending = false, tester = null, status = null;
  let timer = 0, historyRequested = false;
  const fields = groups.flatMap(group => group[2]);
  const fmt = (value, digits = 3) => Number.isFinite(value) ? value.toFixed(digits) : "-";
  function message(text, kind = "danger") {
    $("res-message").className = `alert alert-${kind}${text ? "" : " d-none"}`;
    $("res-message").textContent = I18n.message(text);
  }
  async function api(url, body) {
    const options = {cache:"no-store", credentials:"same-origin", signal:AbortSignal.timeout(10000)};
    if (body) {
      const response = await fetch("/api/status", options);
      if (!response.ok) throw new Error(`Status HTTP ${response.status}`);
      options.method = "POST";
      options.headers = {"Content-Type":"application/json", "X-CSRF-Token":(await response.json()).csrfToken};
      options.body = JSON.stringify(body);
    }
    const response = await fetch(url, options);
    if (!response.ok) throw new Error(I18n.message(await response.text()));
    return body ? response.text() : response.json();
  }
  function controls() {
    const busy = sending || Boolean(status?.busy);
    $("res-start").disabled = busy || !loaded || !status || !tester || tester.controlsBusy ||
      !tester.link?.connected || !tester.link?.hasFrame || !tester.databaseReady || !$("res-confirm").checked;
    $("res-save").disabled = busy || !loaded;
    $("res-abort").disabled = !status?.busy;
    $("res-method").disabled = busy;
    for (const field of fields) $(`res-${field[0]}`).disabled = busy;
  }
  function method() {
    for (const element of document.querySelectorAll("[data-res-auto]"))
      element.classList.toggle("d-none", $("res-method").value !== "automatic");
    controls();
  }
  for (const [title, automatic, list] of groups) {
    const group = document.createElement("div");
    group.className = "mb-3";
    if (automatic) group.dataset.resAuto = "true";
    const heading = document.createElement("h4");
    heading.className = "h6 border-bottom pb-2";
    heading.textContent = title;
    group.append(heading);
    const row = document.createElement("div");
    row.className = "row g-2";
    for (const [name, label, min, max, step] of list) {
      const cell = document.createElement("div");
      cell.className = "col-sm-6";
      const lab = document.createElement("label");
      lab.className = "form-label small";
      lab.htmlFor = `res-${name}`;
      lab.textContent = label;
      const input = document.createElement("input");
      Object.assign(input, {id:lab.htmlFor, type:"number", className:"form-control",
        min:String(min), max:String(max), step:String(step), required:true});
      cell.append(lab, input);
      row.append(cell);
    }
    group.append(row);
    $("res-fields").append(group);
  }
  function collect() {
    const config = {automatic:$("res-method").value === "automatic"};
    for (const [name, label, min, max] of fields) {
      const input = $(`res-${name}`);
      const value = Number(input.value);
      if (!input.value || !Number.isFinite(value) || value < min || value > max)
        throw new Error(I18n.f("text.91bfbfe9b35f", {p0: label, p1: min, p2: max}));
      config[name] = value;
    }
    return config;
  }
  function render(data) {
    const wasBusy = status?.busy;
    status = data;
    if (!loaded) {
      for (const [name] of fields) $(`res-${name}`).value = data.config[name];
      $("res-method").value = data.config.automatic ? "automatic" : "prepared";
      loaded = true;
      method();
    }
    const color = data.phase === "complete" ? "success" : data.phase === "failed" ? "danger" :
      data.busy ? "primary" : data.phase === "interrupted" ? "warning" : "secondary";
    $("res-state").className = `badge text-bg-${color} fs-6`;
    $("res-state").textContent = phases[data.phase] || data.phase;
    $("res-phase").textContent = phases[data.phase] || data.phase;
    $("res-elapsed").textContent = `${fmt(data.elapsed, 1)} s`;
    $("res-ah").textContent = `${fmt(data.removedAh, 4)} / ${fmt(data.targetAh, 4)} Ah`;
    $("res-baseline").textContent = `${fmt(data.baselineVoltage, 4)} V / ${fmt(data.baselineCurrent)} A`;
    $("res-onset").textContent = `${fmt(data.onsetUncertainty)} s`;
    $("res-detail").textContent = I18n.message(data.message || "-");
    $("res-run").textContent = data.runId || "-";
    $("res-results").replaceChildren();
    for (const r of data.results) {
      const row = document.createElement("tr");
      const values = [
        `${fmt(r.requested, 1)} / ${r.captured ? fmt(r.actual) : "-"}`,
        r.captured ? fmt(r.voltage, 4) : "-", r.captured ? fmt(r.current) : "-",
        r.valid ? fmt(r.milliohms) : I18n.message(r.error || "-"),
        r.epochMs ? new Date(r.epochMs).toLocaleString(I18n.locale) : r.captured ? I18n.t("text.b4516e3d024e") : "-",
      ];
      for (const text of values) {
        const cell = document.createElement("td");
        cell.textContent = text;
        row.append(cell);
      }
      $("res-results").append(row);
    }
    controls();
    if (wasBusy && !data.busy) void history();
  }
  async function history() {
    try {
      const rows = await api("/api/tester/resistance/history");
      $("res-history").replaceChildren();
      if (!rows.length) $("res-history").textContent = I18n.t("text.865b64f74dd7");
      for (const row of rows) {
        const detail = document.createElement("details");
        detail.className = "border rounded p-2 mb-2";
        const heading = document.createElement("summary");
        heading.textContent = `${row.battery} | ${row.started} | ${row.status}`;
        const text = document.createElement("p");
        text.className = "small text-break mt-2";
        text.textContent = `${row.runId}: ${row.notes}`;
        detail.append(heading);
        for (const r of row.results || []) {
          const value = document.createElement("p");
          value.className = "small mt-2 mb-1";
          value.textContent = I18n.f("text.37948995544c", {p0: fmt(r.requested, 1), p1: fmt(r.actual)}).trim() + " " +
            (r.valid ? `${fmt(r.milliohms)} mOhm` : I18n.f("text.e741d71489d8", {p0: I18n.message(r.error || I18n.t("text.cd7831e632ce"))})) +
            ` | ${r.capturedUtc ? r.capturedUtc + " UTC" : I18n.t("text.9fa761ebb714")}`;
          detail.append(value);
        }
        detail.append(text);
        $("res-history").append(detail);
      }
      historyRequested = true;
    } catch (error) { message(I18n.f("text.f906f7357e05", {p0: error.message})); }
  }
  async function refresh() {
    clearTimeout(timer);
    if (!document.hidden && !$("tab-resistance").classList.contains("d-none")) {
      try {
        render(await api("/api/tester/resistance"));
        if (!historyRequested) await history();
      } catch (error) { status = null; controls(); message(error.message); }
    }
    timer = setTimeout(refresh, 1000);
  }
  async function action(name) {
    if (sending && name !== "abort") return;
    let body;
    try { body = name === "abort" ? {action:name} : {action:name, config:collect()}; }
    catch (error) { message(error.message); return; }
    sending = true;
    controls();
    try { message(await api("/api/tester/resistance", body), "success"); }
    catch (error) { message(error.message); }
    finally {
      sending = false;
      tester = null;
      controls();
      void refresh();
    }
  }
  document.addEventListener("tester-status", event => {
    tester = event.detail;
    $("res-battery").textContent = tester.battery || I18n.t("text.e2a71bcb34a6");
    $("res-live").textContent = tester.link?.connected ?
      `${fmt(tester.link.voltage)} V / ${fmt(tester.link.current)} A` : I18n.t("text.b0caf73f4640");
    controls();
  });
  document.addEventListener("tester-status-error", () => { tester = null; controls(); });
  $("res-form").addEventListener("submit", event => { event.preventDefault(); void action("start"); });
  $("res-save").addEventListener("click", () => void action("save"));
  $("res-abort").addEventListener("click", () => void action("abort"));
  $("res-method").addEventListener("change", () => {
    $("res-confirm").checked = false;
    method();
  });
  $("res-confirm").addEventListener("change", controls);
  $("res-history-refresh").addEventListener("click", () => void history());
  for (const [name] of fields) $(`res-${name}`).addEventListener("input", () => {
    $("res-confirm").checked = false;
    controls();
  });
  method();
  void refresh();
})();
