# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0
"""Check actual ESP32-S3 configuration stack frames after an ESP-IDF build."""

from pathlib import Path
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
objdump = shutil.which("xtensa-esp32s3-elf-objdump")
if objdump is None:
    raise SystemExit("Run this test in the ESP-IDF ESP32-S3 environment.")
obj = root / "build" / "esp-idf" / "ebc_config" / "CMakeFiles" / "__idf_ebc_config.dir" / "ebc_config.c.obj"
if not obj.is_file():
    raise SystemExit("Build the firmware before checking configuration stack frames.")

disassembly = subprocess.check_output([objdump, "-d", str(obj)], text=True)
frames = {}
for name, instruction in re.findall(r"<([^>]+)>:\n([^\n]*)", disassembly):
    match = re.search(r"\bentry\s+a1,\s*(0x[0-9a-f]+|\d+)", instruction)
    if match:
        frames[name] = int(match.group(1), 0)

for required in ("ebc_config_init", "persist", "ebc_config_save_with_active"):
    assert required in frames, f"Missing stack-frame evidence for {required}"
for name, size in frames.items():
    assert size <= 256, f"{name} uses {size} stack bytes; configuration buffers must not be stack-local"
    print(f"{name}: {size} stack bytes")
print("All compiled configuration stack frames are within the 256-byte limit.")
