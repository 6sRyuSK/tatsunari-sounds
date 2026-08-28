"""Unit tests for tools/promote (stdlib only, like the other tools tests).

These cover the properties a promote must not lose, stated as the failure they
prevent rather than as "function returns X":

  * an immutable artifact key is never overwritten with different bytes;
  * nothing reaches a manifest that was not read back and re-digested;
  * the live pointer is archived before it is replaced, so rollback has a target;
  * a rollback restores a pointer and refuses to run without an explicit --yes;
  * the generated documents match the published latest.schema.json and use only
    the enum values the Go client accepts (an unknown arch silently drops the
    asset, i.e. a plugin that does not install).
"""

import json
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS / "promote"))

import manifest as mf  # noqa: E402
import promote  # noqa: E402
import signing  # noqa: E402
from store import MemoryStore, sha256_bytes, sha256_file  # noqa: E402

REPO_ROOT = TOOLS.parent

# The plugin-asset enums the Go client validates in
# tools/installer/internal/updates/parse.go. Duplicated here deliberately: if
# the client ever narrows them, this test is where the mismatch surfaces before
# a release does.
CLIENT_OS = {"macos", "windows"}
CLIENT_PLUGIN_ARCH = {"universal", "x86_64", "arm64"}
CLIENT_FORMATS = {"vst3", "au", "clap"}


def write_zips(root: Path, names) -> None:
    root.mkdir(parents=True, exist_ok=True)
    for n in names:
        (root / n).write_bytes(f"payload-{n}".encode())


class _Args:
    """Duck-typed stand-in for argparse.Namespace."""

    def __init__(self, **kw):
        self.store = "memory"
        self.bucket = None
        self.endpoint_url = None
        self.sign = False
        self.secret_key_env = "MINISIGN_SECRET_KEY"
        self.public_key = None
        self.quiet = True
        self.channel = "stable"
        self.__dict__.update(kw)


def run_publish(tmp: Path, names, store=None):
    """Publish `names` into a store, returning (store, out_dir)."""
    art, out = tmp / "art", tmp / "out"
    write_zips(art, names)
    out.mkdir(parents=True, exist_ok=True)
    args = _Args(artifacts_dir=str(art), out_dir=str(out))
    store = store or MemoryStore()
    promote.make_store = lambda _a, _s=store: _s  # inject
    rc = promote.cmd_publish(args)
    assert rc == 0, "publish should succeed"
    return store, out


class ImmutabilityTest(unittest.TestCase):
    def setUp(self):
        self._orig_make_store = promote.make_store
        self.addCleanup(lambda: setattr(promote, "make_store", self._orig_make_store))

    def test_republishing_identical_bytes_is_a_noop(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            names = ["tn-equalizer-v0_1_0-macOS-VST3.zip"]
            store, _ = run_publish(tmp / "a", names)
            before = dict(store.objects)
            # Same inputs, same store: a re-run after a partial failure must be safe.
            run_publish(tmp / "b", names, store=store)
            key = "artifacts/tn-equalizer/0.1.0/tn-equalizer-v0_1_0-macOS-VST3.zip"
            self.assertEqual(store.objects[key], before[key])

    def test_different_bytes_under_an_existing_key_is_refused(self):
        store = MemoryStore()
        art = mf.Artifact(
            path=Path("/nonexistent.zip"), slug="tn-equalizer", version="0.1.0",
            fmt="vst3", os_id="macos", arch="universal", subpath="VST3/tatsunari-sounds",
            sha256="b" * 64, size=1,
        )
        store.put(art.object_key, b"the bytes someone already installed",
                  content_type="application/zip", cache_control="immutable")
        with self.assertRaises(promote.PromoteError) as ctx:
            promote.upload_immutable(store, [art], lambda _m: None)
        self.assertIn("never overwritten", str(ctx.exception))

    def test_read_back_mismatch_stops_the_promote(self):
        store = MemoryStore()
        art = mf.Artifact(
            path=Path("/nonexistent.zip"), slug="s", version="0.1.0", fmt="vst3",
            os_id="macos", arch="universal", subpath="VST3/tatsunari-sounds",
            sha256=sha256_bytes(b"expected"), size=8,
        )
        store.put(art.object_key, b"corrupted",
                  content_type="application/zip", cache_control="immutable")
        with self.assertRaises(promote.PromoteError) as ctx:
            promote.read_back_verify(store, [art], lambda _m: None)
        self.assertIn("read-back digest mismatch", str(ctx.exception))

    def test_missing_object_after_upload_stops_the_promote(self):
        store = MemoryStore()
        art = mf.Artifact(
            path=Path("/nonexistent.zip"), slug="s", version="0.1.0", fmt="vst3",
            os_id="macos", arch="universal", subpath="VST3/tatsunari-sounds",
            sha256="c" * 64, size=1,
        )
        with self.assertRaises(promote.PromoteError):
            promote.read_back_verify(store, [art], lambda _m: None)


class CachePolicyTest(unittest.TestCase):
    def setUp(self):
        self._orig = promote.make_store
        self.addCleanup(lambda: setattr(promote, "make_store", self._orig))

    def test_artifacts_immutable_pointer_revalidated(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            store, _ = run_publish(Path(td), ["tn-equalizer-v0_1_0-macOS-VST3.zip"])
        art_key = "artifacts/tn-equalizer/0.1.0/tn-equalizer-v0_1_0-macOS-VST3.zip"
        self.assertIn("immutable", store.meta[art_key]["cacheControl"])
        for key in (promote.LATEST_KEY, promote.CATALOG_KEY):
            self.assertIn("must-revalidate", store.meta[key]["cacheControl"],
                          f"{key} must be revalidated; it is the mutable pointer")


class PointerSwitchTest(unittest.TestCase):
    def setUp(self):
        self._orig = promote.make_store
        self.addCleanup(lambda: setattr(promote, "make_store", self._orig))

    def test_previous_pointer_is_archived_before_replacement(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            store, _ = run_publish(tmp / "a", ["tn-equalizer-v0_1_0-macOS-VST3.zip"])
            first = store.get(promote.LATEST_KEY)
            history = store.list_prefix(promote.HISTORY_PREFIX)
            self.assertEqual(history, [], "first publish has nothing to archive")

            run_publish(tmp / "b", ["tn-equalizer-v0_2_0-macOS-VST3.zip"], store=store)
            history = [k for k in store.list_prefix(promote.HISTORY_PREFIX)
                       if k.endswith(".json")]
            self.assertEqual(len(history), 1, "the live pointer must be archived")
            self.assertEqual(store.get(history[0]), first,
                             "the archive must hold the pointer that was live")
            self.assertNotEqual(store.get(promote.LATEST_KEY), first,
                                "the live pointer must have moved on")


class RollbackTest(unittest.TestCase):
    def setUp(self):
        self._orig = promote.make_store
        self.addCleanup(lambda: setattr(promote, "make_store", self._orig))

    def _two_publishes(self, tmp):
        store, _ = run_publish(tmp / "a", ["tn-equalizer-v0_1_0-macOS-VST3.zip"])
        v1 = store.get(promote.LATEST_KEY)
        run_publish(tmp / "b", ["tn-equalizer-v0_2_0-macOS-VST3.zip"], store=store)
        return store, v1

    def test_rollback_restores_the_previous_pointer(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            store, v1 = self._two_publishes(tmp)
            promote.make_store = lambda _a, _s=store: _s
            rc = promote.cmd_rollback(_Args(to="previous", yes=True))
            self.assertEqual(rc, 0)
            self.assertEqual(store.get(promote.LATEST_KEY), v1)

    def test_rollback_refuses_without_yes(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            store, _ = self._two_publishes(tmp)
            promote.make_store = lambda _a, _s=store: _s
            with self.assertRaises(promote.PromoteError) as ctx:
                promote.cmd_rollback(_Args(to="previous", yes=False))
            self.assertIn("--yes", str(ctx.exception))

    def test_rollback_with_no_history_is_refused(self):
        store = MemoryStore()
        promote.make_store = lambda _a, _s=store: _s
        with self.assertRaises(promote.PromoteError) as ctx:
            promote.cmd_rollback(_Args(to="previous", yes=True))
        self.assertIn("no archived pointer", str(ctx.exception))

    def test_rollback_archives_what_it_replaces(self):
        """A rollback is itself reversible: roll forward again if it was wrong."""
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            store, _ = self._two_publishes(tmp)
            v2 = store.get(promote.LATEST_KEY)
            promote.make_store = lambda _a, _s=store: _s
            promote.cmd_rollback(_Args(to="previous", yes=True))
            archived = [store.get(k) for k in store.list_prefix(promote.HISTORY_PREFIX)
                        if k.endswith(".json")]
            self.assertIn(v2, archived)


class ManifestShapeTest(unittest.TestCase):
    """The generated documents must be what the CLIENT accepts, not merely valid JSON."""

    def setUp(self):
        self._orig = promote.make_store
        self.addCleanup(lambda: setattr(promote, "make_store", self._orig))
        import tempfile
        self._td = tempfile.TemporaryDirectory()
        self.addCleanup(self._td.cleanup)
        self.store, self.out = run_publish(Path(self._td.name), [
            "tn-equalizer-v0_1_0-macOS-AU.zip",
            "tn-equalizer-v0_1_0-macOS-VST3.zip",
            "tn-equalizer-v0_1_0-Windows.zip",
        ])
        self.latest = json.loads((self.out / "latest.json").read_text())
        self.catalog = json.loads((self.out / "catalog.json").read_text())

    def test_latest_matches_the_published_schema(self):
        schema = json.loads(
            (REPO_ROOT / "schemas/updates/v1/latest.schema.json").read_text())
        self.assertEqual(self.latest["schema"], schema["properties"]["schema"]["const"])
        for key in schema["required"]:
            self.assertIn(key, self.latest)
        import re
        pat = re.compile(schema["properties"]["plugins"]["items"]
                         ["properties"]["latest"]["pattern"])
        for p in self.latest["plugins"]:
            self.assertTrue(pat.match(p["latest"]), p)

    def test_assets_use_only_client_accepted_enums(self):
        for p in self.catalog["plugins"]:
            for v in p["versions"]:
                for a in v["assets"]:
                    self.assertIn(a["os"], CLIENT_OS, a)
                    self.assertIn(a["arch"], CLIENT_PLUGIN_ARCH,
                                  f"{a} — an unknown arch makes the client drop the asset")
                    self.assertIn(a["format"], CLIENT_FORMATS, a)

    def test_subpaths_match_the_installer_placement_rules(self):
        """§11.4: VST3/CLAP get the vendor folder, AU never does."""
        for p in self.catalog["plugins"]:
            for v in p["versions"]:
                for a in v["assets"]:
                    if a["format"] == "au":
                        self.assertEqual(a["subpath"], "Components", a)
                    else:
                        self.assertTrue(a["subpath"].endswith("/tatsunari-sounds"), a)

    def test_every_url_is_on_the_baked_in_host_and_prefix(self):
        for p in self.catalog["plugins"]:
            for v in p["versions"]:
                for a in v["assets"]:
                    self.assertTrue(a["url"].startswith(mf.ARTIFACT_BASE + "/"), a)

    def test_bundle_basename_is_the_slug(self):
        """§11.1 froze the bundle basename to the slug, not the display name."""
        for p in self.catalog["plugins"]:
            for v in p["versions"]:
                for a in v["assets"]:
                    self.assertTrue(a["bundleName"].startswith(p["slug"] + "."), a)

    def test_plugin_ids_come_from_the_identity_gate(self):
        for p in self.catalog["plugins"]:
            self.assertEqual(p["pluginIds"]["clapId"],
                             f"jp.tatsunari-sounds.{p['slug']}")
            self.assertEqual(p["pluginIds"]["auManufacturer"], "Ttsn")

    def test_latest_and_catalog_agree(self):
        a = {p["slug"]: p["latest"] for p in self.latest["plugins"]}
        b = {p["slug"]: p["latest"] for p in self.catalog["plugins"]}
        self.assertEqual(a, b)


class CarryOverTest(unittest.TestCase):
    def setUp(self):
        self._orig = promote.make_store
        self.addCleanup(lambda: setattr(promote, "make_store", self._orig))

    def test_previous_versions_are_not_retracted(self):
        """Publishing 0.2.0 must not remove 0.1.0 from the catalog: someone has it."""
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            store, _ = run_publish(tmp / "a", ["tn-equalizer-v0_1_0-macOS-VST3.zip"])
            _, out2 = run_publish(tmp / "b", ["tn-equalizer-v0_2_0-macOS-VST3.zip"],
                                  store=store)
            catalog = json.loads((out2 / "catalog.json").read_text())
            versions = [v["version"] for v in catalog["plugins"][0]["versions"]]
            self.assertEqual(versions, ["0.1.0", "0.2.0"])
            self.assertEqual(catalog["plugins"][0]["latest"], "0.2.0")


class ValidationTest(unittest.TestCase):
    def test_unrecognised_file_in_artifacts_dir_is_refused(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            art = Path(td) / "art"
            write_zips(art, ["tn-equalizer-v0_1_0-macOS-VST3.zip", "notes.txt"])
            with self.assertRaises(mf.ManifestError) as ctx:
                mf.collect_artifacts(art, sha256_file)
            self.assertIn("not release zips", str(ctx.exception))

    def test_empty_artifacts_dir_is_refused(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            art = Path(td) / "art"
            art.mkdir()
            with self.assertRaises(mf.ManifestError):
                mf.collect_artifacts(art, sha256_file)

    def test_mixed_versions_for_one_plugin_are_refused(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            art = Path(td) / "art"
            write_zips(art, ["tn-equalizer-v0_1_0-macOS-VST3.zip",
                             "tn-equalizer-v0_2_0-macOS-AU.zip"])
            artifacts = mf.collect_artifacts(art, sha256_file)
            meta = mf.load_plugin_meta()
            with self.assertRaises(mf.ManifestError) as ctx:
                mf.build_latest(artifacts, meta)
            self.assertIn("disagree on the version", str(ctx.exception))

    def test_unknown_plugin_is_refused(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            art = Path(td) / "art"
            write_zips(art, ["not-a-real-plugin-v0_1_0-macOS-VST3.zip"])
            artifacts = mf.collect_artifacts(art, sha256_file)
            meta = mf.load_plugin_meta()
            with self.assertRaises(mf.ManifestError) as ctx:
                mf.build_latest(artifacts, meta)
            self.assertIn("unknown plugin", str(ctx.exception))

    def test_zip_name_parsing(self):
        self.assertEqual(mf.parse_zip_name("tn-vocal-tuner-v1_2_3-Windows.zip"),
                         ("tn-vocal-tuner", "1.2.3", "Windows"))
        self.assertIsNone(mf.parse_zip_name("tn-vocal-tuner-1.2.3-Windows.zip"))
        self.assertIsNone(mf.parse_zip_name("catalog.json"))


class SigningWiringTest(unittest.TestCase):
    """The key itself is a human's job; these cover the code path around it."""

    def test_sign_without_the_secret_is_a_hard_failure(self):
        import os
        os.environ.pop("TEST_MINISIGN_KEY", None)
        with self.assertRaises(signing.SigningError) as ctx:
            signing.Signer.from_args(sign=True, secret_key_env="TEST_MINISIGN_KEY",
                                     public_key="RWQ...")
        self.assertIn("refusing to publish unsigned", str(ctx.exception).lower())

    def test_sign_without_a_public_key_is_a_hard_failure(self):
        import os
        os.environ["TEST_MINISIGN_KEY"] = "not-a-real-key"
        self.addCleanup(lambda: os.environ.pop("TEST_MINISIGN_KEY", None))
        with self.assertRaises(signing.SigningError) as ctx:
            signing.Signer.from_args(sign=True, secret_key_env="TEST_MINISIGN_KEY",
                                     public_key=None)
        self.assertIn("verified", str(ctx.exception))

    def test_disabled_signer_is_explicit_about_why(self):
        s = signing.Signer.from_args(sign=False, secret_key_env="X", public_key=None)
        self.assertFalse(s.enabled)
        self.assertIn("rehearsal", s.describe())
        self.assertIsNone(s.sign(b"x"))
        self.assertFalse(s.verify(b"x", b"y"))


if __name__ == "__main__":
    unittest.main()
