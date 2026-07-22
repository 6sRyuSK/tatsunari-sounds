"""Unit tests for tools/gen_catalog.py — README catalog rendering, --check
staleness gating, and the --emit-json (catalog.json) schema the TUI installer
consumes. Fixtures are built in a tempdir; the module's ROOT/README globals are
redirected there so the tests never touch the real README or plugins/."""
from __future__ import annotations

import io
import contextlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import gen_catalog  # noqa: E402


def run_main(args: list[str]) -> None:
    """Drive gen_catalog.main() with argv = ['gen_catalog.py', *args]."""
    orig = sys.argv
    sys.argv = ["gen_catalog.py", *args]
    try:
        gen_catalog.main()
    finally:
        sys.argv = orig

README_TEMPLATE = (
    "# Title\n\nintro\n\n"
    f"{gen_catalog.BEGIN}\n\nOLD STALE CONTENT\n\n{gen_catalog.END}\n\ntail\n"
)


def make_plugin(root: Path, slug: str, *, name: str, category: str, version: str,
                status: str, formats: list[str], reference: str,
                archived: bool = False) -> None:
    d = (root / "archive" / "plugins" / slug) if archived else (root / "plugins" / slug)
    d.mkdir(parents=True, exist_ok=True)
    fmt = ", ".join(f'"{f}"' for f in formats)
    (d / "plugin.toml").write_text(
        "[plugin]\n"
        f'name      = "{name}"\n'
        f'slug      = "{slug}"\n'
        f'category  = "{category}"\n'
        f'status    = "{status}"\n'
        f'version   = "{version}"\n'
        f"formats   = [{fmt}]\n"
        f'reference = "{reference}"\n',
        encoding="utf-8",
    )


class GenCatalogTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

        # Redirect the module's module-level path constants at the fixture tree.
        self._orig_root = gen_catalog.ROOT
        self._orig_readme = gen_catalog.README
        gen_catalog.ROOT = self.root
        gen_catalog.README = self.root / "README.md"
        self.addCleanup(self._restore)

        (self.root / "README.md").write_text(README_TEMPLATE, encoding="utf-8")
        # Two shipped, one in-progress, plus a roadmap (planned) entry.
        make_plugin(self.root, "alpha-eq", name="Alpha EQ", category="EQ",
                    version="1.2.0", status="shipped", formats=["VST3", "AU"],
                    reference="Pro-Q")
        make_plugin(self.root, "beta-comp", name="Beta Comp", category="Dynamics",
                    version="1.0.0", status="shipped", formats=["VST3"],
                    reference="SSL")
        make_plugin(self.root, "gamma-verb", name="Gamma Verb", category="Reverb",
                    version="0.1.0", status="in-progress", formats=["VST3", "AU"],
                    reference="FDN")
        make_plugin(self.root, "old-fuzz", name="Old Fuzz", category="Distortion",
                    version="0.3.0", status="shipped", formats=["VST3", "AU"],
                    reference="Fuzz Face", archived=True)
        (self.root / "roadmap.toml").write_text(
            "[[plugin]]\n"
            'name      = "Delta Delay"\n'
            'category  = "Delay"\n'
            'reference = "Tape echo"\n',
            encoding="utf-8",
        )

    def _restore(self) -> None:
        gen_catalog.ROOT = self._orig_root
        gen_catalog.README = self._orig_readme

    # ---- rendering -------------------------------------------------------
    def test_render_sections_and_counts(self):
        block = gen_catalog.render()
        self.assertIn("### Shipped (2)", block)
        self.assertIn("### In progress (1)", block)
        self.assertIn("### Planned (1)", block)
        # shipped table carries version + formats
        self.assertIn("Alpha EQ", block)
        self.assertIn("1.2.0", block)
        self.assertIn("VST3, AU", block)
        # roadmap entry rendered under Planned
        self.assertIn("Delta Delay", block)
        # in-progress plugin not listed as shipped
        self.assertIn("Gamma Verb", block)

    def test_render_archived_section(self):
        block = gen_catalog.render()
        self.assertIn("### Archived (1)", block)
        self.assertIn("Old Fuzz", block)
        # archived plugins never count as shipped/in-progress
        self.assertIn("### Shipped (2)", block)
        self.assertIn("### In progress (1)", block)

    # ---- --check gating --------------------------------------------------
    def test_check_detects_stale_readme(self):
        # README still holds the OLD STALE CONTENT placeholder -> stale.
        with self.assertRaises(SystemExit) as cm:
            with contextlib.redirect_stdout(io.StringIO()):
                run_main(["--check"])
        self.assertEqual(cm.exception.code, 1)

    def test_check_passes_on_fresh_readme(self):
        # Regenerate first (no --check), then --check must pass without exiting.
        with contextlib.redirect_stdout(io.StringIO()):
            run_main([])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            run_main(["--check"])  # returns normally on fresh
        self.assertIn("up to date", out.getvalue())
        # README now embeds the rendered catalog between the markers.
        text = (self.root / "README.md").read_text(encoding="utf-8")
        self.assertIn("### Shipped (2)", text)
        self.assertIn(gen_catalog.BEGIN, text)
        self.assertIn(gen_catalog.END, text)
        self.assertNotIn("OLD STALE CONTENT", text)

    # ---- --emit-json schema ---------------------------------------------
    def test_emit_json_schema_pins_installer_keys(self):
        dest = self.root / "catalog.json"
        with contextlib.redirect_stdout(io.StringIO()):
            gen_catalog.emit_json(str(dest))
        data = json.loads(dest.read_text(encoding="utf-8"))
        # All plugins (shipped + in-progress), not status-filtered — but NEVER
        # archived ones (the installer must not offer them).
        self.assertEqual(len(data), 3)
        self.assertNotIn("old-fuzz", {e["slug"] for e in data})
        # Keys the installer's release.CatalogEntry decodes (catalog.go).
        required = {"name", "slug", "category", "formats", "status", "version", "reference"}
        for entry in data:
            self.assertTrue(required.issubset(entry.keys()),
                            f"missing keys: {required - set(entry.keys())}")
            self.assertIsInstance(entry["formats"], list)  # array, not joined string
        by_slug = {e["slug"]: e for e in data}
        self.assertEqual(by_slug["alpha-eq"]["formats"], ["VST3", "AU"])
        self.assertEqual(by_slug["alpha-eq"]["version"], "1.2.0")
        self.assertEqual(by_slug["gamma-verb"]["status"], "in-progress")

    def test_emit_json_missing_dest_arg_errors(self):
        with self.assertRaises(SystemExit):
            run_main(["--emit-json"])


if __name__ == "__main__":
    unittest.main()
