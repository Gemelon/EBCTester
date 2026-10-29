// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

(async () => {
  const { bindNumericValidation, setNumericBounds } = await import("/ui/tester/validation.js");
  const $ = (id) => document.getElementById(id);
  const chargeValidation = bindNumericValidation($("man-charge-form"));
  const dischargeValidation = bindNumericValidation($("man-discharge-form"));
  const fmt = (value, digits, unit) => (Number.isFinite(value) ? `${value.toFixed(digits)} ${unit}` : "-");
  let manual = {};
  let timer = 0;
  let submitting = false;
  let latestStatus = null;
  let activeLimits = { chargeVoltage: 5, dischargeVoltage: 5, chargeCurrent: 5, dischargeCurrent: 20, power: 85 };

  function modelLimits() {
    return activeLimits;
  }

  function updateModelHeading(link) {
    const subtitle = document.querySelector("#tab-manual p[data-model-heading]");
    if (!subtitle) return;
    const base = I18n.t("text.dd59d095071c").replace("EBC-A20", "Unknown");
    if (!link?.connected || !link?.hasModel) {
      subtitle.textContent = base;
      return;
    }
    const ident = `0x${Number(link.model).toString(16).padStart(2, "0").toUpperCase()}`;
    const name = link.modelKnown
      ? link.modelName : `Unknown (${ident})`;
    subtitle.textContent = I18n.t("text.dd59d095071c").replace("EBC-A20", name);
  }

  function startBlockReason(status) {
    const link = status?.link;
    if (!status) return "";
    if (!link?.connected) return "text.7c3e1a90b2d4";
    if (!link.hasFrame) return "text.5b9d03c7a1e8";
    if (link.pending) return "text.91e4c2f6a3b0";
    if (link.active) return "text.d03a7b58f2c1";
    if (status.plan?.state === "running") return "text.8a2d5e07c6f3";
    if (status.controlsBusy) return "text.c5b1f84a90d2";
    return "";
  }

  function updateControls() {
    const link = latestStatus?.link;
    const busy = submitting || !latestStatus || latestStatus.controlsBusy || link?.pending ||
      link?.active || latestStatus.plan?.state === "running";
    for (const form of ["man-charge-form", "man-discharge-form"]) {
      for (const button of $(form).querySelectorAll("button")) button.disabled = busy || !link?.connected || !link?.hasFrame;
    }
    const reason = submitting ? "" : startBlockReason(latestStatus);
    for (const id of ["man-hint-charge", "man-hint-discharge"]) {
      $(id).textContent = reason ? I18n.t(reason) : "";
      $(id).classList.toggle("d-none", !reason);
    }
    $("man-connect").disabled = Boolean(busy);
    $("man-disconnect").disabled = Boolean(busy);
    $("man-stop").disabled = !link?.connected;
  }

  function showMessage(text, kind = "info") {
    const box = $("man-message");
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

  const visible = () => !document.hidden && $("man-title") && !$("tab-manual")?.classList.contains("d-none");
  const discharge = () => (document.querySelector("input[name='man-discharge-mode']:checked")?.value || "cc");

  function applyDischargeMode() {
    const cp = discharge() === "cp";
    const limits = modelLimits();
    const input = $("man-discharge-value");
    $("man-discharge-value-label").textContent = cp ? I18n.t("text.c9d1515a58db") : I18n.t("text.bb39ee18acd6");
    setNumericBounds(input, { min: cp ? 1 : 0.01, max: cp ? limits.power : limits.dischargeCurrent,
      step: cp ? 1 : 0.01 });
    input.value = cp ? manual.power ?? 5 : manual.dischargeCurrent ?? 0.5;
    $("man-discharge-cutoff").value = cp ? manual.powerCutoff ?? 2.5 : manual.dischargeCutoff ?? 2.5;
    $("man-discharge-minutes").value = cp ? manual.powerMinutes ?? 239 : manual.dischargeMinutes ?? 239;
    updateModelLimits();
  }

  function updateModelLimits() {
    const limits = modelLimits();
    setNumericBounds($("man-charge-current"), { max: limits.chargeCurrent });
    setNumericBounds($("man-charge-voltage"), { max: limits.chargeVoltage });
    setNumericBounds($("man-charge-cutoff"), { max: Math.min(
      limits.chargeCurrent, Number($("man-charge-current").value) || limits.chargeCurrent) });
    setNumericBounds($("man-discharge-cutoff"), { max: limits.dischargeVoltage });
    const cp = discharge() === "cp";
    setNumericBounds($("man-discharge-value"), { max: cp
      ? Math.min(limits.power,
        limits.dischargeCurrent * (Number($("man-discharge-cutoff").value) || limits.dischargeVoltage))
      : limits.dischargeCurrent });
    chargeValidation.refresh();
    dischargeValidation.refresh();
  }

  async function loadSettings() {
    try {
      const settings = JSON.parse(await api("/api/tester/settings"));
      manual = settings.manual || {};
      $("man-charge-current").value = manual.chargeCurrent ?? 0.5;
      $("man-charge-voltage").value = manual.chargeVoltage ?? 3.45;
      $("man-charge-cutoff").value = manual.chargeCutoff ?? 0.1;
      applyDischargeMode();
    } catch (error) {
      showMessage(I18n.f("text.0204f4bad84e", {p0: error.message}), "danger");
    }
  }

  function render(status) {
    latestStatus = status;
    const link = status.link || {};
    if (link.limits) activeLimits = link.limits;
    updateModelHeading(link);
    updateModelLimits();
    const badge = $("man-link-badge");
    if (link.connected && link.active) {
      badge.className = "badge text-bg-success fs-6";
      badge.textContent = I18n.message(link.modeName);
    } else if (link.connected) {
      badge.className = "badge text-bg-primary fs-6";
      badge.textContent = I18n.t("text.0338189c985e");
    } else {
      badge.className = "badge text-bg-secondary fs-6";
      badge.textContent = I18n.t("text.b0caf73f4640");
    }
    const data = link.connected && link.hasFrame;
    $("man-voltage").textContent = data ? fmt(link.voltage, 3, "V") : "-";
    $("man-current").textContent = data ? fmt(link.current, 2, "A") : "-";
    $("man-power").textContent = data ? fmt(link.power, 2, "W") : "-";
    $("man-capacity").textContent = data ? fmt(link.capacity, 3, "Ah") : "-";
    $("man-mode").textContent = data ? `${I18n.message(link.modeName)} (0x${link.mode.toString(16).padStart(2, "0")})` : "-";
    $("man-setpoints").textContent = data
      ? `${link.setpoint1.toFixed(2)} / ${link.setpoint2.toFixed(2)} / ${link.setpoint3.toFixed(2)}` : "-";
    $("man-battery").textContent = status.battery || I18n.t("text.e182b5b43dce");
    const recorder = status.recorder || {};
    let recording = recorder.active ? I18n.f("text.b8e7da1f11ce", {p0: recorder.written}) : I18n.t("text.355e909366ca");
    if (recorder.pending) recording += I18n.f("text.44a2881930ec", {p0: recorder.pending});
    if (recorder.dropped) recording += I18n.f("text.551635377e11", {p0: recorder.dropped});
    if (recorder.failed) recording += " " + I18n.f("text.4e13a19ee934", {p0: I18n.message(recorder.error)});
    if (!status.databaseReady) recording += " " + I18n.t("text.534d2095f430");
    $("man-recorder").textContent = recording;
    const plan = status.plan || {};
    $("man-plan").textContent = plan.state === "running"
      ? I18n.f("text.4040cdd62f95", {p0: plan.step, p1: plan.steps}) : I18n.t("text.f19cd8e6f34a");
    $("man-model").textContent = link.hasFrame ? link.model : "-";
    $("man-frames").textContent = `${link.framesOk ?? 0} / ${link.framesBad ?? 0}`;
    $("man-age").textContent = link.ageMs >= 0 ? I18n.f("text.80b331d82b21", {p0: (link.ageMs / 1000).toFixed(1)}) : I18n.t("text.0e370a9362dc");
    $("man-pending").textContent = link.pending ? I18n.t("text.3702fc186663") : I18n.t("text.4d03db393cf1");
    $("man-auto").checked = Boolean(link.autoConnect);
    updateControls();
  }

  async function refresh() {
    clearTimeout(timer);
    if (!$("man-title")) return;
    if (visible()) {
      try {
        render(JSON.parse(await api("/api/tester/status")));
      } catch (error) {
        latestStatus = null;
        updateControls();
        showMessage(I18n.f("text.3082d3bc31da", {p0: error.message}), "danger");
      }
    }
    timer = setTimeout(refresh, 1000);
  }

  async function command(body) {
    if (submitting && body.action !== "stop") return;
    submitting = true;
    updateControls();
    showMessage("");
    try {
      showMessage(await api("/api/tester/command", { method: "POST", body: JSON.stringify(body) }), "success");
    } catch (error) {
      showMessage(error.message, "danger");
    } finally {
      submitting = false;
      latestStatus = null;
      updateControls();
    }

    void refresh();
  }

  document.addEventListener("tester-status", (event) => render(event.detail));
  document.addEventListener("tester-status-error", () => {
    latestStatus = null;
    updateControls();
  });

  $("man-stop").addEventListener("click", () => void command({ action: "stop" }));
  $("man-connect").addEventListener("click", () => void command({ action: "connect" }));
  $("man-disconnect").addEventListener("click", () => void command({ action: "disconnect" }));
  $("man-auto").addEventListener("change", () => void command({ action: "auto_connect", enabled: $("man-auto").checked }));
  $("man-charge-form").addEventListener("submit", (event) => {
    event.preventDefault();
    manual.chargeCurrent = Number($("man-charge-current").value);
    manual.chargeVoltage = Number($("man-charge-voltage").value);
    manual.chargeCutoff = Number($("man-charge-cutoff").value);
    void command({ action: "charge", current: manual.chargeCurrent, voltage: manual.chargeVoltage,
      cutoff: manual.chargeCutoff });
  });
  $("man-discharge-form").addEventListener("submit", (event) => {
    event.preventDefault();
    const value = Number($("man-discharge-value").value);
    const cutoff = Number($("man-discharge-cutoff").value);
    const minutes = Number($("man-discharge-minutes").value);
    if (discharge() === "cp") {
      Object.assign(manual, { power: value, powerCutoff: cutoff, powerMinutes: minutes });
      void command({ action: "discharge_cp", power: value, cutoff, minutes });
    } else {
      Object.assign(manual, { dischargeCurrent: value, dischargeCutoff: cutoff, dischargeMinutes: minutes });
      void command({ action: "discharge_cc", current: value, cutoff, minutes });
    }
  });
  for (const radio of document.querySelectorAll("input[name='man-discharge-mode']")) {
    radio.addEventListener("change", applyDischargeMode);
  }
  for (const id of ["man-charge-current", "man-discharge-cutoff"]) {
    $(id).addEventListener("input", updateModelLimits);
  }

  void loadSettings();
  updateControls();
  void refresh();
})().catch(error => {
  const box = document.getElementById("man-message");
  box.className = "alert alert-danger";
  box.textContent = I18n.message(error.message);
});
