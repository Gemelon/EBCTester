// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  const MAX_RENDERED_LINES = 500;
  const POLL_INTERVAL_MS = 1000;
  let lastSeq = 0;
  let paused = false;
  let timer = 0;
  let busy = false;

  function showMessage(text, kind = "info") {
    const box = $("debug-message");
    if (!box) return;
    box.className = `alert alert-${kind}${text ? "" : " d-none"}`;
    box.textContent = I18n.message(text);
  }

  function attached() {
    return Boolean($("debug-output"));
  }

  function appendLines(lines) {
    const output = $("debug-output");
    if (!output || !lines.length) return;
    const follow = $("debug-follow").checked;
    const atBottom = output.scrollHeight - output.scrollTop - output.clientHeight < 40;
    for (const line of lines) {
      const row = document.createElement("div");
      row.textContent = line.text;
      output.append(row);
    }
    while (output.childElementCount > MAX_RENDERED_LINES) output.firstElementChild.remove();
    if (follow && atBottom) output.scrollTop = output.scrollHeight;
  }

  async function fetchJson(url) {
    const response = await fetch(url, { cache: "no-store", credentials: "same-origin" });
    const text = await response.text();
    if (!response.ok) throw new Error(text || `HTTP ${response.status}`);
    return JSON.parse(text);
  }

  async function csrfToken() {
    return (await fetchJson("/api/status")).csrfToken || "";
  }

  async function post(url) {
    const response = await fetch(url, {
      method: "POST",
      cache: "no-store",
      credentials: "same-origin",
      headers: { "X-CSRF-Token": await csrfToken() },
    });
    const text = await response.text();
    if (!response.ok) throw new Error(text || `HTTP ${response.status}`);
    return text;
  }

  function schedule() {
    clearTimeout(timer);
    if (!attached()) return;
    timer = setTimeout(poll, POLL_INTERVAL_MS);
  }

  /* Fetches in batches so that a full buffer is caught up without blocking the device. */
  async function poll() {
    if (!attached()) return;
    if (busy || paused || document.hidden) {
      schedule();
      return;
    }
    busy = true;
    try {
      for (let batch = 0; batch < 6; ++batch) {
        const status = await fetchJson(`/api/debug/log?after=${lastSeq}`);
        if (!attached()) return;
        if (!status.enabled) {
          showMessage(I18n.t("text.70a26ac89ca5"), "warning");
          return;
        }
        showMessage("");
        if (lastSeq !== 0 && status.firstSeq > lastSeq + 1) {
          appendLines([{ text: I18n.f("text.6aef072d5fba", {p0: status.firstSeq - lastSeq - 1}) }]);
        }
        const lines = status.lines || [];
        appendLines(lines);
        if (lines.length) lastSeq = lines[lines.length - 1].seq;
        $("debug-info").textContent =
          I18n.f("text.31d3bbe81890", {p0: status.count, p1: status.capacity}) +
          (status.dropped ? I18n.f("text.b6f413f08433", {p0: status.dropped}) : "");
        if (lines.length === 0) break;
      }
    } catch (error) {
      showMessage(error.message, "danger");
    } finally {
      busy = false;
      schedule();
    }
  }

  $("debug-pause").addEventListener("click", () => {
    paused = !paused;
    $("debug-pause").textContent = paused ? I18n.t("text.d640c7421da0") : I18n.t("text.858e4ba7a29f");
    $("debug-pause").classList.toggle("btn-warning", paused);
    $("debug-pause").classList.toggle("btn-outline-secondary", !paused);
    if (!paused) void poll();
  });

  $("debug-clear").addEventListener("click", async () => {
    try {
      const result = JSON.parse(await post("/api/debug/clear"));
      lastSeq = result.nextSeq - 1;
      $("debug-output").textContent = "";
      showMessage(I18n.t("text.8b40be6f56f2"), "success");
    } catch (error) {
      showMessage(error.message, "danger");
    }
  });

  $("debug-download").addEventListener("click", () => {
    const text = [...$("debug-output").children].map((row) => row.textContent).join("\n");
    const url = URL.createObjectURL(new Blob([text], { type: "text/plain" }));
    const link = document.createElement("a");
    link.href = url;
    link.download = "device-log.txt";
    link.click();
    URL.revokeObjectURL(url);
  });

  void poll();
})();
