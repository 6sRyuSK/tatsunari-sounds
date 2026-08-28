"""Object-store backends for the promote pipeline (plan §5 "promote").

The promote pipeline never talks to a cloud SDK directly. It goes through this
tiny interface so the whole eight-step sequence — including the parts that must
NOT be skipped, like read-back verification and the refusal to overwrite an
existing immutable key — is exercised headlessly against `MemoryStore` in CI,
and only the transport differs in production.

Why the S3 backend shells out to the `aws` CLI rather than signing requests
itself: R2 is S3-compatible, and a hand-rolled SigV4 implementation is exactly
the kind of code that looks right, passes its own unit tests, and then fails
against the real service in a way nobody can debug during a release. The CLI is
already exercised by everyone else's releases. The runbook pins its version.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Protocol


@dataclass(frozen=True)
class ObjectInfo:
    """What the store knows about a key that already exists."""

    sha256: str
    size: int


# Cache policy per plan §6.1 / §5 "CDN header": artifacts are immutable and may
# be cached forever; the pointer manifests are mutable and must be revalidated.
IMMUTABLE_CACHE_CONTROL = "public, max-age=31536000, immutable"
POINTER_CACHE_CONTROL = "public, max-age=60, must-revalidate"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _is_not_found(stderr: str) -> bool:
    """True only for an unambiguous "this key does not exist" from the CLI.

    Deliberately narrow. Everything this does NOT match — credentials, network,
    throttling, 5xx — is a fault the promote must stop on, because the callers
    treat "absent" as permission to proceed.
    """
    text = stderr or ""
    return ("Not Found" in text
            or "NoSuchKey" in text
            or "404" in text
            or "does not exist" in text)


class ObjectStore(Protocol):
    """The surface promote needs. Deliberately smaller than any S3 API."""

    def head(self, key: str) -> Optional[ObjectInfo]:
        """Return info for key, or None when it does not exist."""

    def put(self, key: str, data: bytes, *, content_type: str, cache_control: str) -> None:
        """Create or replace key. Callers decide whether replacing is allowed."""

    def get(self, key: str) -> bytes:
        """Read key back. Raises KeyError when missing."""

    def list_prefix(self, prefix: str) -> list[str]:
        """Every key under prefix, sorted."""


class MemoryStore:
    """In-process store used by the tests and by `--store memory` rehearsals.

    It records the metadata promote sets (content type, cache control) so tests
    can assert the CDN policy is applied at upload time rather than hoping a
    later header check catches it.
    """

    def __init__(self) -> None:
        self.objects: dict[str, bytes] = {}
        self.meta: dict[str, dict[str, str]] = {}

    def head(self, key: str) -> Optional[ObjectInfo]:
        data = self.objects.get(key)
        if data is None:
            return None
        return ObjectInfo(sha256=sha256_bytes(data), size=len(data))

    def put(self, key: str, data: bytes, *, content_type: str, cache_control: str) -> None:
        self.objects[key] = data
        self.meta[key] = {"contentType": content_type, "cacheControl": cache_control}

    def get(self, key: str) -> bytes:
        try:
            return self.objects[key]
        except KeyError:
            raise KeyError(key) from None

    def list_prefix(self, prefix: str) -> list[str]:
        return sorted(k for k in self.objects if k.startswith(prefix))


class S3Store:
    """R2 (or any S3-compatible bucket) via the `aws` CLI.

    Credentials come from the environment the workflow provides
    (AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY); this class never reads or logs
    them. `endpoint_url` is the R2 S3 endpoint — the R2 *development* URL is
    deliberately never used as a public URL (plan §6.1), and it is not the CDN
    host either: what the client fetches is always https://6sryusk.com/…
    """

    def __init__(self, bucket: str, endpoint_url: str, *, cli: str = "aws") -> None:
        self.bucket = bucket
        self.endpoint_url = endpoint_url
        self.cli = cli
        if shutil.which(cli) is None:
            raise RuntimeError(
                f"{cli!r} not found on PATH; the promote workflow installs it. "
                "Refusing to guess an alternative transport."
            )

    def _run(self, *args: str, check: bool = True) -> subprocess.CompletedProcess:
        cmd = [self.cli, "s3api", *args, "--endpoint-url", self.endpoint_url]
        return subprocess.run(cmd, capture_output=True, text=True, check=check)

    def head(self, key: str) -> Optional[ObjectInfo]:
        res = self._run("head-object", "--bucket", self.bucket, "--key", key, check=False)
        if res.returncode != 0:
            # 404 is the expected "not there yet"; anything else is a real fault
            # and must not be mistaken for an absent object.
            if _is_not_found(res.stderr):
                return None
            raise RuntimeError(f"head-object {key!r} failed: {res.stderr.strip()}")
        meta = json.loads(res.stdout)
        # We store our own digest in metadata: S3 ETag is not a sha256 for
        # multipart uploads, so it cannot be used for the read-back check.
        digest = (meta.get("Metadata") or {}).get("sha256", "")
        return ObjectInfo(sha256=digest, size=int(meta.get("ContentLength", 0)))

    def put(self, key: str, data: bytes, *, content_type: str, cache_control: str) -> None:
        with tempfile.NamedTemporaryFile(delete=False) as tmp:
            tmp.write(data)
            tmp_path = tmp.name
        try:
            self._run(
                "put-object",
                "--bucket", self.bucket,
                "--key", key,
                "--body", tmp_path,
                "--content-type", content_type,
                "--cache-control", cache_control,
                "--metadata", f"sha256={sha256_bytes(data)}",
            )
        finally:
            os.unlink(tmp_path)

    def get(self, key: str) -> bytes:
        with tempfile.NamedTemporaryFile(delete=False) as tmp:
            tmp_path = tmp.name
        try:
            res = self._run("get-object", "--bucket", self.bucket, "--key", key, tmp_path,
                            check=False)
            if res.returncode != 0:
                # Only a genuine 404 may become KeyError. Callers read KeyError
                # as "this object does not exist yet" and act on it —
                # archive_current_pointer() reads it as "first publish" and
                # skips archiving. Turning an auth error, a timeout or a 5xx
                # into the same signal means a transient read fault followed by
                # a successful write silently destroys the rollback target.
                if _is_not_found(res.stderr):
                    raise KeyError(key)
                raise RuntimeError(
                    f"get-object {key!r} failed and it is NOT a 404: "
                    f"{res.stderr.strip()}. Refusing to treat an unreadable "
                    "object as an absent one."
                )
            return Path(tmp_path).read_bytes()
        finally:
            os.unlink(tmp_path)

    def list_prefix(self, prefix: str) -> list[str]:
        res = self._run("list-objects-v2", "--bucket", self.bucket, "--prefix", prefix)
        payload = json.loads(res.stdout or "{}")
        return sorted(obj["Key"] for obj in payload.get("Contents", []))
