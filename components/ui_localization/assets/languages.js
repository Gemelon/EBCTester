"use strict";
document.getElementById("languages-refresh").addEventListener("click", () => {
  if (window.confirm(I18n.t("language.reload"))) location.reload();
});
