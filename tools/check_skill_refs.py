#!/usr/bin/env python3
"""Drift check for .claude/skills/*/SKILL.md against the real repository.

The skills are a contract ("read these INSTEAD of other sources"), so a stale
reference is worse than a missing one: an agent implements the old path without
ever checking. Most of the staleness found in the 2026-07 audit had a mechanical
shape -- a reference to something that no longer exists -- so this checks exactly
that class and nothing subjective:

  1. PATHS      -- every backticked repo-relative path must exist on disk.
  2. CMAKE      -- every FACTORY_* symbol must appear in a CMake file or workflow.
  3. CTEST      -- every backticked `<name>_<fs>` must be a registered rate-swept
                   CTest name (add_test(NAME <name>_${_fs})).
  4. ARCHIVED   -- an archived plugin slug must be marked as archived on the line
                   that mentions it, so examples never point at a dead plugin.

It cannot prove a skill is CORRECT -- only that the things it names still exist.
Prose accuracy stays a human/agent review job.

Usage:
    python tools/check_skill_refs.py           # report + exit 1 on any finding
    python tools/check_skill_refs.py --list    # dump what was extracted, exit 0
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SKILLS_DIR = REPO_ROOT / ".claude" / "skills"

# A backticked token is treated as a repo path only when it starts with one of
# these top-level entries. This deliberately skips include-relative spellings
# (e.g. `factory_core/FFT.h`, which resolves against core/include).
PATH_ROOTS = (
    ".claude/",
    ".github/",
    "archive/",
    "cmake/",
    "core/",
    "docs/",
    "params/",
    "plugins/",
    "presets/",
    "shell/",
    "tools/",
    "ui/",
    "update/",
)

# Placeholders (<slug>, <Camel>Params.h), globs, and ellipses are not real paths.
PLACEHOLDER_CHARS = set("<>*?@{}|…")

# `path.ext:123` / `path.ext:12-34` -- a line reference, not part of the path.
LINE_REF_RE = re.compile(r":\d+(?:-\d+)?$")

BACKTICK_RE = re.compile(r"`([^`\n]+)`")
# No leading \b: the skills write these as `-DFACTORY_JUCE_ORACLES=OFF`, where the
# "-D" leaves no word boundary in front of FACTORY.
FACTORY_SYM_RE = re.compile(r"FACTORY_[A-Z0-9_]+\b")
FS_TEST_RE = re.compile(r"^([a-z][a-z0-9_]*)_<fs>$")
ADD_TEST_RE = re.compile(r"add_test\(\s*NAME\s+([A-Za-z0-9_${}]+)")

# Symbols the skills legitimately name that are defined outside CMake/workflows
# (or are deliberately described as NOT existing). Keep this list short and
# justified -- it is an escape hatch, not a dumping ground.
FACTORY_SYM_ALLOWLIST: set[str] = {
    "FACTORY_UI_ABI_EXPORTS",  # a CMake variable inside tools/ui-dev (scanned separately)
}


def _iter_skill_files() -> list[Path]:
    return sorted(SKILLS_DIR.glob("*/SKILL.md"))


def _backticked(text: str) -> list[str]:
    return [m.group(1).strip() for m in BACKTICK_RE.finditer(text)]


def extract_paths(text: str) -> list[str]:
    """Backticked tokens that name a repo-relative path."""
    out = []
    for tok in _backticked(text):
        if not tok.startswith(PATH_ROOTS):
            continue
        if PLACEHOLDER_CHARS & set(tok):
            continue
        if " " in tok:
            continue
        out.append(LINE_REF_RE.sub("", tok))
    return out


def extract_factory_symbols(text: str) -> list[str]:
    return sorted(set(FACTORY_SYM_RE.findall(text)) - FACTORY_SYM_ALLOWLIST)


def extract_fs_test_names(text: str) -> list[str]:
    """Backticked `<base>_<fs>` tokens -- rate-swept CTest names."""
    out = []
    for tok in _backticked(text):
        m = FS_TEST_RE.match(tok)
        if m:
            out.append(m.group(1))
    return out


def known_cmake_symbols(repo_root: Path) -> set[str]:
    """FACTORY_* symbols defined or used anywhere in the build / CI definition."""
    syms: set[str] = set()
    globs = ("CMakeLists.txt", "*.cmake", ".github/workflows/*.yml")
    for pattern in globs:
        for path in repo_root.rglob(pattern):
            if "/build" in str(path):
                continue
            try:
                syms.update(FACTORY_SYM_RE.findall(path.read_text(encoding="utf-8", errors="ignore")))
            except OSError:
                continue
    return syms


def known_rate_swept_tests(repo_root: Path) -> set[str]:
    """Base names of add_test(NAME <base>_${_fs}) registrations."""
    names: set[str] = set()
    for path in repo_root.rglob("CMakeLists.txt"):
        if "/build" in str(path):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for raw in ADD_TEST_RE.findall(text):
            if raw.endswith("${_fs}"):
                names.add(raw[: -len("${_fs}")].rstrip("_"))
    return names


def archived_slugs(repo_root: Path) -> list[str]:
    d = repo_root / "archive" / "plugins"
    if not d.is_dir():
        return []
    return sorted(p.name for p in d.iterdir() if p.is_dir())


def check_skill(path: Path, repo_root: Path, cmake_syms: set[str],
                fs_tests: set[str], archived: list[str]) -> list[str]:
    text = path.read_text(encoding="utf-8")
    rel = path.relative_to(repo_root)
    problems: list[str] = []

    for p in extract_paths(text):
        if not (repo_root / p).exists():
            problems.append(f"{rel}: path does not exist: {p}")

    for sym in extract_factory_symbols(text):
        if sym not in cmake_syms:
            problems.append(f"{rel}: no CMake/workflow defines or uses: {sym}")

    for base in extract_fs_test_names(text):
        if base not in fs_tests:
            problems.append(f"{rel}: not a registered rate-swept CTest: {base}_<fs>")

    for line_no, line in enumerate(text.splitlines(), start=1):
        if "archive" in line.lower():
            continue
        for slug in archived:
            if re.search(rf"\b{re.escape(slug)}\b", line):
                problems.append(
                    f"{rel}:{line_no}: mentions archived plugin '{slug}' "
                    f"without marking it archived"
                )
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true",
                    help="print what was extracted per skill and exit 0")
    args = ap.parse_args(argv)

    skills = _iter_skill_files()
    if not skills:
        print(f"::error::no skills found under {SKILLS_DIR}", file=sys.stderr)
        return 1

    cmake_syms = known_cmake_symbols(REPO_ROOT)
    fs_tests = known_rate_swept_tests(REPO_ROOT)
    archived = archived_slugs(REPO_ROOT)

    if args.list:
        for path in skills:
            text = path.read_text(encoding="utf-8")
            print(f"--- {path.relative_to(REPO_ROOT)}")
            print(f"    paths:   {len(extract_paths(text))}")
            print(f"    FACTORY: {', '.join(extract_factory_symbols(text)) or '-'}")
            print(f"    _<fs>:   {', '.join(extract_fs_test_names(text)) or '-'}")
        return 0

    problems: list[str] = []
    for path in skills:
        problems.extend(check_skill(path, REPO_ROOT, cmake_syms, fs_tests, archived))

    if problems:
        for p in problems:
            print(f"::error::{p}")
        print(f"\n{len(problems)} stale reference(s) in {len(skills)} skill(s).",
              file=sys.stderr)
        return 1

    print(f"skill references OK ({len(skills)} skills checked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
