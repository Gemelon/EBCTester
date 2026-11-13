# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

"""Test real discovery startup with mocked network status and mDNS APIs."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get("CC") or shutil.which("cl") or shutil.which("cc")
if not compiler:
    raise SystemExit("A host C compiler is required.")

with tempfile.TemporaryDirectory(prefix="ebctester-discovery-") as temporary:
    work = Path(temporary)
    for name in ("esp_log.h", "mdns.h"):
        (work / name).write_text("", encoding="ascii")
    (work / "esp_err.h").write_text(
        "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n"
        "#define ESP_ERR_INVALID_ARG 1\n#define ESP_ERR_NO_MEM 2\n", encoding="ascii")
    includes = [work, root / "main",
                root / "external" / "ota-suite" / "components" / "network_manager" / "include"]
    source = root / "tests" / "local_discovery_test.c"
    executable = work / "discovery-test"
    if Path(compiler).name.lower() in ("cl", "cl.exe"):
        executable = executable.with_suffix(".exe")
        command = [compiler, "/nologo", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                   *[f"/I{path}" for path in includes], str(source), f"/Fe:{executable}"]
    else:
        command = [compiler, "-std=c11", "-Wall", "-Wextra",
                   *[f"-I{path}" for path in includes], str(source), "-o", str(executable)]
    subprocess.run(command, cwd=work, check=True)
    subprocess.run([str(executable)], cwd=work, check=True)
