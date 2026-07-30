"""Tests for tools/check_plugin_ids.py."""
from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MOD_PATH = ROOT / "tools" / "check_plugin_ids.py"


def _load():
    spec = importlib.util.spec_from_file_location("check_plugin_ids", MOD_PATH)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


class TestCheckPluginIds(unittest.TestCase):
    def test_live_plugins_unique(self):
        mod = _load()
        rows = mod.collect()
        self.assertGreaterEqual(len(rows), 6)  # 3 plugins × stable+dev
        errors = mod.check_unique(rows)
        self.assertEqual(errors, [], msg=errors)

    def test_collision_detected(self):
        mod = _load()
        rows = [
            {"slug": "a", "variant": "stable", "clap_id": "jp.tatsunari-sounds.a",
             "au_subtype": "AAAA", "au_manufacturer": "Ttsn"},
            {"slug": "b", "variant": "stable", "clap_id": "jp.tatsunari-sounds.a",
             "au_subtype": "BBBB", "au_manufacturer": "Ttsn"},
        ]
        errors = mod.check_unique(rows)
        self.assertTrue(any("clap_id=" in e for e in errors))


if __name__ == "__main__":
    unittest.main()
