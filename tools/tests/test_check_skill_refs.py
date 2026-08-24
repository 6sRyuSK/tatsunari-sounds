"""Unit tests for tools/check_skill_refs.py (stdlib only, like the other tools tests)."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import check_skill_refs as csr  # noqa: E402


class ExtractPathsTest(unittest.TestCase):
    def test_takes_backticked_repo_paths(self):
        text = "see `core/include/factory_core/FFT.h` and `tools/gen_catalog.py`"
        self.assertEqual(
            csr.extract_paths(text),
            ["core/include/factory_core/FFT.h", "tools/gen_catalog.py"],
        )

    def test_strips_line_references(self):
        self.assertEqual(csr.extract_paths("`ui/visage/CMakeLists.txt:210`"),
                         ["ui/visage/CMakeLists.txt"])
        self.assertEqual(csr.extract_paths("`plugins/a/shell/ClapEntry.cpp:34-36`"),
                         ["plugins/a/shell/ClapEntry.cpp"])

    def test_skips_placeholders_and_globs(self):
        text = ("`plugins/<slug>/tests/dsp_test.cpp` `plugins/*/CMakeLists.txt` "
                "`plugins/@slug@/ui/x.h`")
        self.assertEqual(csr.extract_paths(text), [])

    def test_skips_include_relative_spellings(self):
        # Resolves against core/include, not the repo root -- not our business.
        self.assertEqual(csr.extract_paths("`factory_core/FFT.h`"), [])

    def test_skips_prose_with_spaces(self):
        self.assertEqual(csr.extract_paths("`tools/ui-dev のハーネス`"), [])


class ExtractFactorySymbolsTest(unittest.TestCase):
    def test_finds_symbols_anywhere_in_the_text(self):
        text = "`-DFACTORY_JUCE_ORACLES=OFF` を渡す。FACTORY_PLUGINS も同様。"
        self.assertEqual(csr.extract_factory_symbols(text),
                         ["FACTORY_JUCE_ORACLES", "FACTORY_PLUGINS"])

    def test_deduplicates_and_sorts(self):
        self.assertEqual(csr.extract_factory_symbols("FACTORY_A FACTORY_A FACTORY_B"),
                         ["FACTORY_A", "FACTORY_B"])


class ExtractFsTestNamesTest(unittest.TestCase):
    def test_takes_rate_swept_names(self):
        text = "`pitch_fix_dsp_<fs>` と `core_primitives_<fs>`"
        self.assertEqual(csr.extract_fs_test_names(text),
                         ["pitch_fix_dsp", "core_primitives"])

    def test_ignores_plain_names(self):
        self.assertEqual(csr.extract_fs_test_names("`resonance_suppressor_preset`"), [])


class RepoScanTest(unittest.TestCase):
    """The scanners run against the real repository."""

    def test_known_cmake_symbols_include_the_real_options(self):
        syms = csr.known_cmake_symbols(csr.REPO_ROOT)
        for expected in ("FACTORY_JUCE_ORACLES", "FACTORY_PLUGINS",
                         "FACTORY_INCLUDE_ARCHIVED", "FACTORY_RS_CLAP_GUI"):
            self.assertIn(expected, syms)

    def test_known_rate_swept_tests_include_the_real_ones(self):
        names = csr.known_rate_swept_tests(csr.REPO_ROOT)
        for expected in ("core_primitives", "pitch_fix_dsp", "resonance_suppressor_dsp"):
            self.assertIn(expected, names)
        # A non-rate-swept registration must NOT appear as a rate-swept base.
        self.assertNotIn("resonance_suppressor_preset", names)

    def test_archived_slugs_are_discovered(self):
        slugs = csr.archived_slugs(csr.REPO_ROOT)
        self.assertIn("saturator", slugs)
        self.assertNotIn("tn-vocal-tuner", slugs)


class CheckSkillTest(unittest.TestCase):
    def _check(self, body, **kw):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "core").mkdir()
            (root / "core" / "real.h").write_text("x", encoding="utf-8")
            skill = root / ".claude" / "skills" / "demo"
            skill.mkdir(parents=True)
            path = skill / "SKILL.md"
            path.write_text(body, encoding="utf-8")
            return csr.check_skill(
                path, root,
                cmake_syms=kw.get("cmake_syms", {"FACTORY_REAL"}),
                fs_tests=kw.get("fs_tests", {"demo_dsp"}),
                archived=kw.get("archived", ["saturator"]),
            )

    def test_clean_skill_has_no_problems(self):
        self.assertEqual(self._check("`core/real.h` FACTORY_REAL `demo_dsp_<fs>`"), [])

    def test_missing_path_is_reported(self):
        problems = self._check("`core/gone.h`")
        self.assertEqual(len(problems), 1)
        self.assertIn("path does not exist: core/gone.h", problems[0])

    def test_unknown_factory_symbol_is_reported(self):
        problems = self._check("FACTORY_GHOST")
        self.assertEqual(len(problems), 1)
        self.assertIn("FACTORY_GHOST", problems[0])

    def test_unregistered_rate_swept_test_is_reported(self):
        problems = self._check("`nope_dsp_<fs>`")
        self.assertEqual(len(problems), 1)
        self.assertIn("not a registered rate-swept CTest: nope_dsp_<fs>", problems[0])

    def test_unmarked_archived_slug_is_reported(self):
        problems = self._check("例: --plugins saturator")
        self.assertEqual(len(problems), 1)
        self.assertIn("archived plugin 'saturator'", problems[0])

    def test_archived_slug_marked_on_the_line_is_accepted(self):
        self.assertEqual(self._check("saturator は archive 済み"), [])


class RealSkillsTest(unittest.TestCase):
    """The gate itself: the checked-in skills must be free of stale references."""

    def test_all_skills_pass(self):
        cmake_syms = csr.known_cmake_symbols(csr.REPO_ROOT)
        fs_tests = csr.known_rate_swept_tests(csr.REPO_ROOT)
        archived = csr.archived_slugs(csr.REPO_ROOT)
        problems = []
        for path in csr._iter_skill_files():
            problems.extend(csr.check_skill(path, csr.REPO_ROOT, cmake_syms,
                                            fs_tests, archived))
        self.assertEqual(problems, [], "\n".join(problems))


if __name__ == "__main__":
    unittest.main()
