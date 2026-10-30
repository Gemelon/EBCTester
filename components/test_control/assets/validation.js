// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

"use strict";

export function setNumericBounds(input, bounds) {
  for (const [name, value] of Object.entries(bounds)) {
    if (input[name] !== String(value)) input[name] = String(value);
  }
}

export function bindNumericValidation(form) {
  const inputs = [...form.querySelectorAll('input[type="number"]')];
  const touched = new Set();
  const feedback = new Map();
  form.noValidate = true;

  for (const input of inputs) {
    const message = document.createElement("div");
    message.id = `${input.id}-error`;
    message.className = "invalid-feedback";
    message.setAttribute("aria-live", "polite");
    input.insertAdjacentElement("afterend", message);
    const describedBy = input.getAttribute("aria-describedby");
    input.setAttribute("aria-describedby", [describedBy, message.id].filter(Boolean).join(" "));
    feedback.set(input, message);
    input.addEventListener("blur", () => {
      touched.add(input);
      refresh();
    });
    input.addEventListener("input", refresh);
  }

  function refresh() {
    for (const input of inputs) {
      const invalid = !input.disabled && touched.has(input) && !input.validity.valid;
      const message = invalid ? input.validationMessage : "";
      input.classList.toggle("is-invalid", invalid);
      input.setAttribute("aria-invalid", String(invalid));
      if (feedback.get(input).textContent !== message) feedback.get(input).textContent = message;
    }
  }

  function validateAll() {
    for (const input of inputs) touched.add(input);
    refresh();
    const invalid = inputs.find(input => !input.disabled && !input.validity.valid);
    if (invalid) invalid.focus();
    return !invalid;
  }

  form.addEventListener("submit", event => {
    if (!validateAll()) {
      event.preventDefault();
      event.stopImmediatePropagation();
    }
  });
  return { refresh, validateAll };
}
