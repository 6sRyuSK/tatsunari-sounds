"""Unit tests for tools/check_legacy_identity.py (stdlib only, like the other tools tests)."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import check_legacy_identity as cli  # noqa: E402


class RepoScanTest(unittest.TestCase):
    """The gate's real job: the checked-in tree must be free of retired identity."""

    def test_repository_is_clean(self):
        findings = cli.scan()
        self.assertEqual(
            findings, [],
            "retired product identifiers survive outside migration material:\n"
            + "\n".join(f"  {p}:{n}: [{k}] {i}" for p, n, k, i, _ in findings),
        )

    def test_every_active_slug_is_tn_prefixed(self):
        slugs = sorted(p.name for p in (cli.REPO_ROOT / "plugins").iterdir() if p.is_dir())
        self.assertTrue(slugs, "no active plugins found")
        for slug in slugs:
            self.assertTrue(slug.startswith("tn-"), f"{slug} is not a tn-* product key")


class _TempRepo:
    """A throwaway git repo so scan() (which uses `git ls-files`) has something to walk."""

    def __init__(self, files: dict[str, str]):
        self.files = files

    def __enter__(self) -> Path:
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        for rel, text in self.files.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text, encoding="utf-8")
        subprocess.run(["git", "init", "-q"], cwd=root, check=True)
        subprocess.run(["git", "add", "-A"], cwd=root, check=True)
        self._saved = cli.REPO_ROOT
        cli.REPO_ROOT = root
        return root

    def __exit__(self, *exc):
        cli.REPO_ROOT = self._saved
        self._tmp.cleanup()
        return False


def kinds(findings):
    return sorted({(f[0], f[2], f[3]) for f in findings})


class DetectionTest(unittest.TestCase):
    def test_flags_a_retired_slug(self):
        with _TempRepo({".github/workflows/clap.yml": "        slug: [pitch-fix]\n"}):
            self.assertEqual(
                kinds(cli.scan()),
                [(".github/workflows/clap.yml", "slug", "pitch-fix")],
            )

    def test_flags_a_retired_display_name_and_au_subtype(self):
        with _TempRepo({
            "a/CMakeLists.txt": 'OUTPUT_NAME "Resonance TatSuppressor"\n',
            "b/CMakeLists.txt": "AUV2_SUBTYPE_CODE  Rsup\n",
        }):
            self.assertEqual(kinds(cli.scan()), [
                ("a/CMakeLists.txt", "display name", "Resonance TatSuppressor"),
                ("b/CMakeLists.txt", "AU subtype", "Rsup"),
            ])

    def test_flags_a_retired_installer_filename(self):
        with _TempRepo({"boot.ps1": "$bin = Join-Path $dir 'tatsunari.exe'\n"}):
            self.assertEqual(
                kinds(cli.scan()),
                [("boot.ps1", "installer filename", "tatsunari.exe")],
            )

    def test_tn_prefixed_slug_is_not_a_match(self):
        # "resonance-suppressor" is a substring of "tn-resonance-suppressor";
        # the replacement must never trip its own gate.
        with _TempRepo({
            "x.md": "plugins/tn-resonance-suppressor and tn-equalizer and tn-vocal-tuner\n",
        }):
            self.assertEqual(cli.scan(), [])

    def test_migration_material_is_exempt(self):
        with _TempRepo({
            "docs/plans/11-product-identity-migration.md": "旧 slug は pitch-fix だった\n",
            "archive/plugins/pitch-fix/plugin.toml": 'slug = "pitch-fix"\n',
        }):
            self.assertEqual(cli.scan(), [])

    def test_marked_legacy_detection_is_exempt(self):
        with _TempRepo({
            "internal/legacy.go": (
                'const oldSlug = "dynamic-eq" // legacy-identity-ok: detect, never remove\n'
            ),
        }):
            self.assertEqual(cli.scan(), [])

    def test_marker_exempts_only_its_own_line(self):
        with _TempRepo({
            "internal/legacy.go": (
                'const a = "dynamic-eq" // legacy-identity-ok\n'
                'const b = "dynamic-eq"\n'
            ),
        }):
            findings = cli.scan()
            self.assertEqual([f[1] for f in findings], [2])

    def test_binary_files_are_skipped(self):
        with _TempRepo({"tools/installer/rsrc_windows_amd64.syso": "pitch-fix\n"}):
            self.assertEqual(cli.scan(), [])


class ExitCodeTest(unittest.TestCase):
    def test_main_returns_zero_when_clean(self):
        with _TempRepo({"x.md": "tn-equalizer\n"}):
            self.assertEqual(cli.main([]), 0)

    def test_main_returns_one_on_a_finding(self):
        with _TempRepo({"x.md": "dynamic-eq\n"}):
            self.assertEqual(cli.main([]), 1)

    def test_list_mode_never_fails(self):
        with _TempRepo({"x.md": "dynamic-eq\n"}):
            self.assertEqual(cli.main(["--list"]), 0)


if __name__ == "__main__":
    unittest.main()
