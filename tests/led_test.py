# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

"""Run actual LED policy, WS2812 rendering and settings handlers without hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get("CC") or shutil.which("cl") or shutil.which("cc")
if not compiler:
    raise SystemExit("A host C compiler is required. Run in a Visual Studio developer shell or set CC.")
suite = root / "external" / "ota-suite" / "components"
with tempfile.TemporaryDirectory(prefix="ebctester-leds-") as directory:
    work = Path(directory)
    for name in ("esp_log.h", "esp_timer.h", "freertos/FreeRTOS.h", "freertos/task.h",
                 "led_strip.h", "esp_http_server.h", "nvs.h"):
        stub = work / name
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text("", encoding="ascii")
    (work / "esp_err.h").write_text(
        "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n"
        "#define ESP_ERR_INVALID_ARG 1\n#define ESP_ERR_INVALID_STATE 2\n#define ESP_ERR_NO_MEM 3\n",
        encoding="ascii")
    includes = [work, root / "main", suite / "status_led" / "include", suite / "ota_manager" / "include"]
    msvc = Path(compiler).name.lower() in ("cl", "cl.exe")
    for name, sources in (
        ("policy", [root / "tests" / "led_policy_test.c", root / "main" / "led_policy.c"]),
        ("driver", [root / "tests" / "status_led_test.c"]),
    ):
        executable = work / (name + (".exe" if msvc else ""))
        if msvc:
            command = [compiler, "/nologo", "/std:c11", *[f"/I{p}" for p in includes],
                       *map(str, sources), f"/Fe:{executable}"]
        else:
            command = [compiler, "-std=c11", "-Wall", "-Wextra", *[f"-I{p}" for p in includes],
                       *map(str, sources), "-o", str(executable)]
        subprocess.run(command, cwd=work, check=True)
        subprocess.run([str(executable)], cwd=work, check=True)

    cjson = root / "managed_components" / "espressif__cjson" / "cJSON"
    includes += [cjson, root / "components" / "ebc_config" / "include",
                 root / "components" / "ebc_link" / "include",
                 root / "components" / "test_control" / "include",
                 suite / "web_interface" / "include"]
    sources = [root / "tests" / "led_settings_test.cpp"]
    executable = work / ("settings.exe" if msvc else "settings")
    if msvc:
        subprocess.run([compiler, "/nologo", "/c", str(cjson / "cJSON.c"),
                        f"/Fo:{work / 'cjson.obj'}"], cwd=work, check=True)
        subprocess.run([compiler, "/nologo", "/std:c11", "/c", str(root / "main" / "led_policy.c"),
                        f"/I{root / 'main'}", f"/Fo:{work / 'policy.obj'}"], cwd=work, check=True)
        command = [compiler, "/nologo", "/std:c++20", "/EHsc", *[f"/I{p}" for p in includes],
                   *map(str, sources), str(work / "cjson.obj"), str(work / "policy.obj"), f"/Fe:{executable}"]
    else:
        subprocess.run([compiler, "-c", str(cjson / "cJSON.c"), "-o", str(work / "cjson.o")],
                       cwd=work, check=True)
        subprocess.run([compiler, "-c", str(root / "main" / "led_policy.c"), "-o", str(work / "policy.o")],
                       cwd=work, check=True)
        cpp = os.environ.get("CXX") or shutil.which("c++")
        if not cpp:
            raise SystemExit("A host C++ compiler is required.")
        command = [cpp, "-std=c++20", *[f"-I{p}" for p in includes], *map(str, sources),
                   str(work / "cjson.o"), str(work / "policy.o"), "-o", str(executable)]
    subprocess.run(command, cwd=work, check=True)
    subprocess.run([str(executable)], cwd=work, check=True)
