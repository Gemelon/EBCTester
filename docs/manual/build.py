# SPDX-FileCopyrightText: 2026 Thomas Stoll
# SPDX-License-Identifier: Apache-2.0
"""Builds the Factory and Maker user manuals (English) from one HTML source.

Usage: python docs/manual/build.py
Requires: WeasyPrint (pip install weasyprint). Screenshots are read from
docs/images/manual/; regenerate them with tools/shots.py (simulated data only).

The source marks edition-specific content with <only factory>...</only> or
<only maker>...</only>. The table of contents is generated from h2.chapter and
h3 headings.
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DOCS = HERE.parent
EDITIONS = {
    "factory": ("EBCTester_Manual_Factory_EN.pdf", "User Manual – Factory Edition"),
    "maker": ("EBCTester_Manual_Maker_EN.pdf", "User Manual – Maker Edition"),
}


def select_edition(source: str, edition: str) -> str:
    pattern = re.compile(r"<only (factory|maker)>(.*?)</only>", re.S)
    previous = None
    while previous != source:
        previous = source
        source = pattern.sub(lambda m: m.group(2) if m.group(1) == edition else "", source)
    return source


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", re.sub(r"<[^>]+>", "", text).lower()).strip("-")


def number_and_toc(html: str) -> str:
    """Numbers chapters/sections, assigns ids and renders the table of contents."""
    toc = []
    extra = {}
    chapter = 0
    section = 0
    used = set()

    def unique(base):
        candidate, n = base, 2
        while candidate in used:
            candidate, n = f"{base}-{n}", n + 1
        used.add(candidate)
        return candidate

    def heading(match):
        nonlocal chapter, section
        tag, attrs, body = match.group(1), match.group(2) or "", match.group(3)
        if 'class="part-title"' in attrs:
            anchor = unique("part-" + slug(body))
            toc.append(("part", anchor, body))
            return f'<h1 id="{anchor}"{attrs}>{body}</h1>'
        if tag == "h2" and 'chapter' in attrs:
            unnumbered = "appendix" in attrs
            if not unnumbered:
                chapter += 1
                section = 0
                label = f'<span class="num">{chapter}</span> '
            else:
                label = ""
            anchor = unique("ch-" + slug(body))
            toc.append(("ch", anchor, (f"{chapter}&nbsp;&nbsp;" if not unnumbered else "") + body))
            return f'<h2 id="{anchor}"{attrs}>{label}{body}</h2>'
        if tag == "h4":
            anchor = unique("h-" + slug(body))
            extra.setdefault(re.sub(r"<[^>]+>", "", body).strip().lower(), (anchor, f"{chapter}.{section}"))
            attrs = re.sub(r'\sid="[^"]*"', "", attrs)
            return f'<h4 id="{anchor}"{attrs}>{body}</h4>'
        if tag == "h3" and "nonum" not in attrs:
            section += 1
            anchor = unique("s-" + slug(body))
            number = f"{chapter}.{section}"
            toc.append(("sec", anchor, f"{number}&nbsp;&nbsp;{body}"))
            return f'<h3 id="{anchor}"{attrs}><span class="num">{number}</span>{body}</h3>'
        return match.group(0)

    html = re.sub(r"<(h1|h2|h3|h4)((?:\s[^>]*)?)>(.*?)</\1>", heading, html, flags=re.S)
    items = "\n".join(f'<li class="{kind}"><a href="#{anchor}">{text}</a></li>' for kind, anchor, text in toc)
    html = html.replace("<!--TOC-->", f"<ul>{items}</ul>")
    # Resolve cross references written as <a href="#ref:Some heading">.
    lookup = {}
    for kind, anchor, text in toc:
        key = re.sub(r"<[^>]+>|^[0-9.]+(&nbsp;)+", "", text).replace("&nbsp;", " ").strip().lower()
        number = re.match(r"([0-9.]+)&nbsp;", text)
        if number:
            label = ("chapter " if kind == "ch" else "section ") + number.group(1)
        else:
            label = re.sub(r"<[^>]+>", "", text).replace("&nbsp;", " ")
        lookup.setdefault(key, (anchor, label))
    for key, (anchor, number) in extra.items():
        lookup.setdefault(key, (anchor, f"section {number}"))

    def xref(match):
        key = match.group(1).strip().lower()
        if key not in lookup:
            raise SystemExit(f"Unresolved cross reference: {match.group(1)}")
        anchor, label = lookup[key]
        text = match.group(2)
        # Written link text is kept and followed by the location, e.g. "Device reset (section 4.5)".
        if text and text.lower() != key and not text.lower().startswith("appendix"):
            return f'<a href="#{anchor}">{text}</a>'
        if text and text.lower().startswith("appendix"):
            return f'<a href="#{anchor}">{text}</a>'
        return f'<a href="#{anchor}">{text} ({label})</a>' if text else f'<a href="#{anchor}">{label}</a>'

    return re.sub(r'<a href="#ref:([^"]+)">(.*?)</a>', xref, html)


def build(edition: str, out_dir: Path, html_only: bool = False) -> Path:
    from weasyprint import HTML
    filename, title = EDITIONS[edition]
    source = (HERE / "manual.src.html").read_text(encoding="utf-8")
    source = re.sub(r'<!--#include "([^"]+)"-->',
                    lambda m: (HERE / "src" / m.group(1)).read_text(encoding="utf-8"), source)
    html = select_edition(source, edition).replace("{{TITLE}}", title)
    html = number_and_toc(html)
    rendered = HERE / f".build-{edition}.html"
    rendered.write_text(html, encoding="utf-8")
    target = out_dir / filename
    if not html_only:
        HTML(filename=str(rendered), base_url=str(HERE)).write_pdf(str(target), optimize_images=True, dpi=200)
        rendered.unlink()
    return target


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else DOCS
    for name in EDITIONS:
        print(build(name, out))
