// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";
(() => {
  const $ = (id) => document.getElementById(id);
  async function reset(scope) {
    const description = scope === "all" ? "Erase all NVS settings, including Wi-Fi credentials" : "Reset device configuration";
    if (!window.confirm(`${description} and restart? SD data will be preserved.`)) return;
    $("config-reset").disabled = $("config-reset-all").disabled = true;
    let restarting = false;
    try {
      const headers = await WebAuth.settingsHeaders();
      if (headers === null) return;
      const response = await fetch("/api/config/reset", {
        method: "POST", cache: "no-store", credentials: "same-origin",
        headers: { "Content-Type": "application/json", ...headers },
        body: JSON.stringify({ scope, confirm: true }),
      });
      const text = await response.text();
      if (!response.ok) throw new Error(text);
      restarting = true;
      $("config-reset-message").textContent = text;
      $("config-reset-message").className = "alert alert-info mt-3";
    } catch (error) {
      $("config-reset-message").textContent = error.message;
      $("config-reset-message").className = "alert alert-danger mt-3";
    } finally {
      $("config-reset").disabled = $("config-reset-all").disabled = restarting;
    }
  }
  $("config-reset").addEventListener("click", () => void reset("config"));
  $("config-reset-all").addEventListener("click", () => void reset("all"));
})();
