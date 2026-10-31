// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

(() => {
  const brand = document.getElementById("app-brand");
  if (brand) {
    const installLogo = () => {
      if (brand.querySelector("img")) return;
      const logo = document.createElement("img");
      logo.src = "/ui/dashboard/brand-logo.png";
      logo.alt = "EBCTester";
      logo.height = 48;
      logo.className = "bg-white rounded px-2 py-1";
      brand.replaceChildren(logo);
      brand.setAttribute("aria-label", "EBCTester home");
      brand.title = "EBCTester";
    };
    installLogo();
    new MutationObserver(installLogo).observe(brand, { childList: true });

    const modelIndicator = document.createElement("span");
    modelIndicator.id = "detected-ebc-model";
    modelIndicator.className = "small text-white";
    modelIndicator.textContent = "Unknown";
    brand.insertAdjacentElement("afterend", modelIndicator);
    const modelHeading = document.getElementById("page-subtitle");
    const modelError = document.createElement("div");
    modelError.id = "tester-connection-error";
    modelError.className = "alert alert-danger d-none";
    modelError.setAttribute("role", "alert");
    if (modelHeading) modelHeading.parentElement.insertAdjacentElement("afterend", modelError);
    let modelName = "Unknown";
    let renderedHeading = null;
    function updateModelHeading() {
      if (!modelHeading) return;
      const template = I18n.t("text.690c0f1d7edf");
      if (modelHeading.textContent !== template && modelHeading.textContent !== renderedHeading) return;
      renderedHeading = template.replace("EBC-A20", modelName);
      if (modelHeading.textContent !== renderedHeading) modelHeading.textContent = renderedHeading;
    }
    // The shared shell restores its section subtitle on every status refresh.
    if (modelHeading) {
      new MutationObserver(updateModelHeading).observe(modelHeading, { childList: true, characterData: true, subtree: true });
    }
    function renderDetectedModel(link) {
      modelError.textContent = I18n.message(link?.error || "");
      modelError.className = `alert alert-danger${link?.error ? "" : " d-none"}`;
      if (!link?.hasDetectedModel && !link?.hasModel) {
        modelIndicator.className = "small text-white";
        modelIndicator.textContent = "Unknown";
        modelIndicator.title = "";
        modelName = "Unknown";
        updateModelHeading();
        return;
      }
      const detected = Boolean(link.hasDetectedModel);
      const model = detected ? link.detectedModel : link.model;
      const ident = `0x${Number(model).toString(16).padStart(2, "0").toUpperCase()}`;
      const known = detected ? link.detectedModelKnown : link.modelKnown;
      let name = detected ? (link.detectedModelName || "Unknown") : link.modelName;
      if (!known) {
        const version = link.detectedVersionKnown
          ? `version V${Math.floor(link.detectedVersion / 100)}.${String(link.detectedVersion % 100).padStart(2, "0")}`
          : "version unknown";
        name = detected ? `Unknown device (ident ${ident}, ${version})` : `Unknown (${ident})`;
      }
      const supported = !detected || link.detectedModelEnabled;
      modelIndicator.className = known && supported && !link.detectedVersionPending
        ? "small text-white" : "badge text-bg-warning text-dark";
      modelIndicator.textContent = name;
      modelIndicator.title = !known
        ? `Unknown model identifier ${ident}; control commands are disabled.`
        : !supported
          ? `${name} is recognized but not supported; control commands are disabled.`
          : link.detectedVersionPending
            ? `${name}; control commands are disabled until its firmware version is identified.`
            : detected ? `Detected ${name} (${ident})` : `Selected ${name} (${ident})`;
      modelName = name;
      updateModelHeading();
    }
    document.addEventListener("tester-status", event => {
      renderDetectedModel(event.detail?.link);
    });
    document.addEventListener("tester-status-error", () => renderDetectedModel(null));
    renderDetectedModel(null);
  }

  let favicon = document.querySelector('link[rel~="icon"]');
  if (!favicon) {
    favicon = document.createElement("link");
    favicon.rel = "icon";
    document.head.append(favicon);
  }
  favicon.type = "image/png";
  favicon.href = "/ui/dashboard/brand-icon.png";

  const $ = (id) => document.getElementById(id);
  const MAX_POINTS = 1500;
  const REFRESH_MS = 15000;
  const MODES = {
    "C-CV": { label: I18n.t("text.4d51d880dd4e"), badge: "text-bg-success" },
    "D-CC": { label: I18n.t("text.a563c0701123"), badge: "text-bg-danger" },
    "D-CP": { label: I18n.t("text.b7766d8f0c50"), badge: "text-bg-danger" },
    WAIT: { label: I18n.t("text.233631694afc"), badge: "text-bg-secondary" },
    CYCLE: { label: I18n.t("text.dfd72aef2879"), badge: "text-bg-info" },
  };
  const STATUS = {
    RUNNING: [I18n.t("text.f85fd2bc5619"), "text-bg-primary"],
    COMPLETED: [I18n.t("text.38ee859a969e"), "text-bg-success"],
    STOPPED: [I18n.t("text.641c1c76e38a"), "text-bg-warning"],
    INTERRUPTED: [I18n.t("text.d8135c8fc9ca"), "text-bg-warning"],
    ERROR: [I18n.t("text.f6cc01cb7edb"), "text-bg-danger"],
  };
  const PALETTE = ["#0d6efd", "#dc3545", "#198754", "#fd7e14", "#6f42c1", "#20c997", "#d63384", "#0dcaf0",
    "#6c757d", "#ffc107"];

  let catalog = { batteries: [], runs: [] };
  let charts = {};
  let currentRunId = "";
  let refreshTimer = 0;
  let busy = false;

  const modeInfo = (mode) => MODES[mode] || { label: mode || "?", badge: "text-bg-light" };
  const color = (index) => PALETTE[index % PALETTE.length];
  const fmt = (value, digits) => (typeof value === "number" && Number.isFinite(value) ? value.toFixed(digits) : "-");

  function parseUtc(text) {
    if (!text) return NaN;
    let iso = String(text).trim().replace(" ", "T");
    if (!/[zZ]|[+-]\d\d:?\d\d$/.test(iso)) iso += "Z";
    return Date.parse(iso);
  }

  function formatDate(text) {
    const ms = parseUtc(text);
    return Number.isFinite(ms) ? new Date(ms).toLocaleString(I18n.locale) : "-";
  }

  function formatDuration(ms) {
    if (!Number.isFinite(ms) || ms < 0) return "-";
    const minutes = Math.round(ms / 60000);
    return `${Math.floor(minutes / 60)}:${String(minutes % 60).padStart(2, "0")} h`;
  }

  function hoursLabel(hours) {
    const minutes = Math.round(hours * 60);
    const sign = minutes < 0 ? "-" : "";
    const abs = Math.abs(minutes);
    return `${sign}${Math.floor(abs / 60)}:${String(abs % 60).padStart(2, "0")}`;
  }

  function showMessage(text, kind = "info") {
    const box = $("dash-message");
    if (!box) return;
    box.className = `alert alert-${kind}${text ? "" : " d-none"}`;
    box.textContent = text || "";
  }

  async function api(url) {
    const response = await fetch(url, { cache: "no-store", credentials: "same-origin" });
    const text = await response.text();
    let body = null;
    try { body = text ? JSON.parse(text) : null; } catch (error) { body = null; }
    if (!response.ok) {
      const reason = (body && (body.error || body.message)) || text || `HTTP ${response.status}`;
      throw new Error(response.status === 503 ? I18n.f("text.33259ec60dd7", {p0: I18n.message(reason)}) : I18n.message(reason));
    }
    return body;
  }

  function loadChartLibrary() {
    if (window.Chart) return Promise.resolve(window.Chart);
    if (loadChartLibrary.pending) return loadChartLibrary.pending;
    loadChartLibrary.pending = new Promise((resolve, reject) => {
      const script = document.createElement("script");
      script.src = "/ui/dashboard/chart.js";
      script.onload = () => (window.Chart ? resolve(window.Chart) : reject(new Error(I18n.t("text.20dea121d061"))));
      script.onerror = () => {
        loadChartLibrary.pending = null;
        reject(new Error(I18n.t("text.33c0dd4d0035")));
      };
      document.head.append(script);
    });
    return loadChartLibrary.pending;
  }

  function option(value, text) {
    const element = document.createElement("option");
    element.value = value;
    element.textContent = text;
    return element;
  }

  function batteryById(id) {
    return catalog.batteries.find((battery) => String(battery.id) === String(id));
  }

  function renderBatterySelect(preferred) {
    const select = $("dash-battery");
    select.replaceChildren(option("", I18n.f("text.2f45fcaee204", {p0: catalog.batteries.length})));
    for (const battery of catalog.batteries) {
      const nominal = typeof battery.nominalAh === "number" ? ` · ${battery.nominalAh} Ah` : "";
      select.append(option(String(battery.id), `${battery.label || I18n.t("text.43787dab5fe9") + battery.id}${nominal}`));
    }
    if (preferred !== undefined && [...select.options].some((item) => item.value === preferred)) select.value = preferred;
  }

  function renderRunSelect(preferred) {
    const select = $("dash-run");
    const batteryFilter = $("dash-battery").value;
    const runs = catalog.runs.filter((run) => !batteryFilter || String(run.batteryId) === batteryFilter);
    select.replaceChildren();
    if (!runs.length) select.append(option("", I18n.t("text.e049e04a01bc")));
    for (const run of runs) {
      const battery = batteryById(run.batteryId);
      const state = (STATUS[run.status] || [run.status])[0];
      select.append(option(run.id,
        run.name || `${formatDate(run.started)} · ${battery ? battery.label : "?"} · ${run.plan || I18n.t("text.b01429e91a75")} · ${state}`));
    }
    if (preferred && runs.some((run) => run.id === preferred)) select.value = preferred;
    $("dash-load").disabled = !runs.length;
  }

  async function loadCatalog(preferredRun) {
    catalog = await api("/api/db/runs");
    catalog.batteries = catalog.batteries || [];
    catalog.runs = catalog.runs || [];
    const run = catalog.runs.find((item) => item.id === preferredRun);
    renderBatterySelect(run ? "" : $("dash-battery").value);
    renderRunSelect(preferredRun);
  }

  function renderSummary(details) {
    const run = details.run || {};
    const last = details.last || {};
    $("dash-battery-label").textContent = run.batteryLabel || "-";
    $("dash-battery-detail").textContent = [run.manufacturer, run.serial].filter(Boolean).join(" · ") || "-";
    $("dash-battery-nominal").textContent = typeof run.nominalAh === "number" ? I18n.f("text.4e25feed78ac", {p0: run.nominalAh}) : "-";
    $("dash-current").textContent = fmt(last.current, 2);
    $("dash-voltage").textContent = fmt(last.voltage, 3);
    $("dash-capacity").textContent = fmt(last.capacity, 3);
    const [label, badge] = STATUS[run.status] || [run.status || "-", "text-bg-secondary"];
    const status = $("dash-status");
    status.className = `badge ${badge}`;
    status.textContent = label;
    $("dash-step").textContent = I18n.f("text.f5c7e02a3035", {p0: run.currentStep ?? "-", p1: run.cyclePass ?? "-"});
    $("dash-run-title").textContent = run.name || I18n.f("text.0ee7dce1257d", {p0: run.plan || I18n.t("text.b01429e91a75"), p1: formatDate(run.started)}) +
      (run.ended ? I18n.f("text.9fa0478ce441", {p0: formatDate(run.ended)}) : "");
  }

  function cell(text, className) {
    const td = document.createElement("td");
    if (className) td.className = className;
    td.textContent = text;
    return td;
  }

  function renderSegments(segments) {
    const body = $("dash-segments");
    body.replaceChildren();
    if (!segments.length) {
      const row = document.createElement("tr");
      const td = cell(I18n.t("text.6f7e2fd9e011"), "text-body-secondary");
      td.colSpan = 11;
      row.append(td);
      body.append(row);
      return;
    }
    for (const segment of segments) {
      const row = document.createElement("tr");
      const info = modeInfo(segment.mode);
      const badge = document.createElement("span");
      badge.className = `badge ${info.badge}`;
      badge.textContent = info.label;
      const modeCell = document.createElement("td");
      modeCell.append(badge);
      const duration = Number.isFinite(segment.endMs) && Number.isFinite(segment.startMs)
        ? segment.endMs - segment.startMs
        : segment.endUptime - segment.startUptime;
      row.append(
        cell(String(segment.n)), modeCell, cell(String(segment.step ?? "-")), cell(String(segment.cycle ?? "-")),
        cell(fmt(segment.sp1, 2), "text-end"), cell(fmt(segment.sp2, 2), "text-end"), cell(fmt(segment.sp3, 2), "text-end"),
        cell(String(segment.samples ?? 0), "text-end"), cell(fmt(segment.capacityAh, 3), "text-end"),
        cell(Number.isFinite(segment.startMs) ? new Date(segment.startMs).toLocaleString(I18n.locale) : "-"),
        cell(formatDuration(duration), "text-end"));
      body.append(row);
    }
  }

  /* Relative hours from run start; prefers wall-clock time, falls back to device uptime. */
  function timeAxis(points) {
    const useEpoch = points.length > 0 && points.every((point) => point[2] !== null);
    const index = useEpoch ? 2 : 1;
    const origin = points.length ? points[0][index] : 0;
    return (point) => (point[index] - origin) / 3600000;
  }

  function destroyCharts() {
    for (const chart of Object.values(charts)) chart.destroy();
    charts = {};
  }

  function sizeBox(id, height) {
    const box = $(id);
    if (box) box.style.height = `${height}px`;
  }

  function linearX() {
    return {
      type: "linear",
      title: { display: true, text: I18n.t("text.b587a75c3586") },
      ticks: { callback: (value) => hoursLabel(value), maxTicksLimit: 12 },
    };
  }

  function tooltipTitle(items) {
    return items.length ? `t = ${hoursLabel(items[0].parsed.x)} h` : "";
  }

  function renderCharts(Chart, details, series) {
    destroyCharts();
    sizeBox("dash-box-vi", 380);
    sizeBox("dash-box-cap", 300);
    sizeBox("dash-box-bar", 300);
    const segments = details.segments || [];
    const points = series.points || [];
    const toX = timeAxis(points);
    const bySegment = new Map();
    for (const point of points) {
      if (!bySegment.has(point[0])) bySegment.set(point[0], []);
      bySegment.get(point[0]).push(point);
    }
    const segmentInfo = new Map(segments.map((segment) => [segment.n, segment]));
    const viData = [];
    const capData = [];
    let colorIndex = 0;
    for (const [segmentNumber, list] of bySegment) {
      const segment = segmentInfo.get(segmentNumber) || {};
      const name = `S${segmentNumber} ${modeInfo(segment.mode).label}`;
      const tone = color(colorIndex++);
      viData.push({
        label: `U · ${name}`, yAxisID: "u", borderColor: tone, backgroundColor: tone, borderWidth: 1.5,
        pointRadius: 0, data: list.map((point) => ({ x: toX(point), y: point[4] })),
      });
      viData.push({
        label: `I · ${name}`, yAxisID: "i", borderColor: tone, backgroundColor: tone, borderWidth: 1.2,
        borderDash: [6, 4], pointRadius: 0, data: list.map((point) => ({ x: toX(point), y: point[3] })),
      });
      if (segment.mode !== "WAIT") {
        capData.push({
          label: name, borderColor: tone, backgroundColor: `${tone}33`, fill: "origin", borderWidth: 1.5,
          pointRadius: 0, data: list.map((point) => ({ x: toX(point), y: point[5] })),
        });
      }
    }
    const common = {
      parsing: false, normalized: true, animation: false, maintainAspectRatio: false, responsive: true,
      interaction: { mode: "nearest", axis: "x", intersect: false },
    };
    charts.vi = new Chart($("dash-chart-vi"), {
      type: "line",
      data: { datasets: viData },
      options: {
        ...common,
        scales: {
          x: linearX(),
          u: { type: "linear", position: "left", title: { display: true, text: I18n.t("text.661a2737adc5") } },
          i: { type: "linear", position: "right", title: { display: true, text: I18n.t("text.bb39ee18acd6") }, grid: { drawOnChartArea: false } },
        },
        plugins: {
          legend: { position: "bottom", labels: { boxWidth: 12, font: { size: 10 } } },
          tooltip: { callbacks: { title: tooltipTitle } },
        },
      },
    });
    charts.cap = new Chart($("dash-chart-cap"), {
      type: "line",
      data: { datasets: capData },
      options: {
        ...common,
        scales: { x: linearX(), y: { beginAtZero: true, title: { display: true, text: I18n.t("text.c656a6c17054") } } },
        plugins: {
          legend: { position: "bottom", labels: { boxWidth: 12, font: { size: 10 } } },
          tooltip: { callbacks: { title: tooltipTitle } },
        },
      },
    });
    const bars = segments.filter((segment) => segment.mode !== "WAIT");
    charts.bar = new Chart($("dash-chart-bar"), {
      type: "bar",
      data: {
        labels: bars.map((segment) => `S${segment.n} ${modeInfo(segment.mode).label} Z${segment.cycle ?? "-"}`),
        datasets: [{
          label: I18n.t("text.c656a6c17054"),
          data: bars.map((segment) => segment.capacityAh ?? 0),
          backgroundColor: bars.map((segment) => (String(segment.mode).startsWith("D-") ? "#dc3545" : "#198754")),
        }],
      },
      options: {
        animation: false, maintainAspectRatio: false, responsive: true,
        scales: { y: { beginAtZero: true, title: { display: true, text: "Ah" } } },
        plugins: { legend: { display: false } },
      },
    });
    $("dash-sample-info").textContent = series.stride > 1
      ? I18n.f("text.72b64a16ead9", {p0: points.length, p1: series.total, p2: series.stride})
      : I18n.f("text.452d72679b6a", {p0: points.length});
  }

  function scheduleRefresh(details) {
    clearTimeout(refreshTimer);
    refreshTimer = 0;
    if (details && details.run && details.run.status === "RUNNING") {
      refreshTimer = setTimeout(() => {
        if ($("dash-run") && document.visibilityState === "visible") void loadRun(currentRunId, true);
        else scheduleRefresh(details);
      }, REFRESH_MS);
    }
  }

  async function loadRun(runId, silent = false) {
    if (!runId || busy) return;
    busy = true;
    $("dash-load").disabled = true;
    if (!silent) showMessage(I18n.t("text.2288c3a5258b"), "info");
    try {
      const id = encodeURIComponent(runId);
      const [Chart, details, series] = await Promise.all([
        loadChartLibrary(),
        api(`/api/db/run?id=${id}`),
        api(`/api/db/measurements?id=${id}&max=${MAX_POINTS}`),
      ]);
      currentRunId = runId;
      renderSummary(details);
      renderSegments(details.segments || []);
      renderCharts(Chart, details, series);
      showMessage("");
      scheduleRefresh(details);
    } catch (error) {
      showMessage(I18n.f("text.30d7af987af8", {p0: error.message}), "danger");
    } finally {
      busy = false;
      $("dash-load").disabled = !$("dash-run").value;
    }
  }

  async function refreshCatalog(preferredRun) {
    try {
      await loadCatalog(preferredRun || $("dash-run").value);
      if (!catalog.runs.length) {
        showMessage(I18n.t("dashboard.noRuns"), "secondary");
      }
      return true;
    } catch (error) {
      showMessage(I18n.f("text.9ee28309cc48", {p0: error.message}), "danger");
      return false;
    }
  }

  async function init() {
    if (!$("dash-run")) return;
    $("dash-battery").addEventListener("change", () => renderRunSelect());
    $("dash-run").addEventListener("change", () => { $("dash-load").disabled = !$("dash-run").value; });
    $("dash-load").addEventListener("click", () => void loadRun($("dash-run").value));
    $("dash-refresh").addEventListener("click", () => void refreshCatalog());
    window.addEventListener("test-runs-changed", async (event) => {
      if (event.detail.action === "delete" && event.detail.id === currentRunId) {
        clearTimeout(refreshTimer);
        currentRunId = "";
        for (const chart of Object.values(charts)) chart.destroy();
        charts = {};
        renderSummary({});
        renderSegments([]);
      }
      await refreshCatalog();
      if (event.detail.action === "rename" && event.detail.id === currentRunId) void loadRun(currentRunId);
    });
    if (await refreshCatalog() && catalog.runs.length) await loadRun($("dash-run").value);
  }

  void init();
})();
