# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

"""Run real Maker HTTP handlers with mocked transport and device services."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get("CXX") or shutil.which("cl") or shutil.which("c++")
if not compiler:
    raise SystemExit("A host C++ compiler is required.")
cjson = root / "managed_components" / "espressif__cjson" / "cJSON"
with tempfile.TemporaryDirectory(prefix="ebctester-maker-") as temporary:
    work = Path(temporary)
    for name in ("esp_log.h", "esp_system.h", "esp_http_server.h",
                 "freertos/FreeRTOS.h", "freertos/task.h", "ota_manager.h"):
        stub = work / name
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text("", encoding="ascii")
    (work / "esp_err.h").write_text(
        "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n"
        "#define ESP_ERR_INVALID_ARG 1\n#define ESP_ERR_INVALID_STATE 2\n", encoding="ascii")
    includes = [work, cjson, root / "components" / "ebc_config" / "include",
                root / "components" / "test_control" / "include",
                root / "external" / "ota-suite" / "components" / "web_interface" / "include"]
    source = root / "tests" / "ebc_config_web_test.cpp"
    executable = work / "maker-test"
    if Path(compiler).name.lower() in ("cl", "cl.exe"):
        subprocess.run([compiler, "/nologo", "/c", str(cjson / "cJSON.c"),
                        f"/Fo:{work / 'cjson.obj'}"], cwd=work, check=True)
        executable = executable.with_suffix(".exe")
        command = [compiler, "/nologo", "/std:c++20", "/EHsc", "/D_CRT_SECURE_NO_WARNINGS",
                   *[f"/I{p}" for p in includes], str(source), str(work / "cjson.obj"), f"/Fe:{executable}"]
    else:
        subprocess.run(["cc", "-c", str(cjson / "cJSON.c"), "-o", str(work / "cjson.o")], cwd=work, check=True)
        command = [compiler, "-std=c++20", *[f"-I{p}" for p in includes],
                   str(source), str(work / "cjson.o"), "-o", str(executable)]
    subprocess.run(command, cwd=work, check=True)
    subprocess.run([str(executable)], cwd=work, check=True)
