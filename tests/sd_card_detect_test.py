# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

"""Run the real SD detection and debounce functions with mocked GPIO input."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get("CC") or shutil.which("cl") or shutil.which("cc")
if not compiler:
    raise SystemExit("A host C compiler is required.")
source = (root / "external" / "sd-card-manager" / "components" /
          "sd_card_manager" / "sd_card_manager.c").read_text(encoding="utf-8")
functions = source[source.index("static bool card_detected(void)"):
                   source.index("static void mark_card_unavailable")]
harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define ESP_LOGI(...) ((void)0)
#define CARD_DETECT_STABLE_SAMPLES 4
static struct { int card_detect_gpio; bool card_detect_active_high; } runtime;
static bool card_present, card_detect_candidate;
static unsigned card_detect_samples, reads;
static int level;
static int gpio_get_level(int pin) { assert(pin == 7); ++reads; return level; }
"""
harness += functions
harness += r"""
int main(void)
{
    for (unsigned active_high = 0; active_high <= 1; ++active_high) {
        runtime.card_detect_gpio = 7;
        runtime.card_detect_active_high = active_high;
        level = !active_high;
        assert(!card_detected());
        card_present = card_detect_candidate = false;
        card_detect_samples = 0;
        level = active_high;
        assert(card_detected());
        for (unsigned i = 0; i < 3; ++i) assert(!update_card_detect_locked());
        level = !active_high;
        assert(!update_card_detect_locked());
        level = active_high;
        for (unsigned i = 0; i < 3; ++i) assert(!update_card_detect_locked());
        assert(update_card_detect_locked());
        level = !active_high;
        for (unsigned i = 0; i < 3; ++i) assert(update_card_detect_locked());
        assert(!update_card_detect_locked());
        runtime.card_detect_gpio = -1;
        unsigned previous_reads = reads;
        assert(card_detected());
        assert(reads == previous_reads);
    }
    puts("SD detection passed: both polarities, insertion/removal debounce, bouncing and no GPIO.");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="ebctester-card-detect-") as temporary:
    work = Path(temporary)
    test_source = work / "card_detect.c"
    test_source.write_text(harness, encoding="ascii")
    executable = work / "card_detect"
    if Path(compiler).name.lower() in ("cl", "cl.exe"):
        executable = executable.with_suffix(".exe")
        command = [compiler, "/nologo", "/std:c11", str(test_source), f"/Fe:{executable}"]
    else:
        command = [compiler, "-std=c11", "-Wall", "-Wextra", str(test_source), "-o", str(executable)]
    subprocess.run(command, cwd=work, check=True)
    subprocess.run([str(executable)], cwd=work, check=True)
