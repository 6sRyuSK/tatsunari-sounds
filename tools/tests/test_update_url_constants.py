#!/usr/bin/env python3
"""Gate drift between C++ factory_update::Urls and Go updates/hosts.go."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / "update" / "include" / "factory_update" / "Urls.h"
GO = ROOT / "tools" / "installer" / "internal" / "updates" / "hosts.go"

KEYS = [
    ("PublicHost", "kPublicHost", "6sryusk.com"),
    ("LatestJSONURL", "kLatestJSONURL", "https://6sryusk.com/tatsunarisounds/updates/v1/latest.json"),
    ("CatalogJSONURL", "kCatalogJSONURL", "https://6sryusk.com/tatsunarisounds/updates/v1/catalog.json"),
    ("HumanUpdatesURL", "kHumanUpdatesURL", "https://6sryusk.com/tatsunarisounds/updates/"),
    ("PathUpdatesV1", "kPathUpdatesV1", "/tatsunarisounds/updates/v1/"),
    ("PathArtifacts", "kPathArtifacts", "/tatsunarisounds/artifacts/"),
    ("PathNotes", "kPathNotes", "/tatsunarisounds/notes/"),
]


def _extract_go(text: str, name: str) -> str | None:
    m = re.search(rf"{name}\s*=\s*\"([^\"]+)\"", text)
    return m.group(1) if m else None


def _extract_cpp(text: str, name: str) -> str | None:
    m = re.search(rf"{name}\s*=\s*\"([^\"]+)\"", text)
    return m.group(1) if m else None


class UpdateUrlConstants(unittest.TestCase):
    def test_cpp_and_go_agree(self) -> None:
        cpp = CPP.read_text(encoding="utf-8")
        go = GO.read_text(encoding="utf-8")
        for go_name, cpp_name, expected in KEYS:
            g = _extract_go(go, go_name)
            c = _extract_cpp(cpp, cpp_name)
            self.assertEqual(g, expected, f"Go {go_name}")
            self.assertEqual(c, expected, f"C++ {cpp_name}")
            self.assertEqual(g, c, f"{go_name} vs {cpp_name}")

    def test_max_latest_bytes(self) -> None:
        cpp = CPP.read_text(encoding="utf-8")
        go = (ROOT / "tools/installer/internal/updates/limits.go").read_text(encoding="utf-8")
        self.assertIn("kMaxLatestBytes = 256 * 1024", cpp)
        self.assertIn("MaxLatestBytes    = 256 * 1024", go)


if __name__ == "__main__":
    unittest.main()
