#!/usr/bin/env python3
"""
tools/gen_catalog.py — generate the README catalog from machine-readable sources.

Sources of truth (one item lives in exactly ONE of these):
  plugins/<slug>/plugin.toml   real plugins   (status: shipped | in-progress)
  roadmap.toml                 planned plugins not yet started

Rewrites only the block between:
  <!-- BEGIN:CATALOG -->  ...  <!-- END:CATALOG -->

Usage:
  python tools/gen_catalog.py            # rewrite README.md in place
  python tools/gen_catalog.py --check    # exit 1 if README is out of date (CI)

Requires Python 3.11+ (stdlib tomllib).
"""
from __future__ import annotations

import glob
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
README = ROOT / "README.md"
BEGIN = "<!-- BEGIN:CATALOG -->"
END = "<!-- END:CATALOG -->"


def load_plugins():
    shipped, in_progress = [], []
    for path in sorted(glob.glob(str(ROOT / "plugins" / "*" / "plugin.toml"))):
        raw = tomllib.loads(Path(path).read_text(encoding="utf-8"))
        p = raw.get("plugin", raw)
        entry = {
            "name": p.get("name", "?"),
            "slug": p.get("slug", Path(path).parent.name),
            "category": p.get("category", "?"),
            "version": p.get("version", ""),
            "formats": ", ".join(p.get("formats", [])),
            "reference": p.get("reference", "—"),
            "status": p.get("status", "shipped"),
        }
        (shipped if entry["status"] == "shipped" else in_progress).append(entry)
    return shipped, in_progress


def load_roadmap():
    f = ROOT / "roadmap.toml"
    if not f.exists():
        return []
    raw = tomllib.loads(f.read_text(encoding="utf-8"))
    items = raw.get("plugin", [])
    return sorted(items, key=lambda x: (x.get("category", ""), x.get("name", "")))


def md_table(rows, cols):
    if not rows:
        return "_None yet._"
    head = "| " + " | ".join(label for _, label in cols) + " |"
    sep = "| " + " | ".join("---" for _ in cols) + " |"
    body = [
        "| " + " | ".join(str(r.get(key, "—") or "—") for key, _ in cols) + " |"
        for r in rows
    ]
    return "\n".join([head, sep, *body])


def render() -> str:
    shipped, in_progress = load_plugins()
    planned = load_roadmap()
    parts = []
    parts.append(f"### Shipped ({len(shipped)})\n")
    parts.append(md_table(shipped, [
        ("name", "Plugin"), ("category", "Category"), ("version", "Version"),
        ("formats", "Formats"), ("reference", "Reference"),
    ]))
    parts.append(f"\n\n### In progress ({len(in_progress)})\n")
    parts.append(md_table(in_progress, [
        ("name", "Plugin"), ("category", "Category"), ("reference", "Reference"),
    ]))
    parts.append(f"\n\n### Planned ({len(planned)})\n")
    parts.append(md_table(planned, [
        ("name", "Plugin"), ("category", "Category"), ("reference", "Reference"),
    ]))
    return "\n".join(parts)


def splice(readme_text: str, block: str) -> str:
    if BEGIN not in readme_text or END not in readme_text:
        raise SystemExit(
            f"README.md is missing the markers {BEGIN} / {END}."
        )
    pre = readme_text.split(BEGIN)[0]
    post = readme_text.split(END, 1)[1]
    return f"{pre}{BEGIN}\n\n{block}\n\n{END}{post}"


def main() -> None:
    check = "--check" in sys.argv
    current = README.read_text(encoding="utf-8")
    updated = splice(current, render())
    if check:
        if current != updated:
            print("README catalog is out of date. Run: python tools/gen_catalog.py")
            sys.exit(1)
        print("README catalog is up to date.")
        return
    if current != updated:
        # Force LF so regenerating on Windows doesn't rewrite every line as CRLF.
        README.write_text(updated, encoding="utf-8", newline="\n")
        print("README catalog updated.")
    else:
        print("README catalog already up to date.")


if __name__ == "__main__":
    main()
