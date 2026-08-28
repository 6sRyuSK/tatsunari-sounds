"""minisign wiring for the promote pipeline (plan §1.3 / §5).

This module is the code path only. It never generates, prints or persists a
secret key: key generation, storage, the out-of-band publication of the public
key, and the active→backup rotation are human steps, written up in
`docs/runbooks/production-rollout.md`.

Two design points worth keeping:

  * **Signing without verification is theatre.** `Signer.verify` runs the
    produced signature back through the PUBLIC key, and the promote's smoke step
    refuses to publish when that fails. A signature nobody can verify is worse
    than none: clients reject it and the release looks corrupt.
  * **The unsigned path is explicit, never silent.** `Signer.disabled(reason)`
    carries why, and every log line and the promote result JSON say so. A
    rehearsal that quietly produced unsigned manifests would be mistaken for a
    signed one.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


class SigningError(RuntimeError):
    pass


@dataclass
class Signer:
    enabled: bool
    reason: str = ""
    _secret_key: str | None = None
    _public_key: str | None = None
    binary: str = "minisign"

    # ── construction ─────────────────────────────────────────────────────────

    @classmethod
    def disabled(cls, reason: str) -> "Signer":
        return cls(enabled=False, reason=reason)

    @classmethod
    def from_args(cls, *, sign: bool, secret_key_env: str,
                  public_key: str | None) -> "Signer":
        if not sign:
            return cls.disabled("--sign not requested (rehearsal)")

        secret = os.environ.get(secret_key_env)
        if not secret:
            # Hard failure, not a downgrade to unsigned: someone asked for a
            # signed promote and must not get an unsigned one instead.
            raise SigningError(
                f"--sign was requested but {secret_key_env} is empty. "
                "The production key lives in the workflow's Environment secrets; "
                "refusing to publish unsigned manifests under a signed promote."
            )
        if not public_key:
            raise SigningError(
                "--sign requires --public-key so the signature can be verified "
                "before the pointer switches. Signing with an unverifiable key "
                "ships a release every client rejects."
            )
        if shutil.which(cls.binary) is None:
            raise SigningError(
                f"{cls.binary!r} not found on PATH; the promote workflow installs it."
            )
        return cls(enabled=True, reason="", _secret_key=secret, _public_key=public_key)

    # ── operations ───────────────────────────────────────────────────────────

    def describe(self) -> str:
        if not self.enabled:
            return f"signing DISABLED — {self.reason}"
        return "signing enabled (minisign; secret key from the environment, never logged)"

    def sign(self, data: bytes) -> bytes | None:
        """Detached signature over exactly the bytes that get uploaded."""
        if not self.enabled:
            return None
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            payload, seckey = tmp / "payload", tmp / "minisign.key"
            payload.write_bytes(data)
            seckey.write_text(self._secret_key or "", encoding="utf-8")
            seckey.chmod(0o600)
            res = subprocess.run(
                [self.binary, "-S", "-s", str(seckey), "-m", str(payload),
                 "-x", str(tmp / "payload.minisig")],
                capture_output=True, text=True,
                # minisign reads the key password from stdin; the CI key is
                # created without one, so feed an empty line rather than
                # letting it block forever on a non-tty.
                input="\n",
            )
            if res.returncode != 0:
                # stderr can echo the key path but never the key material.
                raise SigningError(f"minisign signing failed: {res.stderr.strip()}")
            return (tmp / "payload.minisig").read_bytes()

    def verify(self, data: bytes, signature: bytes) -> bool:
        """Verify a signature against the PUBLIC key. False, never an exception."""
        if not self.enabled or not self._public_key:
            return False
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            payload, sig = tmp / "payload", tmp / "payload.minisig"
            payload.write_bytes(data)
            sig.write_bytes(signature)
            res = subprocess.run(
                [self.binary, "-V", "-P", self._public_key,
                 "-m", str(payload), "-x", str(sig)],
                capture_output=True, text=True,
            )
            return res.returncode == 0
