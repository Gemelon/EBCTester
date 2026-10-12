// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";
document.getElementById("languages-refresh").addEventListener("click", () => {
  if (window.confirm(I18n.t("language.reload"))) location.reload();
});
