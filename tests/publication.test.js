// SPDX-FileCopyrightText: 2026 Thomas Stoll
// SPDX-License-Identifier: Apache-2.0

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");
const { spawnSync } = require("node:child_process");

const root = path.resolve(__dirname, "..");
const suite = process.env.OTA_SUITE_DIR || path.join(root, "external", "ota-suite");
const cmake = process.env.CMAKE_EXE || "cmake";
const script = path.join(suite, "cmake", "publish_firmware.cmake");

test("fresh checkouts use pinned packages and opt-in publication without private defaults", () => {
  const source = fs.readFileSync(path.join(root, "CMakeLists.txt"), "utf8");
  assert.match(source, /\/external\/ota-suite/);
  assert.match(source, /\/external\/sd-card-manager/);
  assert.match(source, /option\(EBCTESTER_PUBLISH_FIRMWARE[^\n]* OFF\)/);
  assert.match(source, /CMakeUserConfig\.cmake" OPTIONAL/);
  assert.match(source, /if\(EBCTESTER_PUBLISH_FIRMWARE\)/);
  assert.doesNotMatch(source, /192\.168\.0\.236|J:/);
  const defaults = fs.readFileSync(path.join(root, "sdkconfig.defaults"), "utf8");
  assert.match(defaults, /https:\/\/updates\.example\.org/);
  assert.match(defaults, /CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=n/);
  const settings = fs.readFileSync(path.join(root, "components", "mariadb_sync", "mariadb_sync_internal.h"), "utf8");
  assert.match(settings, /bool enabled = false;/);
  assert.match(settings, /std::string host;/);
});

test("firmware publication includes original and third-party license documents", () => {
  const dir = path.join(__dirname, `.publication-licenses-${process.pid}`);
  fs.mkdirSync(dir);
  try {
    const sd = process.env.SD_CARD_MANAGER_DIR || path.join(root, "external", "sd-card-manager");
    const result = spawnSync(cmake, [
      `-DPROJECT_SOURCE_DIR=${root}`, `-DOTA_SUITE_DIR=${suite}`,
      `-DSD_CARD_MANAGER_DIR=${sd}`, `-DDESTINATION_DIR=${dir}`,
      "-DFIRMWARE_VERSION=9.9.9", "-P", path.join(root, "cmake", "copy_license_bundle.cmake"),
    ], { encoding: "utf8" });
    assert.equal(result.status, 0, result.error?.message || result.stderr);
    const bundle = path.join(dir, "licenses", "EBCTester-9.9.9");
    for (const [source, relative] of [[root, ""], [suite, "external/ota-suite"], [sd, "external/sd-card-manager"]]) {
      for (const name of ["LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"]) {
        assert.deepEqual(fs.readFileSync(path.join(bundle, relative, name)), fs.readFileSync(path.join(source, name)));
      }
      assert.deepEqual(
        fs.readFileSync(path.join(bundle, relative, "third_party", "licenses", "cjson", "LICENSE")),
        fs.readFileSync(path.join(source, "third_party", "licenses", "cjson", "LICENSE")));
    }
    for (const file of ["CHARTJS-LICENSE.md", "KURKLE-COLOR-LICENSE.md"]) {
      const relative = path.join("components", "battery_dashboard", "assets", file);
      assert.deepEqual(fs.readFileSync(path.join(bundle, relative)), fs.readFileSync(path.join(root, relative)));
    }
    const bootstrap = path.join("components", "web_interface", "assets", "BOOTSTRAP-LICENSE");
    assert.deepEqual(fs.readFileSync(path.join(bundle, "external", "ota-suite", bootstrap)),
      fs.readFileSync(path.join(suite, bootstrap)));
    const invalid = spawnSync(cmake, [
      `-DPROJECT_SOURCE_DIR=${dir}`, `-DOTA_SUITE_DIR=${suite}`,
      `-DSD_CARD_MANAGER_DIR=${sd}`, `-DDESTINATION_DIR=${dir}`,
      "-DFIRMWARE_VERSION=9.9.9", "-P", path.join(root, "cmake", "copy_license_bundle.cmake"),
    ], { encoding: "utf8" });
    assert.notEqual(invalid.status, 0);
    assert.match(invalid.stderr, /Required license document is missing/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test("publication uses real binary metadata and immutable versioned releases", () => {
  const dir = path.join(__dirname, `.publication-${process.pid}`);
  fs.mkdirSync(dir);
  try {
    const source = path.join(dir, "source.bin");
    const destination = path.join(dir, "nas");
    fs.mkdirSync(destination);
    const bytes = Buffer.from("test ESP image bytes, not ELF descriptor");
    fs.writeFileSync(source, bytes);
    const publish = (version, trailingSeparator = false) => spawnSync(cmake, [
      `-DSOURCE_FILE=${source}`, `-DDESTINATION_DIR=${destination}${trailingSeparator ? path.sep : ""}`,
      `-DFIRMWARE_VERSION=${version}`, "-DFIRMWARE_PROJECT=EBCTester",
      "-DFIRMWARE_TARGET=esp32s3", "-DFIRMWARE_ORIGIN=http://updates.example.org:8094",
      "-P", script,
    ], { encoding: "utf8" });
    let result = publish("1.2.2");
    assert.equal(result.status, 0, result.error?.message || result.stderr);
    const manifestFile = path.join(destination, "firmware.json");
    const manifest = JSON.parse(fs.readFileSync(manifestFile));
    assert.equal(manifest.version, "1.2.2");
    assert.equal(manifest.size, bytes.length);
    assert.equal(manifest.sha256, crypto.createHash("sha256").update(bytes).digest("hex"));
    assert.equal(manifest.target, "esp32s3");
    assert.equal(manifest.url, "http://updates.example.org:8094/EBCTester-1.2.2.bin");
    assert.deepEqual(fs.readFileSync(path.join(destination, "EBCTester-1.2.2.bin")), bytes);
    assert.deepEqual(fs.readFileSync(path.join(destination, "EBCTester.bin")), bytes);
    assert.ok(!fs.readdirSync(destination).some(name => name.endsWith(".uploading")));
    result = publish("1.2.2", true);
    assert.equal(result.status, 0, result.stderr);
    const before = fs.readFileSync(manifestFile);
    fs.writeFileSync(source, "different bytes");
    result = publish("1.2.2");
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /Immutable release/);
    assert.deepEqual(fs.readFileSync(manifestFile), before);
    assert.deepEqual(fs.readFileSync(path.join(destination, "EBCTester.bin")), bytes);
    result = publish("1.2.3");
    assert.equal(result.status, 0, result.stderr);
    assert.equal(JSON.parse(fs.readFileSync(manifestFile)).version, "1.2.3");
    assert.deepEqual(fs.readFileSync(path.join(destination, "EBCTester-1.2.2.bin")), bytes);
    assert.deepEqual(fs.readdirSync(dir).sort(), ["nas", "source.bin"]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test("OTA checks downloaded binary integrity before finalization and boot selection", () => {
  const source = fs.readFileSync(path.join(suite, "components", "ota_manager", "ota_manager.c"), "utf8");
  const perform = source.slice(source.indexOf("static void perform_update"), source.indexOf("static void ota_worker"));
  assert.ok(perform.indexOf("psa_hash_update") < perform.indexOf("esp_ota_end(handle)"));
  assert.ok(perform.indexOf("received != selected.size") < perform.indexOf("esp_ota_end(handle)"));
  assert.ok(perform.indexOf("memcmp(digest, selected.sha256") < perform.indexOf("esp_ota_end(handle)"));
  assert.ok(perform.indexOf("esp_ota_end(handle)") < perform.indexOf("esp_ota_set_boot_partition(next)"));
  assert.match(perform, /esp_ota_abort\(handle\)/);
  assert.match(perform, /\.disable_auto_redirect = true/);
  assert.doesNotMatch(perform, /esp_https_ota_finish/);
  const checker = fs.readFileSync(path.join(suite, "components", "update_check", "update_check.c"), "utf8");
  assert.doesNotMatch(checker, /esp_ota_begin|esp_ota_write|esp_ota_set_boot_partition|ota_manager_start/);
  assert.match(checker, /CHECK_INTERVAL_US \(6LL \* 60 \* 60 \* 1000000\)/);
  assert.match(checker, /RETRY_GUARD_US \(60LL \* 1000000\)/);
});
