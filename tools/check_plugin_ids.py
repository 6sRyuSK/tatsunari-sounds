#!/usr/bin/env python3
"""Verify stable/dev plugin identifier uniqueness (plan §1.6).

Enumerates active plugins' CLAP IDs and AU subtypes from shell sources and
checks that the planned .dev / AU-suffix-D variants do not collide with any
stable identity. VST3 UIDs are derived by clap-wrapper at build time from the
bundle identifier; this gate covers the source-of-truth identifiers we control.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLUGINS = ROOT / "plugins"

CLAP_ID_RE = re.compile(r'"((?:jp|com)\.tatsunari-sounds\.[a-z0-9-]+)"')
AU_SUBTYPE_RE = re.compile(r"AUV2_SUBTYPE_CODE\s+(\w+)")


def collect() -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for slug_dir in sorted(p for p in PLUGINS.iterdir() if p.is_dir()):
        clap_entry = slug_dir / "shell" / "ClapEntry.cpp"
        cmake = slug_dir / "shell" / "CMakeLists.txt"
        if not clap_entry.is_file():
            continue
        text = clap_entry.read_text(encoding="utf-8")
        ids = CLAP_ID_RE.findall(text)
        if not ids:
            raise SystemExit(f"no CLAP id in {clap_entry}")
        clap_id = ids[0]
        subtype = ""
        if cmake.is_file():
            m = AU_SUBTYPE_RE.search(cmake.read_text(encoding="utf-8"))
            if m:
                subtype = m.group(1)
        rows.append({
            "slug": slug_dir.name,
            "variant": "stable",
            "clap_id": clap_id,
            "au_subtype": subtype,
            "au_manufacturer": "Ttsn",
        })
        # Planned dev variant (not yet built) — must still be unique.
        if clap_id.endswith(".dev"):
            raise SystemExit(f"stable id already ends with .dev: {clap_id}")
        dev_subtype = ""
        if subtype:
            if len(subtype) != 4:
                raise SystemExit(f"AU subtype must be 4 chars, got {subtype!r} for {slug_dir.name}")
            dev_subtype = subtype[:3] + "D"
        rows.append({
            "slug": slug_dir.name,
            "variant": "dev",
            "clap_id": clap_id + ".dev",
            "au_subtype": dev_subtype,
            "au_manufacturer": "Ttsn",
        })
    return rows


def check_unique(rows: list[dict[str, str]]) -> list[str]:
    errors: list[str] = []
    for field in ("clap_id", "au_subtype"):
        seen: dict[str, str] = {}
        for r in rows:
            val = r[field]
            if not val:
                continue
            key = f"{field}={val}"
            label = f"{r['slug']}/{r['variant']}"
            if key in seen:
                errors.append(f"collision on {key}: {seen[key]} vs {label}")
            else:
                seen[key] = label
    # Prefix convention: shipping IDs use jp. (plan §1.6 said com. — superseded by code).
    for r in rows:
        if r["variant"] == "stable" and not r["clap_id"].startswith("jp.tatsunari-sounds."):
            errors.append(f"unexpected clap id prefix: {r['clap_id']}")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="exit non-zero on collisions")
    ap.add_argument("--emit", action="store_true", help="print rows as TSV")
    args = ap.parse_args()
    rows = collect()
    if args.emit:
        for r in rows:
            print("\t".join([r["slug"], r["variant"], r["clap_id"], r["au_subtype"], r["au_manufacturer"]]))
    errors = check_unique(rows)
    if errors:
        for e in errors:
            print(e, file=sys.stderr)
        if args.check:
            return 1
    elif args.check:
        print(f"ok: {len(rows)} identities unique")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
