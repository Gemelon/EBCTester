<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# Development handbook

This file describes how to continue developing EBCTester on another machine,
which repositories and branches exist, what has been changed compared with
upstream, and how to build, flash and test without and with hardware.

## Repositories and branches

Changes are developed in forks and reach the upstream repositories through
pull requests:

| Upstream (no write access) | Fork (development) |
| --- | --- |
| `https://github.com/Gemelon/EBCTester` | `https://github.com/matzeeg3/EBCTester` |
| `https://github.com/Gemelon/ota-suite` | `https://github.com/matzeeg3/ota-suite` |
| `https://github.com/Gemelon/sd-card-manager` | `https://github.com/matzeeg3/sd-card-manager` |

EBCTester uses two Git submodules, `external/ota-suite` and
`external/sd-card-manager`. `.gitmodules` keeps the upstream URLs. Until the
submodule pull requests are merged, the commits that the EBCTester branches
refer to exist only in the submodule forks, so a checkout fetches the
submodules from the forks (see "Setting up a new machine").

Branches:

| Repository | Branch | Content |
| --- | --- | --- |
| EBCTester | `raise-current-limit-40a` | 40 A current limit, resistance test defaults (150 W, 3.8 V, 2.0 V), EBC-A20/A40L documentation, HTTP header limit 4096. Pull request [Gemelon/EBCTester#1](https://github.com/Gemelon/EBCTester/pull/1). |
| EBCTester | `tester-model-and-sd-bus-settings` | Builds on the branch above: tester model and SD bus settings, translations, host tests, mock web UI server, this handbook. **Current development branch.** |
| ota-suite | `raise-extension-limits` | 16 extensions / 96 URI handlers, English and German texts. |
| sd-card-manager | `runtime-sd-bus-mode` | SD bus mode selectable at runtime, settings card. |

Merge order: the ota-suite and sd-card-manager pull requests first, then the
EBCTester pull requests (afterwards `.gitmodules` needs no override any more).

## Changes compared with upstream

**Tester model (EBC-A20 / EBC-A40L).** A settings card selects the connected
tester; it is stored in NVS (namespace `tester`, key `model`, default A20).
The model sets the charge and discharge current limit: 20 A (A20) or 40 A
(A40L). Manual starts, test plans (load and start, also via MQTT) and the
internal resistance measurement are checked against it; the Manual,
Automatic and Internal resistance forms adopt the limit from
`/api/tester/status` (`testerModel`, `maxCurrent`). The checks live in
`components/test_control/tester_model.cpp` without ESP-IDF dependencies.
The absolute plan format limit in `test_store` is 40 A.

**SD bus mode at runtime.** The Kconfig choice in sd-card-manager now only
sets the default; both pin sets are always compiled in. The "SD card bus"
settings card stores SPI or 4-bit SDMMC in NVS (namespace `sd_card`, key
`bus`); it applies after a restart. "Save and restart" refuses to restart
while `test_control_is_busy()` reports an operation or recording. The web
interface works without an SD card, so a wrong mode can always be switched
back.

**Other changes.**
- Internal resistance test defaults: max. power 150 W, max. cell voltage
  3.8 V, cutoff voltage 2.0 V.
- `CONFIG_HTTPD_MAX_REQ_HDR_LEN=4096`: browsers with many cookies for the
  device address otherwise get "header fields are too long".
- ota-suite allows 16 web extensions and 96 URI handlers (the two new
  settings cards exceeded the old limits of 12 and 72).
- The new cards, the Debug log card and tab, and the Home Assistant MQTT card
  use the shared translation catalog (`en.json`, `de.json` in ota-suite and
  `sd_card/languages/en-custom.json`, which must contain the same strings as
  `en.json`). Device messages of these cards are translated too.
- Documentation: README, `docs/TECHNICAL_REFERENCE.md` (settings, APIs,
  tests).

The details are in the commit messages of the branches listed above.

## Setting up a new machine

Tested on Fedora Linux (aarch64) with ESP-IDF 6.0.1.

### 1. System packages

```text
sudo dnf install git python3 cmake ninja-build gcc gcc-c++ libusb1
```

On Debian/Ubuntu: `sudo apt install git python3 python3-venv cmake ninja-build
build-essential libusb-1.0-0`. Google Chrome or Chromium is optional and only
needed for the browser part of the UI tests. Node.js is optional for the
upstream JavaScript tests (`node --test tests/...`).

Add your user to the serial port group and log in again:

```text
sudo usermod -aG dialout $USER
```

### 2. ESP-IDF 6.0.1

```text
mkdir -p ~/esp && cd ~/esp
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

Load the environment in every new shell before using `idf.py`:

```text
. ~/esp/esp-idf/export.sh
```

### 3. Clone the project including the submodules

```text
git clone https://github.com/matzeeg3/EBCTester.git EBCTester
cd EBCTester
git checkout tester-model-and-sd-bus-settings
git submodule init
git config submodule.external/ota-suite.url https://github.com/matzeeg3/ota-suite.git
git config submodule.external/sd-card-manager.url https://github.com/matzeeg3/sd-card-manager.git
git submodule update
git remote add upstream https://github.com/Gemelon/EBCTester.git
```

The two `git config` lines only change the local checkout so that the
submodules come from the forks; `.gitmodules` keeps the upstream URLs. Once
the submodule pull requests are merged, plain
`git clone --recurse-submodules https://github.com/Gemelon/EBCTester.git`
works again.

To continue working inside a submodule, check out its branch first (a
submodule is otherwise in "detached HEAD" state) and add upstream as a
second remote:

```text
git -C external/ota-suite checkout -B raise-extension-limits origin/raise-extension-limits
git -C external/ota-suite remote add upstream https://github.com/Gemelon/ota-suite.git
git -C external/sd-card-manager checkout -B runtime-sd-bus-mode origin/runtime-sd-bus-mode
git -C external/sd-card-manager remote add upstream https://github.com/Gemelon/sd-card-manager.git
```

After committing in a submodule, push it to its fork (this updates its pull
request) and record the new submodule commit in EBCTester:

```text
git -C external/ota-suite push origin raise-extension-limits
git add external/ota-suite && git commit
git push origin tester-model-and-sd-bus-settings
```

### 4. Local configuration (never committed)

`sdkconfig` is generated from `sdkconfig.defaults` on the first build and is
ignored by Git. Settings that are specific to one device or person belong
in a local file, never in the repository. In particular the **admin/AP
password stays empty in the repository**, so every device generates a random
password on first start and prints it on the serial console.

Copy the example and adjust it:

```text
cp sdkconfig.local.example sdkconfig.local
```

`sdkconfig.local` is ignored by Git. It is read only when `sdkconfig` is
created, so use it for the first build on a machine. The first command
creates `sdkconfig`; the second one is needed because the CMake files decide
from the existing `sdkconfig` whether the optional MQTT component is built:

```text
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.local" reconfigure
idf.py reconfigure build
```

To change a value later, use `idf.py menuconfig` (or delete `sdkconfig` and
run both commands above again). The values used for the current test
devices are:

| Setting | Value | Where |
| --- | --- | --- |
| Flash size | 16 MB (required by `partitions.csv`) | `sdkconfig.defaults` |
| SD bus default | SPI (MISO 2, MOSI 15, CS 6, CLK 14, detect 7) | `sdkconfig.local` or the web setting |
| Home Assistant MQTT | enabled in the build | `sdkconfig.local` |
| Admin/AP password | optional fixed password for your own devices | `sdkconfig.local` only |

`main/Kconfig.projbuild` keeps an empty default for
`BATTERY_AP_PASSWORD`; do not change it in the repository.

## Build, flash and update

```text
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor      # Ctrl+] leaves the monitor
```

Use `ls /dev/ttyACM* /dev/ttyUSB*` to find the port. `idf.py flash` keeps
NVS (Wi-Fi, settings, stored password); `idf.py -p PORT erase-flash` clears
it.

Single image for flashing on another computer with esptool only (offset 0x0):

```text
cd build
esptool --chip esp32s3 merge-bin -o EBCTester-full.bin --flash-mode dio \
  --flash-size 16MB --flash-freq 80m 0x0 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin 0xf000 ota_data_initial.bin \
  0x20000 EBCTester.bin
esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 write-flash 0x0 EBCTester-full.bin
```

(esptool 4.x and older: `esptool.py`, `merge_bin`, `write_flash`.)

OTA updates download `firmware.json` and the image from a web server; there
is no upload in the web UI. Increase `PROJECT_VER` in `CMakeLists.txt`,
publish as described in the README ("Updates and publishing firmware") and
set the manifest URL on the device. Plain HTTP must be allowed explicitly on
the device and only on a trusted network.

## Tests

None of these tests flash hardware or send tester commands.

| Test | Command | Needs |
| --- | --- | --- |
| Database schema | `python3 tests/resistance_schema_test.py` | Python |
| Tester model limits | see `docs/TECHNICAL_REFERENCE.md` (g++ command for `tests/tester_model_host`) | gcc-c++ |
| DCIR host test | see `docs/TECHNICAL_REFERENCE.md` | gcc-c++ |
| Web UI mock and browser checks | `python3 tests/ui_mock/ui_mock_test.py` | Python; Chrome/Chromium for the browser part |
| Upstream JavaScript tests | `node --test tests/web_interface.test.js tests/publication.test.js tests/ui_languages_test.js` | Node.js |

Interactive mock of the complete web UI with a simulated device (all tabs
and settings cards, demo measurements, English/German):

```text
python3 tests/ui_mock/mock_server.py --model A40L --tab settings
```

Then open http://127.0.0.1:8080. The mock mirrors the firmware's messages and
limits; keep `tests/ui_mock/mock_server.py` in sync when changing the API.

Still to be verified on hardware: SD mounting in SPI and SDMMC mode, the bus
switch with restart, NVS persistence of tester model and bus mode, and
operation with the EBC-A40L at currents above 20 A.

## Hardware notes

- The firmware requires an ESP32-S3 with **16 MB flash**. On an 8 MB module
  the app aborts with "Detected size(8192k) smaller than the size in the
  binary image header(16384k)" and reboots in a loop.
- The EBC-A20/A40L use 5 V TTL levels: connect UART1 (TX 17, RX 18, 9600 8O1)
  through a level shifter.
- A card that answers the SD initialization but fails with
  `sdmmc_init_spi_crc ... 0x106` in the log was defective in testing;
  replacing the card solved it.
- 4-bit SDMMC additionally needs D1 (GPIO 4), D2 (GPIO 5) and pull-ups on
  CMD and D0–D3. Many ready-made SPI SD modules do not route D1/D2.
- The Debug log (Settings) shows the device log over Wi-Fi; bootloader output
  still needs the USB console.

## Open items

- Review and merge of the pull requests (submodules first, see "Merge order").
- Hardware verification listed under "Tests".
- Older German catalog texts use "ae/oe/ue/ss" instead of umlauts; new texts
  use umlauts.
