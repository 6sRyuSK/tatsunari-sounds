#!/usr/bin/env python3
"""Generate a placeholder artifact set so the promote CODE PATH can be drilled.

`promote.py publish --store memory` is a full rehearsal of the eight steps, but
it still needs files to promote, and a Phase B drill should not have to wait for
a real release to exist. This writes correctly-NAMED, obviously-fake zips and
installer binaries, taking the versions from `plugins/*/plugin.toml` so the
generated manifests describe the real product set.

    python tools/promote/rehearsal.py --artifacts-dir DIR --installer-dir DIR

What this is NOT: a release. The files contain a marker string instead of a
plugin, so nothing they produce may ever be uploaded to a real store. The
guard is that a rehearsal runs with `--store memory`; the marker is there so
that if one ever escaped, the bytes say what they are.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import manifest as mf  # noqa: E402

MARKER = b"NOT A RELEASE - promote rehearsal placeholder (tools/promote/rehearsal.py)\n"

# One per release target, matching release.yml's package step.
TARGETS = ("macOS-AU", "macOS-VST3", "Windows")

# One per installer.yml build-matrix entry.
INSTALLER_BINARIES = (
    "tatsunari-sounds-installer-darwin-amd64",
    "tatsunari-sounds-installer-darwin-arm64",
    "tatsunari-sounds-installer-windows-amd64.exe",
)


def version_token(version: str) -> str:
    return version.replace(".", "_")


def write_artifacts(artifacts_dir: Path, meta: dict[str, mf.PluginMeta]) -> list[Path]:
    """One placeholder zip per (plugin, release target)."""
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for slug, m in sorted(meta.items()):
        for target in TARGETS:
            path = artifacts_dir / f"{slug}-v{version_token(m.version)}-{target}.zip"
            path.write_bytes(MARKER + f"{slug} {m.version} {target}\n".encode())
            written.append(path)
    return written


def write_installers(installer_dir: Path, version: str) -> list[Path]:
    installer_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for name in INSTALLER_BINARIES:
        path = installer_dir / name
        path.write_bytes(MARKER + f"{name} {version}\n".encode())
        written.append(path)
    return written


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--artifacts-dir", required=True)
    ap.add_argument("--installer-dir", required=True)
    ap.add_argument("--installer-version", default="0.0.0-rehearsal")
    args = ap.parse_args(argv)

    meta = mf.load_plugin_meta()
    zips = write_artifacts(Path(args.artifacts_dir), meta)
    bins = write_installers(Path(args.installer_dir), args.installer_version)
    print(f"rehearsal set: {len(zips)} zip(s) for {len(meta)} plugin(s), "
          f"{len(bins)} installer binary/ies")
    print("THESE ARE NOT RELEASE ARTIFACTS. Promote them with --store memory only.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
