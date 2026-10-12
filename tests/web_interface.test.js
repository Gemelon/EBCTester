const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const { TextEncoder } = require("node:util");

const root = path.resolve(__dirname, "..");
const suite = process.env.OTA_SUITE_DIR || path.join(root, "external", "ota-suite");
const script = fs.readFileSync(path.join(suite, "components", "web_interface", "assets", "app.js"), "utf8");
const assets = path.join(suite, "components", "web_interface", "assets");
const languageScript = fs.readFileSync(path.join(assets, "i18n.js"), "utf8");
const languages = {
  en: JSON.parse(fs.readFileSync(path.join(assets, "en.json"), "utf8")),
  de: JSON.parse(fs.readFileSync(path.join(assets, "de.json"), "utf8")),
  sdEnabled: false,
};

const readyStatus = {
  version: "1.0", project: "EBCTester", csrfToken: "test-token",
  ssid: "Test-WLAN", apSsid: "BatteryTester-123456", ip: "192.168.1.42",
  connected: true, configuring: false, timeReady: true,
  otaConfigured: true, bootConfirmed: true, otaReady: true, otaBusy: false,
  otaInsecureHttp: false,
  otaState: "idle", otaMessage: "Bereit.", networkMessage: "Verbunden.",
  bytesRead: 0, totalBytes: 0,
  updateConfigured: true, updateChecking: false, updateAvailable: true, updateStale: false,
  availableVersion: "1.2.0", updateNotes: "Release notes", updateMessage: "Update verfuegbar.",
  updateLastSuccess: 1791535000, updateLastSuccessUptime: 10,
};

function createPage(status = readyStatus, confirmResult = true) {
  const elements = new Map();
  const requests = [];
  const timers = [];
  const confirmations = [];
  let responseStatus = { ...status };
  let actionError = null;
  let offline = false;
  let statusDelay = null;
  const element = (id) => {
    if (!elements.has(id)) {
      elements.set(id, {
        value: "", disabled: true, textContent: "", className: "",
        classList: { add() {}, remove() {}, toggle() {} }, listeners: {},
        querySelector() { return {}; },
        querySelectorAll() { return []; },
        setAttribute(name, value) { this[name] = value; },
        append() {},
        replaceChildren() {},
        addEventListener(name, listener) { this.listeners[name] = listener; },
        removeAttribute(name) { delete this[name]; },
      });
    }
    return elements.get(id);
  };
  const context = {
    document: {
      getElementById: element, querySelectorAll: () => [],
      documentElement: {}, createElement: () => ({}),
    },
    navigator: { language: "de" },
    localStorage: { getItem: () => "de" },
    EBCLanguages: languages,
    console,
    TextEncoder, AbortSignal: { timeout: () => undefined },
    confirm: (message) => { confirmations.push(message); return confirmResult; },
    setTimeout: (callback) => timers.push(callback),
    fetch: async (url, options) => {
      requests.push({ url, options });
      if (offline) throw new Error("offline");
      if (url === "/api/status") {
        const snapshot = { ...responseStatus };
        if (statusDelay) {
          const delay = statusDelay;
          statusDelay = null;
          await delay;
        }
        return { ok: true, json: async () => snapshot };
      }
      return { ok: !actionError, text: async () => actionError || "Angenommen." };
    },
  };
  context.window = context;
  vm.createContext(context);
  vm.runInContext(languageScript, context);
  vm.runInContext(script, context);
  return {
    element, requests, confirmations,
    async settle() { await new Promise(setImmediate); },
    async poll(next = responseStatus) {
      responseStatus = { ...next };
      assert.ok(timers.length > 0, "status polling must continue");
      timers.shift()();
      await this.settle();
    },
    setActionError(message) { actionError = message; },
    setOffline(value) { offline = value; },
    delayNextStatus() {
      let release;
      statusDelay = new Promise((resolve) => { release = resolve; });
      return release;
    },
    click() { element("ota-start").listeners.click(); },
    submit() { element("wifi-form").listeners.submit({ preventDefault() {} }); },
    mutations() { return requests.filter((request) => request.options.method === "POST"); },
  };
}

test("startup and repeated polling never automatically start OTA", async () => {
  const page = createPage();
  await page.settle();
  for (let i = 0; i < 5; ++i) await page.poll();
  assert.equal(page.element("ota-start").disabled, false);
  assert.equal(page.mutations().length, 0);
  assert.ok(page.requests.every((request) => request.url === "/api/status"));
});

test("OTA requires explicit confirmation and sends exactly one authenticated POST", async () => {
  const page = createPage();
  await page.settle();
  page.click();
  page.click();
  await page.settle();
  const actions = page.mutations();
  assert.equal(actions.length, 1);
  assert.equal(actions[0].url, "/api/ota");
  assert.equal(actions[0].options.headers["X-CSRF-Token"], "test-token");
  assert.equal(actions[0].options.headers["X-Update-Version"], "1.2.0");
  assert.equal(actions[0].options.body, undefined);
  assert.equal(page.element("ota-start").disabled, true);
});

test("cancelling confirmation does not download or start OTA", async () => {
  const page = createPage(readyStatus, false);
  await page.settle();
  page.click();
  await page.settle();
  assert.equal(page.mutations().length, 0);
});

test("HTTP test mode warns and permits manual start without synchronized time", async () => {
  const page = createPage({ ...readyStatus, otaInsecureHttp: true, timeReady: false });
  await page.settle();
  assert.equal(page.element("ota-start").disabled, false);
  assert.match(page.element("ota-transport-warning").textContent, /manipuliert/);
  assert.doesNotMatch(page.element("ota-transport-warning").className, /d-none/);
  assert.doesNotMatch(page.element("ota-requirement").textContent, /Uhrzeit/);
  await page.poll();
  assert.equal(page.mutations().length, 0);
  page.click();
  await page.settle();
  assert.match(page.confirmations[0], /HTTP-Testadresse/);
  assert.match(page.confirmations[0], /Serveridentitaet ungeprueft/);
  assert.equal(page.mutations().length, 1);
});

test("HTTPS retains the time requirement and has no HTTP warning", async () => {
  const page = createPage({ ...readyStatus, timeReady: false, otaReady: false });
  await page.settle();
  assert.equal(page.element("ota-start").disabled, true);
  assert.match(page.element("ota-requirement").textContent, /Uhrzeit/);
  assert.match(page.element("ota-transport-warning").className, /d-none/);
  await page.poll(readyStatus);
  page.click();
  await page.settle();
  assert.match(page.confirmations[0], /HTTPS-Adresse/);
  assert.doesNotMatch(page.confirmations[0], /HTTP-Testadresse/);
});

for (const [name, blocked] of Object.entries({
  "missing HTTPS URL": { otaConfigured: false },
  "disconnected WLAN": { connected: false },
  "missing certificate time": { timeReady: false },
  "unconfirmed boot": { bootConfirmed: false },
  "network configuration": { configuring: true },
  "active OTA": { otaBusy: true, otaState: "downloading" },
  "no newer version": { updateAvailable: false },
  "stale candidate": { updateStale: true },
  "active availability check": { updateChecking: true },
})) {
  test(`OTA is disabled with ${name}`, async () => {
    const page = createPage({ ...readyStatus, ...blocked, otaReady: false });
    await page.settle();
    assert.equal(page.element("ota-start").disabled, true);
    page.click();
    await page.settle();
    assert.equal(page.mutations().length, 0);
  });
}

test("busy OTA disables WLAN changes and renders measurable progress", async () => {
  const page = createPage({
    ...readyStatus, otaBusy: true, otaReady: false, otaState: "downloading",
    bytesRead: 512, totalBytes: 1024,
  });
  await page.settle();
  assert.equal(page.element("ota-progress").value, 50);
  assert.equal(page.element("wifi-save").disabled, true);
  page.submit();
  assert.equal(page.mutations().length, 0);
});

test("chunked download displays indeterminate progress", async () => {
  const page = createPage({
    ...readyStatus, otaBusy: true, otaReady: false, bytesRead: 512, totalBytes: 0,
  });
  await page.settle();
  assert.equal(Object.hasOwn(page.element("ota-progress"), "value"), false);
  assert.match(page.element("ota-bytes").textContent, /512 Bytes/);
});

test("failed firmware is shown as an error, with retry only by another click", async () => {
  const page = createPage({
    ...readyStatus, otaState: "failed", otaMessage: "Firmware nicht akzeptiert.",
  });
  await page.settle();
  await page.poll();
  assert.match(page.element("ota-message").className, /alert-danger/);
  assert.equal(page.element("ota-start").disabled, false);
  assert.equal(page.mutations().length, 0);
});

test("lost response does not cause automatic OTA retry", async () => {
  const page = createPage();
  await page.settle();
  page.setActionError("Verbindung unterbrochen");
  page.click();
  await page.settle();
  assert.match(page.element("action-result").textContent, /nicht automatisch wiederholen/);
  await page.poll({ ...readyStatus, otaBusy: true, otaReady: false });
  assert.equal(page.mutations().length, 1);
});

test("a stale status response cannot re-enable OTA after a manual start", async () => {
  const page = createPage();
  await page.settle();
  const release = page.delayNextStatus();
  await page.poll();
  page.click();
  await page.settle();
  release();
  await page.settle();
  assert.equal(page.element("ota-start").disabled, true);
  page.click();
  assert.equal(page.mutations().length, 1);
  await page.poll({ ...readyStatus, otaBusy: true, otaReady: false });
  assert.equal(page.element("ota-start").disabled, true);
});

test("offline status disables both controls and reports an explicit error", async () => {
  const page = createPage();
  await page.settle();
  page.setOffline(true);
  await page.poll();
  assert.equal(page.element("ota-start").disabled, true);
  assert.equal(page.element("wifi-save").disabled, true);
  assert.match(page.element("connection-error").textContent, /offline/);
  page.setOffline(false);
  await page.poll();
  assert.equal(page.element("ota-start").disabled, false);
});

test("WLAN submit clears the password field and sends the expected JSON", async () => {
  const page = createPage();
  await page.settle();
  page.element("ssid").value = "Heimnetz";
  page.element("password").value = "test-password";
  page.submit();
  await page.settle();
  assert.equal(page.element("password").value, "");
  assert.deepEqual(JSON.parse(page.mutations()[0].options.body),
    { ssid: "Heimnetz", password: "test-password" });
  assert.equal(page.mutations()[0].url, "/api/network");
});

test("WLAN lengths are validated as UTF-8 bytes, not character counts", async () => {
  const page = createPage();
  await page.settle();
  page.element("ssid").value = "\u00e4".repeat(17);
  page.element("password").value = "test-password";
  page.submit();
  await page.settle();
  assert.equal(page.mutations().length, 0);
  assert.match(page.element("action-result").className, /alert-danger/);
});

test("invalid 64-byte non-hex WPA password is rejected", async () => {
  const page = createPage();
  await page.settle();
  page.element("ssid").value = "Heimnetz";
  page.element("password").value = "z".repeat(64);
  page.submit();
  await page.settle();
  assert.equal(page.mutations().length, 0);
});

test("status strings are rendered as text, not executable HTML", async () => {
  const page = createPage({ ...readyStatus, ssid: "<img src=x onerror=alert(1)>" });
  await page.settle();
  assert.equal(page.element("network").textContent, "<img src=x onerror=alert(1)>");
  assert.equal(page.element("network").innerHTML, undefined);
});

test("manual availability check sends one CSRF-protected POST and never installs", async () => {
  const page = createPage();
  await page.settle();
  page.element("update-check").listeners.click();
  page.element("update-check").listeners.click();
  await page.settle();
  assert.equal(page.mutations().length, 1);
  assert.equal(page.mutations()[0].url, "/api/updates/check");
  assert.equal(page.mutations()[0].options.headers["X-CSRF-Token"], "test-token");
  assert.equal(page.confirmations.length, 0);
  await page.poll({ ...readyStatus, updateChecking: true });
  assert.equal(page.element("update-check").disabled, true);
  assert.equal(page.element("ota-start").disabled, true);
});

test("availability notes and last successful check are safe text", async () => {
  const notes = "<img src=x onerror=alert(1)>";
  const page = createPage({ ...readyStatus, updateNotes: notes, updateStale: true, updateAvailable: false });
  await page.settle();
  assert.equal(page.element("update-notes").textContent, notes);
  assert.equal(page.element("update-notes").innerHTML, undefined);
  assert.match(page.element("update-last-success").textContent, /Letzte erfolgreiche Pruefung/);
  assert.match(page.element("update-version").textContent, /veraltet/);
  page.click();
  assert.equal(page.mutations().length, 0);
});
