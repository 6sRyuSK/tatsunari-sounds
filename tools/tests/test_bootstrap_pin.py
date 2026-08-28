"""The bootstrap shim must pin the payload it actually ships (plan §5).

The shim is the one object in the distribution chain that cannot be verified by
signature — it is what fetches the key material's transport in the first place.
Its whole value is that it is short enough to read AND that it refuses to run a
payload whose SHA-256 it does not recognise. A pin that has drifted from the
committed payload destroys both halves: either the published bootstrap stops
working, or somebody "fixes" it by loosening the check.
"""

import subprocess
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS / "promote"))

import bootstrap as bs  # noqa: E402


class PinTest(unittest.TestCase):
    def test_committed_pins_match_the_committed_payloads(self):
        self.assertEqual(bs.check(), 0,
                         "run: python tools/promote/bootstrap.py --write")

    def test_check_is_also_reachable_as_a_script(self):
        res = subprocess.run(
            [sys.executable, str(TOOLS / "promote" / "bootstrap.py"), "--check"],
            capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_a_tampered_payload_is_detected(self):
        payload = bs.BOOTSTRAP_DIR / "install.sh"
        original = payload.read_bytes()
        try:
            payload.write_bytes(original + b"\n# tampered\n")
            self.assertEqual(bs.check(), 1,
                             "a changed payload must invalidate the pin")
        finally:
            payload.write_bytes(original)
        self.assertEqual(bs.check(), 0, "the fixture must restore cleanly")


class ShimContentTest(unittest.TestCase):
    """Properties of the shim that make the pin meaningful."""

    def test_shim_never_follows_redirects(self):
        sh = (bs.BOOTSTRAP_DIR / "shim.sh").read_text()
        self.assertNotIn("-fsSL", sh,
                         "-L follows redirects; a bootstrap must not change origin silently")
        self.assertIn("--proto '=https'", sh)
        ps1 = (bs.BOOTSTRAP_DIR / "shim.ps1").read_text()
        self.assertIn("-MaximumRedirection 0", ps1)

    def test_payload_download_also_refuses_redirects(self):
        sh = (bs.BOOTSTRAP_DIR / "install.sh").read_text()
        self.assertNotIn("curl -fsSL", sh)
        ps1 = (bs.BOOTSTRAP_DIR / "install.ps1").read_text()
        self.assertIn("-MaximumRedirection 0", ps1)

    def test_shims_are_served_from_the_product_host(self):
        for name in ("shim.sh", "shim.ps1"):
            text = (bs.BOOTSTRAP_DIR / name).read_text()
            self.assertIn("https://6sryusk.com/tatsunarisounds", text)
            self.assertNotIn("raw.githubusercontent.com", text,
                             "plan §5.1 retires GitHub as a delivery path")

    def test_readme_one_liners_point_at_the_product_host(self):
        readme = (TOOLS.parent / "README.md").read_text()
        self.assertIn("https://6sryusk.com/tatsunarisounds/install.sh", readme)
        self.assertIn("https://6sryusk.com/tatsunarisounds/install.ps1", readme)
        self.assertNotIn("raw.githubusercontent.com/6sRyuSK/tatsunari-sounds/main/tools/installer/bootstrap",
                         readme, "plan §5.1 retires GitHub as a delivery path")


if __name__ == "__main__":
    unittest.main()
