#!/usr/bin/env python3
"""Dry-run / staging promote skeleton for /tatsunarisounds/ updates (plan §5).

Pipeline (plan):
  validate → upload immutable → read-back verify → generate manifest → sign
  → upload manifest → smoke → pointer switch

This script implements the LOCAL / dry-run half so CI can exercise the shape
without R2 secrets or minisign production keys (those are Ask-a-human).

Usage:
  python tools/promote/promote.py --dry-run \\
      --artifacts-dir dist/ \\
      --out-dir /tmp/promote-out \\
      --channel stable

Exit 0 on success. Never uploads when --dry-run is set (the default).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def planned_key(slug: str, version: str, filename: str) -> str:
    return f"tatsunarisounds/artifacts/{slug}/{version}/{filename}"


def collect_artifacts(root: Path) -> list[dict]:
    rows = []
    if not root.is_dir():
        return rows
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        if p.suffix.lower() not in {".zip", ".clap", ".vst3"} and "tatsunari" not in p.name:
            # Accept zips and installer binaries; skip noise.
            if not p.name.startswith("tatsunari"):
                continue
        rel = p.relative_to(root).as_posix()
        # Heuristic: <slug>-vX_Y_Z-... or flat files under <slug>/<ver>/
        parts = Path(rel).parts
        slug = parts[0] if len(parts) > 1 else p.stem.split("-v")[0]
        version = "0.0.0"
        name = p.name
        if "-v" in name:
            try:
                ver = name.split("-v", 1)[1].split("-", 1)[0]
                version = ver.replace("_", ".")
            except IndexError:
                pass
        digest = sha256_file(p)
        rows.append(
            {
                "path": str(p),
                "filename": name,
                "slug": slug,
                "version": version,
                "size": p.stat().st_size,
                "sha256": digest,
                "objectKey": planned_key(slug, version, name),
            }
        )
    return rows


def write_latest_stub(plugins: list[dict], out: Path) -> None:
    body = {
        "schema": 1,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "plugins": [
            {"slug": p["slug"], "latest": p["version"], "highlights": []}
            for p in plugins
        ],
    }
    out.write_text(json.dumps(body, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dry-run", action="store_true", default=True,
                    help="Print plan only; never touch the network (default)")
    ap.add_argument("--upload", action="store_true",
                    help="Reserved: real R2 upload (requires secrets; refused for now)")
    ap.add_argument("--artifacts-dir", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--channel", default="stable")
    args = ap.parse_args()

    if args.upload:
        print("ERROR: --upload is gated on R2 secrets + human approval; use --dry-run",
              file=sys.stderr)
        return 2

    args.out_dir.mkdir(parents=True, exist_ok=True)
    artifacts = collect_artifacts(args.artifacts_dir)

    plan = {
        "dryRun": True,
        "channel": args.channel,
        "steps": [
            "validate",
            "upload_immutable",
            "read_back_verify",
            "generate_manifest",
            "sign",
            "upload_manifest",
            "smoke",
            "pointer_switch",
        ],
        "artifacts": artifacts,
        "notes": [
            "minisign production keys not embedded — sign step is a no-op in dry-run",
            "pointer switch is logged only",
            "CDN host: 6sryusk.com path prefix /tatsunarisounds/",
        ],
    }
    plan_path = args.out_dir / "promote-plan.json"
    plan_path.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")

    # Deduplicate plugins by slug keeping the max version string (lexicographic —
    # good enough for dry-run stubs; real promote uses SemVer).
    by_slug: dict[str, dict] = {}
    for a in artifacts:
        prev = by_slug.get(a["slug"])
        if prev is None or a["version"] > prev["version"]:
            by_slug[a["slug"]] = a
    write_latest_stub(list(by_slug.values()), args.out_dir / "latest.json")

    print(f"dry-run promote: {len(artifacts)} artifact(s)")
    for a in artifacts:
        print(f"  {a['objectKey']}  sha256={a['sha256'][:12]}…  size={a['size']}")
    print(f"wrote {plan_path}")
    print(f"wrote {args.out_dir / 'latest.json'}")
    print("pointer switch: SKIPPED (dry-run)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
