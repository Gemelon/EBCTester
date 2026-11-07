<!-- SPDX-FileCopyrightText: 2026 Thomas Stoll
     SPDX-License-Identifier: Apache-2.0 -->

# User manual sources

Both English manuals are built from one HTML source:

| Edition | Output | Content |
| --- | --- | --- |
| Factory | `docs/EBCTester_Manual_Factory_EN.pdf` | Normal user interface, compact technical reference |
| Maker | `docs/EBCTester_Manual_Maker_EN.pdf` | Maker setup and Maker Settings, detailed technical reference |

Translations follow once the firmware has reached a stable release, or on request.

## Files

- `manual.src.html` – document shell (cover, contents) that includes the chapters from `src/`.
- `src/*.html` – chapters. Edition-specific text is wrapped in `<only factory>…</only>` or
  `<only maker>…</only>`. Chapters are `<h2 class="chapter">`, sections `<h3>`; numbering and the
  table of contents are generated. Cross references are written as `<a href="#ref:Heading text">`.
- `manual.css` – print layout (A4, WeasyPrint).
- `build.py` – builds both PDFs into `docs/`.
- `tools/fixture.py`, `tools/shots.py` – recreate the screenshots in `docs/images/manual/` from the
  real web assets with simulated data. They never access hardware.

## Build

```powershell
pip install weasyprint playwright pillow
python -m playwright install chromium
python docs\manual\tools\shots.py      # only when the web UI changed
python docs\manual\build.py
```
