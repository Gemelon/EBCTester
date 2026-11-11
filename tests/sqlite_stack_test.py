# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

"""Check SQLite scratch allocation policy and exercise the real allocator."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
configuration = (root / "components" / "sqlite3" / "CMakeLists.txt").read_text(encoding="utf-8")
definitions = re.search(
    r"target_compile_definitions\(\$\{COMPONENT_LIB\} PRIVATE(.*?)\n\)",
    configuration, re.S,
).group(1).split()
assert not any(item.split("=")[0] == "SQLITE_USE_ALLOCA" for item in definitions), (
    "SQLITE_USE_ALLOCA must be undefined, not set to 0"
)
sqlite = root / "build" / "sqlite-amalgamation-3530400"
if not (sqlite / "sqlite3.c").is_file():
    raise SystemExit("Configure the firmware build first to obtain its SQLite source.")
compiler = os.environ.get("CC") or shutil.which("cl") or shutil.which("cc")
if not compiler:
    raise SystemExit("Run in a host C developer shell or set CC.")

# The host uses its native VFS and keeps test assertions enabled.
definitions = [item for item in definitions if item not in ("NDEBUG=1", "SQLITE_OS_OTHER=1")]
with tempfile.TemporaryDirectory(prefix="ebctester-sqlite-stack-") as temporary:
    work = Path(temporary)
    source = root / "tests" / "sqlite_stack_test.c"
    if Path(compiler).name.lower() in ("cl", "cl.exe"):
        executable = work / "sqlite-stack-test.exe"
        command = [compiler, "/nologo", "/O2", f"/I{sqlite}",
                   *[f"/D{item}" for item in definitions],
                   str(source), f"/Fe:{executable}"]
    else:
        executable = work / "sqlite-stack-test"
        command = [compiler, "-O2", f"-I{sqlite}",
                   *[f"-D{item}" for item in definitions],
                   str(source), "-lm", "-o", str(executable)]
    subprocess.run(command, cwd=work, check=True)
    subprocess.run([str(executable)], cwd=work, check=True)
