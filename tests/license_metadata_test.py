# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0

import fnmatch
import os
from pathlib import Path
import re
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
PACKAGES = (
    ROOT,
    Path(os.environ.get("OTA_SUITE_DIR", ROOT / "external" / "ota-suite")),
    Path(os.environ.get("SD_CARD_MANAGER_DIR", ROOT / "external" / "sd-card-manager")),
)
UPSTREAM = {
    "components/battery_dashboard/assets/chart.umd.js",
    "components/battery_dashboard/assets/CHARTJS-LICENSE.md",
    "components/battery_dashboard/assets/KURKLE-COLOR-LICENSE.md",
    "components/web_interface/assets/bootstrap.min.css",
    "components/web_interface/assets/BOOTSTRAP-LICENSE",
}


def original_files(root):
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "--cached", "--others",
         "--exclude-standard", "-z"],
        check=True, capture_output=True, text=True,
    )
    for name in sorted(set(result.stdout.split("\0"))):
        if not name or name in UPSTREAM or name.startswith(("external/", "third_party/", ".reuse/")):
            continue
        path = root / name
        if path.is_file() and name not in ("LICENSE", "NOTICE"):
            yield name, path


class LicenseMetadataTests(unittest.TestCase):
    def test_same_apache_license_and_author_in_all_packages(self):
        license_text = (ROOT / "LICENSE").read_bytes()
        self.assertIn(b"Apache License", license_text)
        self.assertIn(b"Version 2.0, January 2004", license_text)
        for root in PACKAGES:
            with self.subTest(package=root.name):
                self.assertEqual((root / "LICENSE").read_bytes(), license_text)
                self.assertIn("Copyright 2026 Thomas Stoll", (root / "NOTICE").read_text())
                self.assertIn("Apache-2.0", (root / ".reuse" / "dep5").read_text())

    def test_original_files_have_headers_or_format_safe_attribution(self):
        for root in PACKAGES:
            dep5 = (root / ".reuse" / "dep5").read_text()
            self.assertIn("Copyright: 2026 Thomas Stoll", dep5)
            files = dep5.split("Files:", 1)[1].split("Copyright:", 1)[0].split()
            for name, path in original_files(root):
                with self.subTest(package=root.name, file=name):
                    text = path.read_text(encoding="utf-8")
                    if any(fnmatch.fnmatchcase(name, pattern) for pattern in files):
                        continue
                    self.assertIn("SPDX-FileCopyrightText: 2026 Thomas Stoll", text[:500])
                    self.assertIn("SPDX-License-Identifier: Apache-2.0", text[:500])

    def test_documentation_links_and_final_third_party_section(self):
        for root in PACKAGES:
            for name in ("README.md", "THIRD_PARTY_NOTICES.md"):
                text = (root / name).read_text(encoding="utf-8")
                for target in re.findall(r"\]\(([^)]+)\)", text):
                    if re.match(r"[a-zA-Z][a-zA-Z0-9+.-]*:", target) or target.startswith("#"):
                        continue
                    with self.subTest(package=root.name, document=name, target=target):
                        relative = target.split("#", 1)[0]
                        resolved = root / relative
                        if root == ROOT:
                            for prefix, package in (
                                ("external/ota-suite/", PACKAGES[1]),
                                ("external/sd-card-manager/", PACKAGES[2]),
                            ):
                                if relative.startswith(prefix):
                                    resolved = package / relative[len(prefix):]
                        self.assertTrue(resolved.exists())
            readme = (root / "README.md").read_text(encoding="utf-8")
            headings = re.findall(r"^## (.+)$", readme, re.M)
            self.assertEqual(headings[-1], "Drittanbieter und ihre Lizenzen")
            self.assertIn("Thomas Stoll", readme)
            self.assertNotIn("No license has yet", readme)

    def test_vendor_assets_keep_their_own_licenses(self):
        chart = ROOT / "components" / "battery_dashboard" / "assets"
        bootstrap = PACKAGES[1] / "components" / "web_interface" / "assets"
        for path, author in (
            (chart / "chart.umd.js", "Chart.js Contributors"),
            (chart / "CHARTJS-LICENSE.md", "Chart.js Contributors"),
            (chart / "KURKLE-COLOR-LICENSE.md", "Jukka Kurkela"),
            (bootstrap / "bootstrap.min.css", "The Bootstrap Authors"),
            (bootstrap / "BOOTSTRAP-LICENSE", "The Bootstrap Authors"),
        ):
            with self.subTest(file=path.name):
                text = path.read_text(encoding="utf-8")
                self.assertIn(author, text[:1000])
                self.assertIn("MIT", text[:1000])
                self.assertNotIn("SPDX-FileCopyrightText: 2026 Thomas Stoll", text)


if __name__ == "__main__":
    unittest.main()
