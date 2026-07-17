#!/usr/bin/env python3
"""
tools/release_plan.py — the extracted, unit-tested DECISION core of release.yml.

release.yml's `plan` job compares every plugins/<slug>/plugin.toml version against
the previous release's manifest.json to decide which plugins must be rebuilt and
which carry over verbatim, and generates the next release's manifest.json. That
logic used to live as inline shell in the workflow; it lives here now so it can
be tested. The workflow keeps the MECHANICS (resolving the tag, downloading the
previous manifest, building/zipping, copying carry-over assets, publishing) — this
script only DECIDES. Semantics are identical to the previous inline shell.

Inputs:
  * the current plugins/<slug>/plugin.toml versions (scanned under --repo-root)
  * the previous release's manifest.json (slug -> dotted version), optional

Output (JSON object on stdout):
  {
    "plugins":     [{slug, version, name, target, kind}, ...],  # every plugin, glob order
    "changed":     ["slug", ...],                               # rebuild set
    "include":     [{slug, target, kind, version, os}, ...],    # build matrix (changed x OS)
    "any_changed": bool,
    "manifest":    {"slug": "version", ...},                    # next release's manifest.json
    "plan":        [{slug, version, name, target, kind, changed, action}, ...]
  }

`action` is "build" for changed plugins and "carry-over" for unchanged ones.
`kind` is the target-declaration style — "juce" (juce_add_plugin) or "clap"
(factory_clap_plugin) — carried on every per-plugin/per-target structure so a
future release.yml can branch the build on it. It is deliberately kept OUT of
`manifest`, which stays the slug->version carry-over contract with prior releases.

Edge cases (preserved verbatim from the inline shell):
  * No previous manifest (first release, or --prev-manifest omitted / missing
    file): every plugin is treated as changed.
  * A plugin present in the previous manifest but deleted now: it is simply not
    scanned, so it drops out of `changed`, `include` and the new `manifest`.
  * A new plugin with no previous manifest entry: its previous version reads as
    "" which never equals its real version, so it is changed.

CLI:
  python tools/release_plan.py --repo-root . [--prev-manifest prevrel/manifest.json]

Requires Python 3.11+ (stdlib tomllib).
"""
from __future__ import annotations

import argparse
import glob
import json
import re
import sys
import tomllib
from pathlib import Path

# Native-OS build matrix: each changed plugin fans out to one entry per OS. Kept
# in this order to match the previous inline shell (`for os in macOS Windows`).
OSES = ["macOS", "Windows"]

# Extract the CMake target from the plugin's declaration macro, keyed by the
# declaration style ("kind"). Two styles are recognised: the current JUCE
# `juce_add_plugin(<Target> ...)` and the forward-looking clap-first
# `factory_clap_plugin(<Target> ...)` (whose macro does not exist in the repo
# yet — that's expected). Both mirror the workflow's grep/sed: first call of the
# macro, first identifier after the opening paren.
_TARGET_RES = {
    "juce": re.compile(r"juce_add_plugin\(\s*([A-Za-z0-9_]+)"),
    "clap": re.compile(r"factory_clap_plugin\(\s*([A-Za-z0-9_]+)"),
}


class ReleasePlanError(Exception):
    """A plugin's version/target/toml could not be resolved — fail the plan
    loudly rather than silently mis-decide what to rebuild."""


def _plugin_table(toml_path: Path) -> dict:
    """Parse plugin.toml and return its plugin table (mirrors gen_catalog's
    `raw.get("plugin", raw)`). A malformed toml is a hard error."""
    try:
        raw = tomllib.loads(toml_path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as exc:
        raise ReleasePlanError(f"malformed TOML in {toml_path}: {exc}") from exc
    return raw.get("plugin", raw)


def _resolve_target(cmake_path: Path) -> tuple[str, str]:
    """Resolve a plugin's (target, kind) from its CMakeLists. kind is "juce" for
    a juce_add_plugin target, "clap" for a factory_clap_plugin one. Declaring
    both styles in one file is ambiguous — which one ships? — so it is a hard
    error, in the same spirit as the no-target case. A missing file or no match
    yields ("", ""), which enumerate_plugins turns into the no-target error."""
    if not cmake_path.is_file():
        return "", ""
    text = cmake_path.read_text(encoding="utf-8")
    found: dict[str, str] = {}
    for kind, rx in _TARGET_RES.items():
        m = rx.search(text)
        if m:
            found[kind] = m.group(1)
    if len(found) > 1:
        raise ReleasePlanError(
            f"{cmake_path.parent.name} declares multiple plugin target styles "
            f"({', '.join(sorted(found))}); expected exactly one of "
            "juce_add_plugin / factory_clap_plugin"
        )
    for kind, target in found.items():
        return target, kind
    return "", ""


def enumerate_plugins(repo_root: Path) -> list[dict]:
    """Every plugins/<slug>/ with a plugin.toml, in glob (sorted) order. slug is
    the directory name — matching the workflow's `basename` — not the toml field."""
    plugins: list[dict] = []
    pattern = str(repo_root / "plugins" / "*" / "plugin.toml")
    for toml_str in sorted(glob.glob(pattern)):
        toml_path = Path(toml_str)
        slug = toml_path.parent.name
        p = _plugin_table(toml_path)
        version = str(p.get("version", "")).strip()
        name = str(p.get("name", "")).strip()
        target, kind = _resolve_target(toml_path.parent / "CMakeLists.txt")
        if not version or not target:
            raise ReleasePlanError(
                f"Could not resolve version/target for {slug} "
                f"(version={version!r}, target={target!r})"
            )
        plugins.append(
            {"slug": slug, "version": version, "name": name,
             "target": target, "kind": kind}
        )
    return plugins


def load_prev_manifest(path: str | None) -> dict | None:
    """Previous release's manifest.json (slug -> version), or None for the
    first-release case. A missing path/file is the first-release case, not an
    error (the workflow only downloads it when a previous tag exists)."""
    if not path:
        return None
    p = Path(path)
    if not p.is_file():
        return None
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ReleasePlanError(f"malformed manifest {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise ReleasePlanError(f"manifest {path} is not a JSON object")
    return data


def build_plan(plugins: list[dict], prev_manifest: dict | None) -> dict:
    changed: list[str] = []
    include: list[dict] = []
    plan: list[dict] = []
    manifest: dict[str, str] = {}
    for pl in plugins:
        slug, ver = pl["slug"], pl["version"]
        prev_ver = "" if prev_manifest is None else str(prev_manifest.get(slug, ""))
        is_changed = prev_ver != ver
        if is_changed:
            changed.append(slug)
            for os_ in OSES:
                include.append(
                    {"slug": slug, "target": pl["target"], "kind": pl["kind"],
                     "version": ver, "os": os_}
                )
        plan.append(
            {**pl, "changed": is_changed, "action": "build" if is_changed else "carry-over"}
        )
        manifest[slug] = ver
    return {
        "plugins": plugins,
        "changed": changed,
        "include": include,
        "any_changed": len(changed) > 0,
        "manifest": manifest,
        "plan": plan,
    }


def compute(repo_root: Path, prev_manifest_path: str | None) -> dict:
    plugins = enumerate_plugins(repo_root)
    prev = load_prev_manifest(prev_manifest_path)
    return build_plan(plugins, prev)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Release change-detection / manifest plan.")
    ap.add_argument("--repo-root", default=".", help="repository root (default: .)")
    ap.add_argument(
        "--prev-manifest",
        default=None,
        help="path to the previous release's manifest.json (omit for first release)",
    )
    args = ap.parse_args(argv)
    try:
        result = compute(Path(args.repo_root), args.prev_manifest)
    except ReleasePlanError as exc:
        print(f"release_plan: error: {exc}", file=sys.stderr)
        return 1
    # Compact JSON so the workflow can pass single-line values through GITHUB_OUTPUT.
    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
