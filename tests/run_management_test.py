# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

"""Compile actual run-management operations against the configured SQLite source."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sqlite = root / "build" / "sqlite-amalgamation-3530400"
if not (sqlite / "sqlite3.c").is_file():
    raise SystemExit("Configure the firmware build first to obtain its SQLite source.")
compiler = os.environ.get("CC") or shutil.which("cl") or shutil.which("cc")
if not compiler:
    raise SystemExit("Run in a host C/C++ developer shell or set CC and CXX.")
with tempfile.TemporaryDirectory(prefix="ebctester-runs-") as temporary:
    work = Path(temporary)
    includes = [sqlite, root / "components" / "battery_dashboard"]
    sources = [root / "tests" / "run_management_test.cpp",
               root / "components" / "battery_dashboard" / "run_management.cpp"]
    if Path(compiler).name.lower() in ("cl", "cl.exe"):
        obj = work / "sqlite.obj"
        subprocess.run([compiler, "/nologo", "/c", "/O2", str(sqlite / "sqlite3.c"),
                        f"/Fo:{obj}"], cwd=work, check=True)
        executable = work / "runs-test.exe"
        command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4",
                   *[f"/I{p}" for p in includes], *map(str, sources), str(obj), f"/Fe:{executable}"]
    else:
        obj = work / "sqlite.o"
        subprocess.run([compiler, "-c", "-O2", str(sqlite / "sqlite3.c"), "-o", str(obj)],
                       cwd=work, check=True)
        cpp = os.environ.get("CXX") or shutil.which("c++")
        if not cpp:
            raise SystemExit("A host C++ compiler is required.")
        executable = work / "runs-test"
        command = [cpp, "-std=c++17", "-Wall", "-Wextra", *[f"-I{p}" for p in includes],
                   *map(str, sources), str(obj), "-lpthread", "-ldl", "-o", str(executable)]
    subprocess.run(command, cwd=work, check=True)
    subprocess.run([str(executable), str(root)], cwd=work, check=True)
