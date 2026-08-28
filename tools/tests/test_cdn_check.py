"""Unit tests for tools/promote/cdn_check.py (stdlib only).

Stated as the outage each check prevents:

  * a pointer cached long (or marked immutable) makes a rollback a no-op for as
    long as the cache lives — the check must catch it even when the header
    "looks fine";
  * an artifact that is not immutable makes every install re-download;
  * a URL outside the product host is one shipped clients refuse outright.

Everything runs against a fake fetcher: this must be testable without the CDN,
or it will only ever be run by hand.
"""

import re
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS / "promote"))

import cdn_check as cc  # noqa: E402

BASE = cc.BASE_URL
GOOD_IMMUTABLE = {"Cache-Control": "public, max-age=31536000, immutable",
                  "ETag": '"abc"'}
GOOD_POINTER = {"Cache-Control": "public, max-age=60, must-revalidate",
                "ETag": '"abc"'}

ARTIFACT = f"{BASE}/artifacts/tn-equalizer/0.1.0/tn-equalizer-v0_1_0-Windows.zip"
POINTER = f"{BASE}/updates/v1/latest.json"
SHIM = f"{BASE}/install.sh"
PAYLOAD = f"{BASE}/bootstrap/1/install.sh"


def check_one(url, headers, status=200):
    report = cc.Report()
    cc.check_headers(url, status, headers, report)
    return report


class ClassifyTests(unittest.TestCase):
    def test_each_published_path_shape_has_a_policy(self):
        cases = {
            ARTIFACT: ("artifact", cc.IMMUTABLE),
            POINTER: ("pointer", cc.MUTABLE),
            f"{BASE}/updates/v1/catalog.json.minisig": ("pointer-signature", cc.MUTABLE),
            f"{BASE}/updates/v1/history/latest-20260101T000000Z.json": ("history", cc.IMMUTABLE),
            PAYLOAD: ("bootstrap-payload", cc.IMMUTABLE),
            SHIM: ("bootstrap-shim", cc.MUTABLE),
            f"{BASE}/notes/tn-equalizer/0.1.0.md": ("release-notes", cc.MUTABLE),
        }
        for url, want in cases.items():
            with self.subTest(url=url):
                self.assertEqual(cc.classify(url), want)

    def test_the_shim_and_its_payload_get_opposite_policies(self):
        # They share a filename; getting this backwards publishes either a
        # permanently-cached entry point or a payload nobody can cache.
        self.assertEqual(cc.classify(SHIM)[1], cc.MUTABLE)
        self.assertEqual(cc.classify(PAYLOAD)[1], cc.IMMUTABLE)

    def test_a_url_off_the_product_host_is_rejected(self):
        with self.assertRaises(cc.CdnCheckError):
            cc.classify("https://pub-1234.r2.dev/artifacts/x/1.0.0/a.zip")

    def test_an_unclassified_path_is_rejected_rather_than_guessed(self):
        with self.assertRaises(cc.CdnCheckError):
            cc.classify(f"{BASE}/something/new.bin")


class ImmutablePolicyTests(unittest.TestCase):
    def test_a_correct_artifact_passes(self):
        self.assertTrue(check_one(ARTIFACT, GOOD_IMMUTABLE).ok)

    def test_artifact_without_immutable_fails(self):
        r = check_one(ARTIFACT, {"Cache-Control": "public, max-age=31536000"})
        self.assertFalse(r.ok)
        self.assertIn("immutable", r.findings[0].problem)

    def test_artifact_with_a_shortened_max_age_fails(self):
        # The edge rewriting our header is the condition worth reporting, even
        # when a day of caching would be "fine".
        r = check_one(ARTIFACT, {"Cache-Control": "public, max-age=86400, immutable"})
        self.assertFalse(r.ok)
        self.assertIn("max-age", r.findings[0].problem)

    def test_artifact_marked_no_store_fails(self):
        r = check_one(ARTIFACT,
                      {"Cache-Control": "public, max-age=31536000, immutable, no-store"})
        self.assertFalse(r.ok)
        self.assertTrue(any("no-store" in f.problem for f in r.findings))


class PointerPolicyTests(unittest.TestCase):
    def test_a_correct_pointer_passes(self):
        self.assertTrue(check_one(POINTER, GOOD_POINTER).ok)

    def test_an_immutable_pointer_fails(self):
        r = check_one(POINTER, {"Cache-Control": "public, max-age=60, immutable",
                                "ETag": '"a"'})
        self.assertFalse(r.ok)
        self.assertIn("rollback", r.findings[0].problem)

    def test_a_long_lived_pointer_fails(self):
        r = check_one(POINTER, {"Cache-Control": "public, max-age=86400", "ETag": '"a"'})
        self.assertFalse(r.ok)
        self.assertIn("max-age", r.findings[0].problem)

    def test_a_pointer_without_a_validator_fails(self):
        r = check_one(POINTER, {"Cache-Control": "public, max-age=60"})
        self.assertFalse(r.ok)
        self.assertIn("ETag", r.findings[0].problem)

    def test_last_modified_counts_as_a_validator(self):
        r = check_one(POINTER, {"Cache-Control": "public, max-age=60",
                                "Last-Modified": "Wed, 27 Aug 2026 00:00:00 GMT"})
        self.assertTrue(r.ok)

    def test_header_names_are_matched_case_insensitively(self):
        r = check_one(POINTER, {"cache-control": "public, max-age=60", "etag": '"a"'})
        self.assertTrue(r.ok)


class ResponseTests(unittest.TestCase):
    def test_a_missing_object_fails_regardless_of_headers(self):
        r = check_one(ARTIFACT, GOOD_IMMUTABLE, status=404)
        self.assertFalse(r.ok)
        self.assertIn("404", r.findings[0].problem)

    def test_no_cache_control_at_all_fails(self):
        r = check_one(ARTIFACT, {"ETag": '"a"'})
        self.assertFalse(r.ok)
        self.assertIn("no Cache-Control", r.findings[0].problem)


class UrlSetTests(unittest.TestCase):
    def test_documents_contribute_pointers_assets_and_changelogs(self):
        latest = {"plugins": [{"slug": "tn-equalizer", "latest": "0.1.0",
                               "changelogUrl": f"{BASE}/notes/tn-equalizer/0.1.0.md"}]}
        catalog = {"plugins": [{"slug": "tn-equalizer", "versions": [
            {"version": "0.1.0", "assets": [{"url": ARTIFACT}]}]}]}
        urls = cc.urls_from_documents(latest, catalog)
        self.assertIn(POINTER, urls)
        self.assertIn(f"{BASE}/updates/v1/catalog.json", urls)
        self.assertIn(ARTIFACT, urls)
        self.assertIn(f"{BASE}/notes/tn-equalizer/0.1.0.md", urls)
        self.assertEqual(len(urls), len(set(urls)), "urls must be de-duplicated")

    def test_installer_client_assets_are_checked_too(self):
        catalog = {"plugins": [], "client": {"assets": [
            {"url": f"{BASE}/artifacts/installer/1.0.0/tatsunari-sounds-installer-darwin-arm64"}]}}
        urls = cc.urls_from_documents({}, catalog)
        self.assertIn(f"{BASE}/artifacts/installer/1.0.0/tatsunari-sounds-installer-darwin-arm64", urls)

    def test_bootstrap_urls_track_the_committed_shim_pin(self):
        # The point of reading the shim is that the check fetches the payload a
        # shipped one-liner really would.
        urls = cc.bootstrap_urls()
        self.assertIn(SHIM, urls)
        self.assertIn(f"{BASE}/install.ps1", urls)
        payloads = [u for u in urls if "/bootstrap/" in u]
        self.assertEqual(len(payloads), 2)
        for u in payloads:
            self.assertEqual(cc.classify(u)[1], cc.IMMUTABLE)


class RunTests(unittest.TestCase):
    def test_run_check_reports_every_bad_url_not_just_the_first(self):
        responses = {
            ARTIFACT: (200, {"Cache-Control": "public, max-age=60"}),
            POINTER: (200, {"Cache-Control": "public, max-age=31536000, immutable"}),
            SHIM: (200, GOOD_POINTER),
        }
        report = cc.run_check(list(responses), fetch=lambda u: responses[u],
                              log=lambda *_: None)
        self.assertEqual(report.checked, 3)
        urls = {f.url for f in report.findings}
        self.assertEqual(urls, {ARTIFACT, POINTER})

    def test_a_transport_failure_is_a_finding_not_a_crash(self):
        def boom(url):
            raise cc.CdnCheckError("connection refused")
        report = cc.run_check([POINTER], fetch=boom, log=lambda *_: None)
        self.assertFalse(report.ok)
        self.assertIn("connection refused", report.findings[0].problem)


class WorkerPolicyDriftTests(unittest.TestCase):
    """The cache policy is stated in three languages; it must say one thing.

    promote (Python) writes it onto the object, the Worker (JS) writes it onto
    the response, and cdn_check (Python) asserts what the edge returns. Two of
    them agreeing while the third drifts is a policy that silently stops being
    enforced, so the strings are compared here rather than trusted.
    """

    WORKER = TOOLS / "worker" / "src" / "routing.js"

    def worker_source(self):
        self.assertTrue(self.WORKER.is_file(), f"missing {self.WORKER}")
        return self.WORKER.read_text(encoding="utf-8")

    def test_the_worker_publishes_the_same_two_cache_control_strings(self):
        import store

        src = self.worker_source()
        self.assertIn(f"IMMUTABLE_CACHE_CONTROL = '{store.IMMUTABLE_CACHE_CONTROL}'", src)
        self.assertIn(f"POINTER_CACHE_CONTROL = '{store.POINTER_CACHE_CONTROL}'", src)

    def test_what_the_worker_serves_is_what_this_checker_demands(self):
        # The checker's own thresholds must accept the header the Worker sets;
        # otherwise every production check fails on our own correct config.
        import store

        for url, cc in ((ARTIFACT, store.IMMUTABLE_CACHE_CONTROL),
                        (POINTER, store.POINTER_CACHE_CONTROL)):
            with self.subTest(url=url):
                r = check_one(url, {"Cache-Control": cc, "ETag": '"a"'})
                self.assertTrue(r.ok, [f.problem for f in r.findings])

    def test_every_route_the_worker_serves_is_one_this_checker_classifies(self):
        # A path the Worker serves but the checker cannot classify is a path
        # whose cache headers nobody verifies.
        src = self.worker_source()
        served = set(re.findall(r"\{ kind: '([a-z-]+)'", src))
        self.assertTrue(served, "could not read the Worker's route table")
        classified = {
            "plugin": ARTIFACT,
            "installer": f"{BASE}/artifacts/installer/1.2.0/tatsunari-sounds-installer-darwin-arm64",
            "pointer": POINTER,
            "history": f"{BASE}/updates/v1/history/latest-20260101T000000Z.json",
            "bootstrap-payload": PAYLOAD,
            "bootstrap-shim": SHIM,
            "notes": f"{BASE}/notes/tn-equalizer/0.1.0.md",
        }
        self.assertEqual(served, set(classified),
                         "the Worker's route table and this test have drifted")
        for kind, url in classified.items():
            with self.subTest(kind=kind):
                cc.classify(url)  # raises if unclassified


if __name__ == "__main__":
    unittest.main()
