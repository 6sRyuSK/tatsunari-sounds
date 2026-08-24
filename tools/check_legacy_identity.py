#!/usr/bin/env python3
"""Residue check for the tn-* product-identity migration (plan §11.6.7).

`docs/plans/update-notification-and-distribution/11-product-identity-migration.md`
re-identifies the three shipping plugins as NEW products (`tn-resonance-suppressor`
/ `tn-equalizer` / `tn-vocal-tuner`). The dangerous failure mode is not a missed
rename in prose -- it is a retired identifier surviving somewhere load-bearing:

  * a retired SLUG in a workflow matrix, a catalog fixture or an installer path
    silently ships assets nobody can install;
  * a retired DISPLAY NAME in a bundle, a manual or the catalog contradicts the
    table in §11.1;
  * a retired CLAP ID / AU SUBTYPE makes a host treat an old session as
    compatible with the new product -- exactly what §11.3 forbids;
  * a retired INSTALLER FILENAME breaks the plugin -> installer handoff (§11.5)
    or reintroduces the UAC Installer Detection prompt.

So this enumerates every retired identifier repository-wide and fails on any
occurrence outside the two places §11.6.7 allows: migration material (which must
name the old products to describe the migration) and legacy detection (code that
deliberately recognises an old install in order to leave it alone).

Usage:
    python tools/check_legacy_identity.py          # report + exit 1 on any finding
    python tools/check_legacy_identity.py --list   # dump every occurrence, exit 0
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Retired identifiers, grouped by what breaks when one survives. Only
# PRODUCT-VISIBLE identity is listed: internal code spellings (rs_core, PfCore,
# the pitch_fix_dsp_* CTest names, the RS/DEQ/PF option prefixes) are not
# product IDs and are deliberately out of scope.
RETIRED: dict[str, tuple[str, ...]] = {
    "slug": (
        "resonance-suppressor",
        "dynamic-eq",
        "pitch-fix",
    ),
    "display name": (
        "Resonance TatSuppressor",
        "Resonance TatSupressor",
        "Dynamic Tatsunari EQ",
        "Dynamic TatEQ",
        "Pitch TatFixer",
    ),
    "AU subtype": (
        "Rsup",
        "Dyeq",
        "Pfix",
    ),
    "installer filename": (
        "tatsunari.exe",
        "tatsunari-darwin-",
        "tatsunari-windows-",
    ),
}

# A retired slug is a substring of its replacement ("pitch-fix" is not, but
# "resonance-suppressor" IS inside "tn-resonance-suppressor"), so matches are
# rejected when immediately preceded by the tn- prefix.
_TN_PREFIXED = re.compile(r"tn-$")

# §11.6.7's two permitted homes for a retired identifier.
#
#  1. MIGRATION MATERIAL -- the plans that specify the migration, the archived
#     tree (frozen historical plugins), and this checker's own table.
#  2. LEGACY DETECTION -- code that recognises an old install to leave it alone.
#     Marked in-line with the marker below so the exemption is explicit and
#     greppable rather than path-shaped.
ALLOWED_PREFIXES = (
    "docs/plans/",
    "archive/",
    "tools/check_legacy_identity.py",
    "tools/tests/test_check_legacy_identity.py",
)

LEGACY_MARKER = "legacy-identity-ok"

# Files git tracks but that are not text we can scan.
BINARY_SUFFIXES = {".png", ".jpg", ".jpeg", ".ico", ".ttf", ".otf", ".woff",
                   ".woff2", ".zip", ".syso", ".pdf"}


def tracked_files() -> list[Path]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=REPO_ROOT,
                         check=True, capture_output=True, text=True).stdout
    return [Path(p) for p in out.split("\0") if p]


def scan() -> list[tuple[str, int, str, str, str]]:
    """Return (relpath, lineno, kind, identifier, line) for every finding."""
    findings: list[tuple[str, int, str, str, str]] = []
    for rel in tracked_files():
        posix = rel.as_posix()
        if posix.startswith(ALLOWED_PREFIXES) or rel.suffix.lower() in BINARY_SUFFIXES:
            continue
        try:
            text = (REPO_ROOT / rel).read_text(encoding="utf-8")
        except (UnicodeDecodeError, FileNotFoundError, IsADirectoryError):
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            if LEGACY_MARKER in line:
                continue
            for kind, identifiers in RETIRED.items():
                for ident in identifiers:
                    start = 0
                    while (hit := line.find(ident, start)) != -1:
                        start = hit + 1
                        if _TN_PREFIXED.search(line[:hit]):
                            continue
                        findings.append((posix, lineno, kind, ident, line.strip()))
                        break
    return findings


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true",
                    help="print every occurrence and exit 0 (audit mode)")
    args = ap.parse_args(argv)

    findings = scan()

    if args.list:
        for posix, lineno, kind, ident, line in findings:
            print(f"{posix}:{lineno}: [{kind}] {ident}\n    {line}")
        print(f"\n{len(findings)} occurrence(s).")
        return 0

    if not findings:
        total = sum(len(v) for v in RETIRED.values())
        print(f"no retired product identifiers outside migration material "
              f"({total} identifiers checked)")
        return 0

    print("Retired product identifiers found (plan §11.6.7).\n"
          "Rename to the tn-* identity, move the mention into docs/plans/, or -- if\n"
          f"this really is legacy detection -- add the '{LEGACY_MARKER}' marker to the line.\n")
    for posix, lineno, kind, ident, line in findings:
        print(f"  {posix}:{lineno}: retired {kind} {ident!r}")
        print(f"      {line}")
    print(f"\n{len(findings)} finding(s).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
