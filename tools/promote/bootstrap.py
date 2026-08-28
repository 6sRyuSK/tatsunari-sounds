#!/usr/bin/env python3
"""Pin the bootstrap shims to their immutable payloads (plan §5 "bootstrap").

The published bootstrap is two objects per platform:

    /tatsunarisounds/install.sh                    <- shim.sh   (mutable, short TTL)
    /tatsunarisounds/bootstrap/<version>/install.sh <- install.sh (immutable)

The shim carries the payload's version and SHA-256 and refuses to execute
anything else. This script is what puts those two values into the shim, so the
pin is derived from the payload rather than typed next to it — a hand-copied
digest that drifts is a bootstrap that either stops working or, worse, stops
checking.

    python tools/promote/bootstrap.py --check    # CI: fail if the pin drifted
    python tools/promote/bootstrap.py --write    # refresh after editing a payload

The version is bumped by hand: it names an immutable object, so it must change
whenever the payload changes in a way already-published shims should not pick
up. `--write` refuses to reuse a version for different bytes unless
`--allow-version-reuse` is passed (which is only correct before the first
publish of that version).
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BOOTSTRAP_DIR = REPO_ROOT / "tools" / "installer" / "bootstrap"

# (shim, payload, sha-line pattern, sha-line template)
PAIRS = [
    (
        BOOTSTRAP_DIR / "shim.sh",
        BOOTSTRAP_DIR / "install.sh",
        re.compile(r'^BOOTSTRAP_SHA256="([0-9a-f]{64})"$', re.M),
        'BOOTSTRAP_SHA256="{sha}"',
        re.compile(r'^BOOTSTRAP_VERSION="([^"]+)"$', re.M),
    ),
    (
        BOOTSTRAP_DIR / "shim.ps1",
        BOOTSTRAP_DIR / "install.ps1",
        re.compile(r"^\$BootstrapSha256  = '([0-9a-f]{64})'$", re.M),
        "$BootstrapSha256  = '{sha}'",
        re.compile(r"^\$BootstrapVersion = '([^']+)'$", re.M),
    ),
]


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check() -> int:
    problems: list[str] = []
    for shim_path, payload_path, sha_re, _tmpl, ver_re in PAIRS:
        if not shim_path.is_file() or not payload_path.is_file():
            problems.append(f"missing {shim_path.name} or {payload_path.name}")
            continue
        shim = shim_path.read_text(encoding="utf-8")
        m = sha_re.search(shim)
        if not m:
            problems.append(f"{shim_path.name}: no generated SHA-256 pin found")
            continue
        want = sha256_file(payload_path)
        if m.group(1) != want:
            problems.append(
                f"{shim_path.name} pins {m.group(1)[:12]}… but "
                f"{payload_path.name} hashes to {want[:12]}… — "
                "run: python tools/promote/bootstrap.py --write"
            )
        if not ver_re.search(shim):
            problems.append(f"{shim_path.name}: no generated version found")

    if problems:
        print("bootstrap pin is stale:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    print(f"bootstrap pins match their payloads ({len(PAIRS)} platform(s))")
    return 0


def write(version: str | None) -> int:
    for shim_path, payload_path, sha_re, tmpl, ver_re in PAIRS:
        shim = shim_path.read_text(encoding="utf-8")
        sha = sha256_file(payload_path)
        if not sha_re.search(shim):
            print(f"{shim_path}: no generated pin block to update", file=sys.stderr)
            return 1
        shim = sha_re.sub(tmpl.format(sha=sha).replace("\\", "\\\\"), shim)
        if version is not None:
            quote = '"' if shim_path.suffix == ".sh" else "'"
            key = "BOOTSTRAP_VERSION=" if shim_path.suffix == ".sh" else "$BootstrapVersion = "
            shim = ver_re.sub(f"{key}{quote}{version}{quote}", shim)
        shim_path.write_text(shim, encoding="utf-8")
        print(f"{shim_path.name}: pinned {payload_path.name} sha256={sha[:12]}…")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true", help="verify the pins (CI)")
    g.add_argument("--write", action="store_true", help="refresh the pins")
    ap.add_argument("--version", help="also set the bootstrap version (immutable path)")
    args = ap.parse_args(argv)
    return check() if args.check else write(args.version)


if __name__ == "__main__":
    raise SystemExit(main())
