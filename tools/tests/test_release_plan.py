"""Unit tests for tools/release_plan.py — the release change-detection / manifest
decision core. Fixture plugin.toml + CMakeLists.txt trees and manifest.json files
are built in tempdirs so the tests never depend on the real plugins/."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

# Import the module under test regardless of cwd.
TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import release_plan  # noqa: E402


def make_plugin(root: Path, slug: str, *, version: str, name: str | None = None,
                target: str | None = None, toml_body: str | None = None,
                cmake_body: str | None = None) -> None:
    """Write plugins/<slug>/plugin.toml + CMakeLists.txt under root. cmake_body
    overrides the default juce_add_plugin(<target>) CMakeLists verbatim — used to
    exercise the clap-first (factory_clap_plugin) and ambiguous declarations."""
    d = root / "plugins" / slug
    d.mkdir(parents=True, exist_ok=True)
    if toml_body is None:
        nm = name if name is not None else slug.title()
        toml_body = (
            "[plugin]\n"
            f'name      = "{nm}"\n'
            f'slug      = "{slug}"\n'
            'category  = "Test"\n'
            'status    = "in-progress"\n'
            f'version   = "{version}"\n'
            'formats   = ["VST3", "AU"]\n'
        )
    (d / "plugin.toml").write_text(toml_body, encoding="utf-8")
    if cmake_body is None:
        tgt = target if target is not None else "".join(w.title() for w in slug.split("-"))
        cmake_body = f"juce_add_plugin({tgt}\n  PRODUCT_NAME \"x\")\n"
    (d / "CMakeLists.txt").write_text(cmake_body, encoding="utf-8")


def write_manifest(root: Path, mapping: dict) -> str:
    p = root / "manifest.json"
    p.write_text(json.dumps(mapping), encoding="utf-8")
    return str(p)


class TmpRepoTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def compute(self, prev_manifest: str | None = None) -> dict:
        return release_plan.compute(self.root, prev_manifest)


class FirstReleaseTest(TmpRepoTest):
    def test_no_prev_manifest_all_changed(self):
        make_plugin(self.root, "alpha", version="1.0.0")
        make_plugin(self.root, "beta", version="0.2.0")
        r = self.compute(prev_manifest=None)
        self.assertEqual(set(r["changed"]), {"alpha", "beta"})
        self.assertTrue(r["any_changed"])
        # matrix fans every changed plugin across both OSes
        self.assertEqual(len(r["include"]), 4)
        self.assertEqual({e["os"] for e in r["include"]}, {"macOS", "Windows"})
        self.assertEqual(r["manifest"], {"alpha": "1.0.0", "beta": "0.2.0"})
        for entry in r["plan"]:
            self.assertTrue(entry["changed"])
            self.assertEqual(entry["action"], "build")

    def test_missing_manifest_file_is_first_release(self):
        make_plugin(self.root, "alpha", version="1.0.0")
        r = self.compute(prev_manifest=str(self.root / "does-not-exist.json"))
        self.assertEqual(r["changed"], ["alpha"])
        self.assertTrue(r["any_changed"])


class NoChangesTest(TmpRepoTest):
    def test_all_carry_over(self):
        make_plugin(self.root, "alpha", version="1.0.0")
        make_plugin(self.root, "beta", version="0.2.0")
        prev = write_manifest(self.root, {"alpha": "1.0.0", "beta": "0.2.0"})
        r = self.compute(prev_manifest=prev)
        self.assertEqual(r["changed"], [])
        self.assertFalse(r["any_changed"])
        self.assertEqual(r["include"], [])
        # manifest still lists every current plugin (drives the next run)
        self.assertEqual(r["manifest"], {"alpha": "1.0.0", "beta": "0.2.0"})
        for entry in r["plan"]:
            self.assertFalse(entry["changed"])
            self.assertEqual(entry["action"], "carry-over")


class VersionBumpTest(TmpRepoTest):
    def test_only_bumped_plugin_changes(self):
        make_plugin(self.root, "alpha", version="1.1.0")  # bumped
        make_plugin(self.root, "beta", version="0.2.0")   # unchanged
        prev = write_manifest(self.root, {"alpha": "1.0.0", "beta": "0.2.0"})
        r = self.compute(prev_manifest=prev)
        self.assertEqual(r["changed"], ["alpha"])
        self.assertTrue(r["any_changed"])
        self.assertEqual({e["slug"] for e in r["include"]}, {"alpha"})
        self.assertEqual(len(r["include"]), 2)  # alpha x 2 OS
        actions = {e["slug"]: e["action"] for e in r["plan"]}
        self.assertEqual(actions, {"alpha": "build", "beta": "carry-over"})
        self.assertEqual(r["manifest"], {"alpha": "1.1.0", "beta": "0.2.0"})


class NewPluginTest(TmpRepoTest):
    def test_new_plugin_absent_from_prev_is_changed(self):
        make_plugin(self.root, "alpha", version="1.0.0")  # in prev, unchanged
        make_plugin(self.root, "gamma", version="0.1.0")  # brand new
        prev = write_manifest(self.root, {"alpha": "1.0.0"})
        r = self.compute(prev_manifest=prev)
        self.assertEqual(r["changed"], ["gamma"])
        self.assertTrue(r["any_changed"])
        self.assertEqual(r["manifest"], {"alpha": "1.0.0", "gamma": "0.1.0"})


class RemovedPluginTest(TmpRepoTest):
    def test_removed_plugin_drops_out(self):
        # prev release had alpha + deleted; only alpha exists now.
        make_plugin(self.root, "alpha", version="1.0.0")
        prev = write_manifest(self.root, {"alpha": "1.0.0", "deleted": "9.9.9"})
        r = self.compute(prev_manifest=prev)
        self.assertEqual(r["changed"], [])          # deleted never enters change set
        self.assertFalse(r["any_changed"])
        self.assertNotIn("deleted", r["manifest"])  # dropped from next manifest
        self.assertEqual(r["manifest"], {"alpha": "1.0.0"})
        self.assertEqual([p["slug"] for p in r["plugins"]], ["alpha"])


class GlobOrderTest(TmpRepoTest):
    def test_plugins_sorted_by_slug(self):
        for slug in ["zeta", "alpha", "mu"]:
            make_plugin(self.root, slug, version="1.0.0")
        r = self.compute(prev_manifest=None)
        self.assertEqual([p["slug"] for p in r["plugins"]], ["alpha", "mu", "zeta"])


class TargetResolutionTest(TmpRepoTest):
    def test_target_from_cmake_not_slug(self):
        make_plugin(self.root, "my-plugin", version="1.0.0", target="MyCustomTarget")
        r = self.compute(prev_manifest=None)
        self.assertEqual(r["plugins"][0]["target"], "MyCustomTarget")
        self.assertTrue(all(e["target"] == "MyCustomTarget" for e in r["include"]))


class KindResolutionTest(TmpRepoTest):
    """Each target is tagged with its declaration style: "juce" for
    juce_add_plugin, "clap" for the forward-looking factory_clap_plugin. The
    tag rides the per-plugin structures + the build matrix, never the manifest."""

    def test_juce_target_kind(self):
        make_plugin(self.root, "alpha", version="1.0.0", target="AlphaTarget")
        r = self.compute(prev_manifest=None)
        self.assertEqual(r["plugins"][0]["target"], "AlphaTarget")
        self.assertEqual(r["plugins"][0]["kind"], "juce")
        # kind also rides the plan entry and every build-matrix include entry.
        self.assertEqual(r["plan"][0]["kind"], "juce")
        self.assertTrue(all(e["kind"] == "juce" for e in r["include"]))

    def test_clap_target_kind(self):
        make_plugin(self.root, "alpha", version="1.0.0",
                    cmake_body="factory_clap_plugin(AlphaClap\n  PRODUCT_NAME \"x\")\n")
        r = self.compute(prev_manifest=None)
        self.assertEqual(r["plugins"][0]["target"], "AlphaClap")
        self.assertEqual(r["plugins"][0]["kind"], "clap")
        self.assertEqual(r["plan"][0]["kind"], "clap")
        self.assertTrue(all(e["kind"] == "clap" for e in r["include"]))

    def test_neither_macro_raises(self):
        # A CMakeLists with neither macro -> target unresolved, existing error.
        make_plugin(self.root, "alpha", version="1.0.0",
                    cmake_body="add_library(alpha STATIC alpha.cpp)\n")
        with self.assertRaises(release_plan.ReleasePlanError) as cm:
            self.compute(prev_manifest=None)
        self.assertIn("Could not resolve version/target", str(cm.exception))

    def test_both_macros_raise(self):
        # Declaring both styles is ambiguous -> hard error, like no-target.
        make_plugin(self.root, "alpha", version="1.0.0",
                    cmake_body="juce_add_plugin(AlphaJuce)\n"
                               "factory_clap_plugin(AlphaClap)\n")
        with self.assertRaises(release_plan.ReleasePlanError) as cm:
            self.compute(prev_manifest=None)
        msg = str(cm.exception)
        self.assertIn("multiple", msg)
        self.assertIn("juce_add_plugin", msg)
        self.assertIn("factory_clap_plugin", msg)

    def test_manifest_has_no_kind_field(self):
        # kind must never leak into manifest.json (the carry-over contract),
        # even with a mix of juce and clap plugins in the plan.
        make_plugin(self.root, "alpha", version="1.0.0")
        make_plugin(self.root, "beta", version="0.2.0",
                    cmake_body="factory_clap_plugin(BetaClap\n  PRODUCT_NAME \"x\")\n")
        r = self.compute(prev_manifest=None)
        self.assertEqual(r["manifest"], {"alpha": "1.0.0", "beta": "0.2.0"})
        # Serialised on its own, the manifest carries no "kind" anywhere.
        self.assertNotIn("kind", json.dumps(r["manifest"]))


class ErrorTest(TmpRepoTest):
    def test_malformed_toml_raises_clear_error(self):
        make_plugin(self.root, "alpha", version="1.0.0",
                    toml_body='[plugin\nname = "broken"\n')  # missing ]
        with self.assertRaises(release_plan.ReleasePlanError) as cm:
            self.compute(prev_manifest=None)
        self.assertIn("malformed TOML", str(cm.exception))
        self.assertIn("alpha", str(cm.exception))

    def test_missing_version_raises(self):
        d = self.root / "plugins" / "alpha"
        d.mkdir(parents=True)
        (d / "plugin.toml").write_text('[plugin]\nname = "a"\n', encoding="utf-8")
        (d / "CMakeLists.txt").write_text("juce_add_plugin(Alpha)\n", encoding="utf-8")
        with self.assertRaises(release_plan.ReleasePlanError) as cm:
            self.compute(prev_manifest=None)
        self.assertIn("Could not resolve version/target", str(cm.exception))

    def test_missing_target_raises(self):
        d = self.root / "plugins" / "alpha"
        d.mkdir(parents=True)
        (d / "plugin.toml").write_text(
            '[plugin]\nname = "a"\nversion = "1.0.0"\n', encoding="utf-8"
        )
        # no CMakeLists.txt -> target unresolved
        with self.assertRaises(release_plan.ReleasePlanError) as cm:
            self.compute(prev_manifest=None)
        self.assertIn("Could not resolve version/target", str(cm.exception))

    def test_malformed_prev_manifest_raises(self):
        make_plugin(self.root, "alpha", version="1.0.0")
        bad = self.root / "bad.json"
        bad.write_text("{not json", encoding="utf-8")
        with self.assertRaises(release_plan.ReleasePlanError) as cm:
            self.compute(prev_manifest=str(bad))
        self.assertIn("malformed manifest", str(cm.exception))


class CliTest(TmpRepoTest):
    def test_cli_emits_json_and_exit_zero(self):
        import io
        import contextlib
        make_plugin(self.root, "alpha", version="1.0.0")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = release_plan.main(["--repo-root", str(self.root)])
        self.assertEqual(rc, 0)
        data = json.loads(buf.getvalue())
        self.assertEqual(data["changed"], ["alpha"])
        self.assertIn("manifest", data)

    def test_cli_malformed_toml_exit_one(self):
        import io
        import contextlib
        make_plugin(self.root, "alpha", version="1.0.0",
                    toml_body='[plugin\nbroken\n')
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = release_plan.main(["--repo-root", str(self.root)])
        self.assertEqual(rc, 1)
        self.assertIn("error", err.getvalue())


if __name__ == "__main__":
    unittest.main()
