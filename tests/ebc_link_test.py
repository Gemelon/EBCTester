# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

"""Run actual UART link, model and controller code with mocked services, never hardware."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get("CC") or shutil.which("cl") or shutil.which("cc")
if not compiler:
    raise SystemExit("A host C compiler is required. Run in a Visual Studio developer shell or set CC.")

with tempfile.TemporaryDirectory(prefix="ebctester-link-") as temporary:
    work = Path(temporary)
    for name in ("driver/uart.h", "esp_log.h", "esp_system.h", "esp_timer.h",
                 "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h", "sdkconfig.h"):
        stub = work / name
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text("", encoding="ascii")
    (work / "esp_err.h").write_text(
        "#pragma once\n"
        "typedef int esp_err_t;\n"
        "#define ESP_OK 0\n"
        "#define ESP_ERR_INVALID_ARG 1\n"
        "#define ESP_ERR_INVALID_STATE 2\n"
        "#define ESP_ERR_NO_MEM 3\n",
        encoding="ascii",
    )
    includes = [work, root / "components" / "ebc_link" / "include"]
    sources = [root / "tests" / "ebc_link_test.c", root / "components" / "ebc_link" / "ebc_model.c"]
    executable = work / "ebc-link-test"
    if Path(compiler).name.lower() in ("cl", "cl.exe"):
        executable = executable.with_suffix(".exe")
        command = [compiler, "/nologo", "/std:c11", "/W4", "/D_CRT_SECURE_NO_WARNINGS",
                   *[f"/I{include}" for include in includes], *map(str, sources), f"/Fe:{executable}"]
    else:
        command = [compiler, "-std=c11", "-Wall", "-Wextra",
                   *[f"-I{include}" for include in includes], *map(str, sources), "-o", str(executable)]
    subprocess.run(command, cwd=work, check=True)
    subprocess.run([str(executable)], cwd=work, check=True)

    for name in ("nvs.h",):
        (work / name).write_text("", encoding="ascii")
    cjson = root / "managed_components" / "espressif__cjson" / "cJSON"
    includes += [root / "components" / component / "include"
                 for component in ("test_control", "test_store", "resistance_test")]
    includes.append(cjson)
    test_source = root / "tests" / "test_control_test.cpp"
    model_source = root / "components" / "ebc_link" / "ebc_model.c"
    executable = work / "tester-control-test"
    if Path(compiler).name.lower() in ("cl", "cl.exe"):
        subprocess.run([compiler, "/nologo", "/std:c11", "/c", str(cjson / "cJSON.c"),
                        f"/Fo:{work / 'cjson.obj'}"], cwd=work, check=True)
        executable = executable.with_suffix(".exe")
        command = [compiler, "/nologo", "/std:c++20", "/EHsc", "/D_CRT_SECURE_NO_WARNINGS",
                   *[f"/I{include}" for include in includes], str(test_source), str(model_source),
                   str(work / "cjson.obj"), f"/Fe:{executable}"]
    else:
        subprocess.run([compiler, "-c", str(cjson / "cJSON.c"), "-o", str(work / "cjson.o")],
                       cwd=work, check=True)
        cpp_compiler = os.environ.get("CXX") or shutil.which("c++")
        if not cpp_compiler:
            raise SystemExit("A host C++ compiler is required for controller tests.")
        command = [cpp_compiler, "-std=c++20", "-pthread",
                   *[f"-I{include}" for include in includes], str(test_source), str(model_source),
                   str(work / "cjson.o"), "-o", str(executable)]
    subprocess.run(command, cwd=work, check=True)
    subprocess.run([str(executable)], cwd=work, check=True)
