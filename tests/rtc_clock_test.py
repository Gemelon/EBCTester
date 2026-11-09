# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0
"""Test the real RTC codec and service with mocked I2C, time and task APIs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get("CC") or shutil.which("cl") or shutil.which("cc")
if not compiler:
    raise SystemExit("A host C compiler is required.")
with tempfile.TemporaryDirectory(prefix="ebctester-rtc-") as temporary:
    work = Path(temporary)
    for name in ("driver/gpio.h", "driver/i2c_master.h", "esp_log.h",
                 "freertos/FreeRTOS.h", "freertos/task.h"):
        stub = work / name
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text("", encoding="ascii")
    (work / "esp_err.h").write_text(
        "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n"
        "#define ESP_ERR_INVALID_ARG 1\n#define ESP_ERR_INVALID_STATE 2\n"
        "#define ESP_ERR_NO_MEM 3\n", encoding="ascii")
    (work / "sys").mkdir()
    (work / "sys" / "time.h").write_text(
        "#pragma once\n#include <time.h>\nstruct timeval { time_t tv_sec; long tv_usec; };\n",
        encoding="ascii")
    includes = [work, root / "main", root / "components" / "ebc_config" / "include"]
    executable = work / "rtc-test"
    sources = [root / "tests" / "rtc_clock_test.c", root / "main" / "ds3231_time.c"]
    if Path(compiler).name.lower() in ("cl", "cl.exe"):
        executable = executable.with_suffix(".exe")
        command = [compiler, "/nologo", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                   *[f"/I{p}" for p in includes], *map(str, sources), f"/Fe:{executable}"]
    else:
        command = [compiler, "-std=c11", "-Wall", "-Wextra", *[f"-I{p}" for p in includes],
                   *map(str, sources), "-o", str(executable)]
    subprocess.run(command, cwd=work, check=True)
    subprocess.run([str(executable)], cwd=work, check=True)
