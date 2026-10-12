<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# Third-party notices

EBCTester's original code and documentation: **Copyright 2026 Thomas Stoll,
Apache-2.0**, see [LICENSE](LICENSE) and [NOTICE](NOTICE).
This does not relicense any upstream software.

## Bundled assets and directly resolved dependencies

| Dependency | Provenance | Unmodified license / attribution |
| --- | --- | --- |
| Bootstrap 5.3.3 | https://github.com/twbs/bootstrap/tree/v5.3.3, supplied by ota-suite | [MIT](external/ota-suite/components/web_interface/assets/BOOTSTRAP-LICENSE), Copyright 2011-2024 The Bootstrap Authors |
| Chart.js 4.4.9 | https://github.com/chartjs/Chart.js/tree/v4.4.9 | [MIT](components/battery_dashboard/assets/CHARTJS-LICENSE.md), Copyright 2014-2024 Chart.js Contributors; bundle header additionally credits 2025 contributors |
| @kurkle/color 0.3.2 | https://github.com/kurkle/color/tree/v0.3.2, included in Chart.js | [MIT](components/battery_dashboard/assets/KURKLE-COLOR-LICENSE.md), Copyright 2018-2021 Jukka Kurkela; bundle header additionally credits 2023 |
| SQLite 3.53.4 | https://sqlite.org/2026/sqlite-amalgamation-3530400.zip | Public domain, https://sqlite.org/copyright.html; original SQLite source notices remain in the downloaded archive |
| espressif/cjson 1.7.19~2 | https://components.espressif.com/components/espressif/cjson | [MIT](third_party/licenses/cjson/LICENSE), Dave Gamble and cJSON contributors |
| espressif/led_strip 3.1.0~1 | https://components.espressif.com/components/espressif/led_strip | [Apache-2.0](third_party/licenses/led_strip/LICENSE), original Espressif source headers remain in managed_components |

Bootstrap and Chart.js bundles and their existing license files are not
modified by the project's Apache licensing. The missing kurkle license text
is included separately, not substituted for Chart.js's license.
The original SQLite adapters in this repository use Apache-2.0; this does
not change SQLite's public-domain status.

## SDK and runtime

The snapshots in [third_party/licenses/](third_party/licenses/) come from the
local **ESP-IDF 6.0.1**, resolved managed components and **Xtensa toolchain
esp-15.2.0_20251204**. Full upstream texts are copied without rewriting their
copyright statements. They are documentation, not vendored SDK source.

| Upstream source location | Local license snapshot |
| --- | --- |
| ESP-IDF `LICENSE` | [esp-idf/LICENSE](third_party/licenses/esp-idf/LICENSE) |
| `components/freertos/FreeRTOS-Kernel/LICENSE.md` | [FreeRTOS-LICENSE.md](third_party/licenses/esp-idf/FreeRTOS-LICENSE.md) |
| `components/lwip/lwip/COPYING` | [lwip-COPYING](third_party/licenses/esp-idf/lwip-COPYING) |
| License header in `components/fatfs/src/ff.c` | [FatFs-NOTICE.txt](third_party/licenses/esp-idf/FatFs-NOTICE.txt), preserved verbatim |
| `components/mbedtls/mbedtls/LICENSE` | [mbedtls-LICENSE](third_party/licenses/esp-idf/mbedtls-LICENSE) |
| `components/mbedtls/mbedtls/tf-psa-crypto/LICENSE` | [tf-psa-crypto-LICENSE](third_party/licenses/esp-idf/tf-psa-crypto-LICENSE) |
| `components/wpa_supplicant/README` and `COPYING` | [README](third_party/licenses/esp-idf/wpa_supplicant-README), [COPYING](third_party/licenses/esp-idf/wpa_supplicant-COPYING) |
| `components/http_parser/LICENSE.txt` | [http_parser-LICENSE.txt](third_party/licenses/esp-idf/http_parser-LICENSE.txt) |
| `components/esp_libc/COPYING.NEWLIB` and `COPYING.picolibc` | [Newlib](third_party/licenses/esp-idf/COPYING.NEWLIB), [Picolibc](third_party/licenses/esp-idf/COPYING.picolibc) |
| `components/esp_wifi/lib/LICENSE`, `esp_phy/lib/LICENSE`, `esp_coex/lib/LICENSE` | [Wi-Fi](third_party/licenses/esp-idf/esp_wifi-LICENSE), [PHY](third_party/licenses/esp-idf/esp_phy-LICENSE), [coexistence](third_party/licenses/esp-idf/esp_coex-LICENSE) |
| Toolchain `share/licenses/gcc/COPYING3`, `COPYING.RUNTIME`, `COPYING3.LIB` | [GPL-3.0](third_party/licenses/toolchain/COPYING3), [Runtime Library Exception](third_party/licenses/toolchain/COPYING.RUNTIME), [LGPL-3.0](third_party/licenses/toolchain/COPYING3.LIB) |
| Toolchain `share/licenses/newlib/COPYING.LIBGLOSS` | [libgloss](third_party/licenses/toolchain/COPYING.LIBGLOSS) |
| `components/console/argtable3/LICENSE`, `console/linenoise/LICENSE` | [Argtable3](third_party/licenses/esp-idf/argtable3-LICENSE), [linenoise](third_party/licenses/esp-idf/linenoise-LICENSE) |
| `components/protobuf-c/protobuf-c/LICENSE`, `spiffs/spiffs/LICENSE` | [protobuf-c](third_party/licenses/esp-idf/protobuf-c-LICENSE), [SPIFFS](third_party/licenses/esp-idf/spiffs-LICENSE) |
| TLSF upstream `tlsf.h`, https://github.com/mattconte/tlsf | [Original license block](third_party/licenses/esp-idf/TLSF-NOTICE.txt), Copyright 2006-2016 Matthew Conte |

[SOURCE-NOTICES.txt](third_party/licenses/esp-idf/SOURCE-NOTICES.txt) preserves
individual copyright/license comments from compiled SDK sources in the
reference build, grouped without rewriting them. This is a conservative
superset, not a claim every object survives linking. SPDX-only notices are
accompanied by full [standard license texts](third_party/licenses/standard/),
including BSD, MIT and CC0 terms. Supporting snapshots for optional SDK
components are included too. New configurations require updating this inventory.

Mbed TLS / TF-PSA-Crypto are used under their Apache-2.0 option where
dual-licensed. That choice does not replace BSD or other notices in
individual files. Newlib/Picolibc are multi-author collections, not a single
blanket Apache-licensed library. GCC runtime libraries use their runtime
exception; the GPL licenses of build tools do not by themselves change the
application's license.

ESP-IDF contains further upstream code and file-level copyright/license
headers. The table is an overview of the documented dependency stack, not a
claim that every SDK component is linked or that every SDK file is Apache-2.0.
Retain source-level notices when redistributing upstream source, and review
additional linked objects and their terms when changing configuration,
toolchain, SDK or dependencies. The reference build map and
`build/project_description.json` identify the actual build.

## Redistributing source or firmware

- Keep this project's `LICENSE` and `NOTICE`, and the package-level equivalents
  for OTA Suite and SD Card Manager.
- Include this document, the `third_party/licenses` directory, the Bootstrap,
  Chart.js and kurkle license texts, and the packages' third-party notices.
  Deliver these as accompanying documentation even for a binary-only release.
  A link in a README alone does not replace required delivered license texts.
- Preserve MIT copyright/permission notices and BSD notices/conditions/disclaimers.
  Retain any upstream `NOTICE` applicable to newly included Apache components.
- Mark modified Apache-covered files and retain relevant copyright,
  attribution, patent and trademark notices. Do not imply upstream endorsement.
- A raw `.bin` does not contain all these documents. EBCTester's enabled
  publication target copies accompanying materials to
  `licenses/EBCTester-<version>/` before publishing the image and manifest.
  Distribute that complete directory with the firmware. The standalone OTA
  Suite publisher only copies images/manifest; its consumers must supply
  their own application's legal distribution materials.
- Managed and SDK source are obtained from their upstream distributions;
  their original notices must remain with any source copies you redistribute.

The reusable project packages use Apache-2.0 too:
[OTA Suite notices](external/ota-suite/THIRD_PARTY_NOTICES.md) and
[SD Card Manager notices](external/sd-card-manager/THIRD_PARTY_NOTICES.md).
