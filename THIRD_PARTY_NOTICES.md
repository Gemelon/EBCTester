# Third-party notices

No license is currently granted for EBCTester's original source code. The
following dependencies and bundled assets retain their upstream terms.

| Dependency | Version / provenance | Terms and notices |
| --- | --- | --- |
| Bootstrap CSS (in ota-suite submodule) | 5.3.3, https://getbootstrap.com/ | MIT; [bundled license](external/ota-suite/components/web_interface/assets/BOOTSTRAP-LICENSE) |
| Chart.js | 4.4.9, https://www.chartjs.org/ | MIT; [bundled license](components/battery_dashboard/assets/CHARTJS-LICENSE.md); upstream header retained in chart.umd.js |
| SQLite amalgamation | 3.53.4 (`3530400`), https://sqlite.org/ | Public domain, https://sqlite.org/copyright.html; downloaded during build with pinned SHA3-256, not vendored |
| ESP-IDF | Tested with 6.0.1, https://github.com/espressif/esp-idf | Apache-2.0 for IDF's original code, with additional third-party licenses in the SDK |
| espressif/cjson | Resolved by dependencies.lock | MIT; license supplied by the managed component |
| espressif/led_strip | Resolved by dependencies.lock | Apache-2.0; license supplied by the managed component |

The `ota-suite` and `sd-card-manager` submodules are original project packages,
not implicitly covered by Bootstrap's or ESP-IDF's license. See each package's
README and notices. Keep upstream copyright and license files with any
redistributed third-party source/assets.
