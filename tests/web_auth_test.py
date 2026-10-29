# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

"""Compile and run the real authentication code with mocked ESP services."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
component = root / "external" / "ota-suite" / "components" / "web_interface"
cjson = root / "managed_components" / "espressif__cjson" / "cJSON"
compiler = os.environ.get("CC") or shutil.which("cl") or shutil.which("cc")
if not compiler:
    raise SystemExit("A host C compiler is required. Run in a Visual Studio developer shell or set CC.")
if not (cjson / "cJSON.c").is_file():
    raise SystemExit("Restore ESP-IDF managed dependencies before running authentication tests.")

with tempfile.TemporaryDirectory(prefix="ebctester-auth-") as temporary:
    work = Path(temporary)
    for name in ("web_interface.h", "esp_log.h", "esp_random.h", "esp_timer.h",
                 "network_manager.h", "nvs.h", "sdkconfig.h"):
        (work / name).write_text("", encoding="ascii")
    for require_password in (False, True):
        sources = [str(root / "tests" / "web_auth_test.c"),
                   str(component / "web_session.c"), str(cjson / "cJSON.c")]
        executable = work / ("auth-password" if require_password else "auth-session")
        if Path(compiler).name.lower() in ("cl", "cl.exe"):
            executable = executable.with_suffix(".exe")
            command = [compiler, "/nologo", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                       f"/I{work}", f"/I{cjson}", *sources, f"/Fe:{executable}"]
            if require_password:
                command.insert(1, "/DCONFIG_OTA_SUITE_WEB_SETTINGS_REQUIRE_PASSWORD=1")
        else:
            command = [compiler, "-std=c11", "-Wall", "-Wextra", f"-I{work}", f"-I{cjson}",
                       *sources, "-o", str(executable)]
            if require_password:
                command.insert(1, "-DCONFIG_OTA_SUITE_WEB_SETTINGS_REQUIRE_PASSWORD=1")
        subprocess.run(command, cwd=work, check=True)
        subprocess.run([str(executable)], cwd=work, check=True)
