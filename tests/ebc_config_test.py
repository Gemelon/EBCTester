# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

"""Run the real configuration core with mock NVS and GPIO; never touch hardware."""

import os
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
idf_path = os.environ.get("IDF_PATH")
if not idf_path:
    idf_path = json.loads((root / "build" / "project_description.json").read_text(encoding="utf-8"))["idf_path"]
parser = Path(idf_path) / "components" / "http_parser"
compiler = os.environ.get("CC") or shutil.which("cl") or shutil.which("cc")
if not compiler:
    raise SystemExit("A host C compiler is required.")

with tempfile.TemporaryDirectory(prefix="ebctester-config-") as temporary:
    work = Path(temporary)
    for name in ("driver/gpio.h", "esp_crc.h", "esp_log.h", "esp_rom_sys.h",
                 "esp_system.h", "nvs.h", "nvs_flash.h", "esp_random.h", "bootloader_random.h"):
        stub = work / name
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text("", encoding="ascii")
    (work / "esp_err.h").write_text(
        "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n"
        "#define ESP_ERR_INVALID_ARG 1\n#define ESP_ERR_INVALID_STATE 2\n"
        "#define ESP_ERR_NO_MEM 3\n", encoding="ascii")
    defaults = {
        "EBCTESTER_RTC_INT_GPIO": "14", "EBCTESTER_I2C_SDA_GPIO": "15",
        "EBCTESTER_I2C_SCL_GPIO": "16",
        "EBC_TESTER_UART_TX_GPIO": "17", "EBC_TESTER_UART_RX_GPIO": "18",
        "EBC_TESTER_UART_BAUD": "9600", "EBC_DONGLE_UART_TX_GPIO": "8",
        "EBC_DONGLE_UART_RX_GPIO": "9", "EBC_DONGLE_UART_BAUD": "9600",
        "TEST_CONTROL_AVERAGING_LENGTH": "20", "TEST_STORE_DEVICE_ID": '"EBC-A20"',
        "OTA_SUITE_AP_PASSWORD": '"ebctester"', "OTA_SUITE_SNTP_SERVER": '"pool.ntp.org"',
        "OTA_SUITE_UPDATE_MANIFEST_URL": '"https://example.org/firmware.json"',
        "OTA_SUITE_WEB_PASSWORD": '"ebctester"', "OTA_SUITE_WEB_PRIMARY_TABS": '"dashboard"',
        "OTA_SUITE_WEB_TAB_ORDER": '"dashboard,settings"',
        "SD_CARD_MANAGER_SDMMC_FREQ_KHZ": "12000",
        "SD_CARD_MANAGER_SPI_FREQ_KHZ": "8000",
        "SD_CARD_MANAGER_GPIO_CARD_DETECT": "13",
        "SD_CARD_MANAGER_MOUNT_POINT": '"/sdcard"',
        "SD_CARD_MANAGER_GPIO_CLK": "10", "SD_CARD_MANAGER_GPIO_CMD": "11",
        "SD_CARD_MANAGER_GPIO_D0": "12", "SD_CARD_MANAGER_GPIO_D1": "4",
        "SD_CARD_MANAGER_GPIO_D2": "5", "SD_CARD_MANAGER_GPIO_D3": "6",
        "SD_CARD_MANAGER_GPIO_SPI_MISO": "2", "SD_CARD_MANAGER_GPIO_SPI_MOSI": "7",
        "SD_CARD_MANAGER_GPIO_SPI_CS": "3", "SD_CARD_MANAGER_GPIO_SPI_CLK": "10",
    }
    for spi in (False, True):
        selected = dict(defaults)
        if spi:
            selected["SD_CARD_MANAGER_BUS_SPI"] = "1"
            selected["SD_CARD_MANAGER_CARD_DETECT_ACTIVE_HIGH"] = "1"
        else:
            selected["SD_CARD_MANAGER_BUS_SDMMC_4BIT"] = "1"
        (work / "sdkconfig.h").write_text(
            "".join(f"#define CONFIG_{name} {value}\n" for name, value in selected.items()),
            encoding="ascii")
        includes = [work, root / "components" / "ebc_config" / "include", parser]
        executable = work / ("config-spi" if spi else "config-sdmmc")
        source = root / "tests" / "ebc_config_test.c"
        if Path(compiler).name.lower() in ("cl", "cl.exe"):
            executable = executable.with_suffix(".exe")
            command = [compiler, "/nologo", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                       *[f"/I{p}" for p in includes], str(source), str(parser / "http_parser.c"), f"/Fe:{executable}"]
        else:
            command = [compiler, "-std=c11", "-Wall", "-Wextra",
                       *[f"-I{p}" for p in includes], str(source), str(parser / "http_parser.c"), "-o", str(executable)]
        subprocess.run(command, cwd=work, check=True)
        subprocess.run([str(executable)], cwd=work, check=True)
