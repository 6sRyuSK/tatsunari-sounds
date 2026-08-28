#!/usr/bin/env python3
"""Verify the CDN's cache headers match the distribution model (plan §5, §8).

The whole update story rests on ONE property of the edge: artifacts are
immutable and the pointers are not. Get that backwards and the two failure
modes are both silent and both bad:

  * a pointer served with a long TTL pins every client to a stale version, and
    a rollback (which is a pointer switch) does nothing for hours — the outage
    lasts as long as the cache does, and nothing in the pipeline reports it;
  * an artifact served with a short TTL is merely wasteful, but an artifact
    served `no-store` / `private` means every install re-downloads and the
    egress bill and the failure rate both climb.

Neither is visible from the publisher's side: promote sets `Cache-Control` on
the object, and the CDN is free to rewrite it (page rules, cache rules, a
proxied Worker that forgets to pass headers through). So the only honest check
is to fetch the published URLs and read what the edge actually returns. That is
what this does.

    # check the live feed
    python tools/promote/cdn_check.py --live

    # check the documents a promote just produced, before switching the pointer
    python tools/promote/cdn_check.py --from-dir out/

Exit status is 0 only when every checked URL passes. It is meant to be run as
one of the Phase B drills and after every production promote.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import manifest as mf  # noqa: E402

BASE_URL = mf.BASE_URL

# What promote writes (store.POINTER_CACHE_CONTROL / IMMUTABLE_CACHE_CONTROL).
# The check is against these exact intents rather than "something long enough":
# a CDN that rewrote our header is precisely the condition worth reporting, even
# when the rewrite happens to be harmless today.
IMMUTABLE_MIN_MAX_AGE = 31536000
POINTER_MAX_MAX_AGE = 300

# Path prefixes, in the order they are tested. Longest/most specific first —
# `/bootstrap/<v>/install.sh` is immutable while `/install.sh` is not, and they
# share a suffix.
MUTABLE = "mutable"
IMMUTABLE = "immutable"

RULES: list[tuple[str, re.Pattern[str], str]] = [
    ("artifact",          re.compile(r"^artifacts/.+"),                 IMMUTABLE),
    ("pointer",           re.compile(r"^updates/v1/(latest|catalog)\.json$"), MUTABLE),
    ("pointer-signature", re.compile(r"^updates/v1/(latest|catalog)\.json\.minisig$"), MUTABLE),
    ("history",           re.compile(r"^updates/v1/history/.+"),        IMMUTABLE),
    ("bootstrap-payload", re.compile(r"^bootstrap/[^/]+/install\.(sh|ps1)$"), IMMUTABLE),
    ("bootstrap-shim",    re.compile(r"^install\.(sh|ps1)$"),           MUTABLE),
    ("release-notes",     re.compile(r"^notes/.+"),                     MUTABLE),
]


class CdnCheckError(RuntimeError):
    pass


@dataclass
class Finding:
    url: str
    problem: str

    def __str__(self) -> str:  # pragma: no cover - formatting only
        return f"{self.url}\n    {self.problem}"


@dataclass
class Report:
    checked: int = 0
    findings: list[Finding] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.findings

    def fail(self, url: str, problem: str) -> None:
        self.findings.append(Finding(url, problem))


def key_for(url: str) -> str:
    """The object key a published URL maps to, or raise.

    Invariant 7: everything we ship points at `https://6sryusk.com/tatsunarisounds/`.
    A URL outside it in a published document is a finding in its own right — the
    shipped clients validate host AND path prefix, so they would refuse it.
    """
    prefix = BASE_URL + "/"
    if not url.startswith(prefix):
        raise CdnCheckError(
            f"{url!r} is not under {BASE_URL}. Shipped clients validate the host "
            "and the path prefix, so this URL is unusable by them."
        )
    return url[len(prefix):]


def classify(url: str) -> tuple[str, str]:
    """(kind, cache policy) for a published URL."""
    key = key_for(url)
    for kind, pattern, policy in RULES:
        if pattern.match(key):
            return kind, policy
    raise CdnCheckError(
        f"{url!r} does not match any known published path. Refusing to guess a "
        "cache policy for it: an unclassified object is one nobody decided the "
        "caching for."
    )


def parse_cache_control(value: str) -> dict[str, str | bool]:
    out: dict[str, str | bool] = {}
    for part in value.split(","):
        part = part.strip().lower()
        if not part:
            continue
        if "=" in part:
            k, _, v = part.partition("=")
            out[k.strip()] = v.strip().strip('"')
        else:
            out[part] = True
    return out


def _max_age(cc: dict[str, str | bool]) -> int | None:
    raw = cc.get("max-age")
    if not isinstance(raw, str):
        return None
    try:
        return int(raw)
    except ValueError:
        return None


def check_headers(url: str, status: int, headers: dict[str, str],
                  report: Report) -> None:
    """Apply the policy for `url` to one response."""
    report.checked += 1
    kind, policy = classify(url)

    if status != 200:
        report.fail(url, f"[{kind}] HTTP {status} — a published URL must be fetchable.")
        return

    lower = {k.lower(): v for k, v in headers.items()}
    raw_cc = lower.get("cache-control")
    if raw_cc is None:
        report.fail(url, f"[{kind}] no Cache-Control header. The edge is deciding "
                         "the TTL for us, which is exactly what this check exists "
                         "to prevent.")
        return
    cc = parse_cache_control(raw_cc)
    age = _max_age(cc)

    if policy == IMMUTABLE:
        if "immutable" not in cc:
            report.fail(url, f"[{kind}] Cache-Control {raw_cc!r} lacks `immutable`. "
                             "Artifacts are content-pinned; clients must never "
                             "revalidate them.")
        if age is None or age < IMMUTABLE_MIN_MAX_AGE:
            report.fail(url, f"[{kind}] Cache-Control {raw_cc!r} has max-age={age} "
                             f"(want >= {IMMUTABLE_MIN_MAX_AGE}). promote publishes "
                             "a year; a shorter value means the edge rewrote our "
                             "header.")
        for forbidden in ("no-store", "no-cache", "private"):
            if forbidden in cc:
                report.fail(url, f"[{kind}] Cache-Control {raw_cc!r} contains "
                                 f"`{forbidden}`, which defeats the immutable model.")
    else:
        if "immutable" in cc:
            report.fail(url, f"[{kind}] Cache-Control {raw_cc!r} says `immutable`. "
                             "This object is a POINTER: marking it immutable means a "
                             "rollback (which is a pointer switch) does nothing until "
                             "every cache expires.")
        if age is None:
            report.fail(url, f"[{kind}] Cache-Control {raw_cc!r} has no max-age.")
        elif age > POINTER_MAX_MAX_AGE:
            report.fail(url, f"[{kind}] Cache-Control {raw_cc!r} has max-age={age} "
                             f"(want <= {POINTER_MAX_MAX_AGE}). A stale pointer is a "
                             "rollback that has not taken effect.")
        if "etag" not in lower and "last-modified" not in lower:
            report.fail(url, f"[{kind}] no ETag (nor Last-Modified). Revalidation is "
                             "the other half of the short-TTL model; without a "
                             "validator every poll is a full re-download.")


# ── the URL set ───────────────────────────────────────────────────────────────

def bootstrap_urls(repo_root: Path | None = None) -> list[str]:
    """The two shims plus the versioned payloads they pin.

    Read from the committed shims rather than hardcoded: the version lives in
    exactly one place (tools/promote/bootstrap.py writes it) and this must check
    the payload a shipped one-liner will actually fetch.
    """
    root = repo_root or mf.REPO_ROOT
    d = root / "tools" / "installer" / "bootstrap"
    urls = [f"{BASE_URL}/install.sh", f"{BASE_URL}/install.ps1"]
    patterns = [
        (d / "shim.sh", re.compile(r'^BOOTSTRAP_VERSION="([^"]+)"$', re.M), "install.sh"),
        (d / "shim.ps1", re.compile(r"^\$BootstrapVersion = '([^']+)'$", re.M), "install.ps1"),
    ]
    for path, pattern, name in patterns:
        if not path.is_file():
            raise CdnCheckError(f"missing bootstrap shim: {path}")
        m = pattern.search(path.read_text(encoding="utf-8"))
        if not m:
            raise CdnCheckError(f"{path.name}: no generated version pin found")
        urls.append(f"{BASE_URL}/bootstrap/{m.group(1)}/{name}")
    return urls


def urls_from_documents(latest: dict, catalog: dict) -> list[str]:
    """Every URL the published documents tell a client to fetch."""
    urls = [f"{BASE_URL}/updates/v1/latest.json",
            f"{BASE_URL}/updates/v1/catalog.json"]
    for plugin in catalog.get("plugins", []):
        for version in plugin.get("versions", []):
            for asset in version.get("assets", []):
                url = asset.get("url")
                if isinstance(url, str):
                    urls.append(url)
    client = catalog.get("client")
    if isinstance(client, dict):
        for entry in client.get("assets", []):
            url = entry.get("url")
            if isinstance(url, str):
                urls.append(url)
    for plugin in latest.get("plugins", []):
        url = plugin.get("changelogUrl")
        if isinstance(url, str):
            urls.append(url)
    # Stable order, no duplicates — the report should be diffable run to run.
    seen: set[str] = set()
    out: list[str] = []
    for u in urls:
        if u not in seen:
            seen.add(u)
            out.append(u)
    return out


# ── transport ─────────────────────────────────────────────────────────────────

def http_head(url: str, timeout: float = 15.0) -> tuple[int, dict[str, str]]:
    """A real HEAD against the edge.

    HEAD rather than GET so a full check does not pull hundreds of megabytes of
    artifacts. Cloudflare serves HEAD from the same cache rules as GET; if that
    ever stops being true the check would need a `Range: bytes=0-0` GET instead.
    """
    req = urllib.request.Request(url, method="HEAD")
    req.add_header("User-Agent", "tatsunari-sounds-cdn-check")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, dict(resp.headers.items())
    except urllib.error.HTTPError as exc:
        return exc.code, dict(exc.headers.items()) if exc.headers else {}
    except urllib.error.URLError as exc:
        raise CdnCheckError(f"{url}: {exc.reason}") from None


def run_check(urls: list[str], fetch=http_head, log=print) -> Report:
    report = Report()
    for url in urls:
        try:
            status, headers = fetch(url)
        except CdnCheckError as exc:
            report.checked += 1
            report.fail(url, str(exc))
            continue
        check_headers(url, status, headers, report)
    log(f"checked {report.checked} URL(s), {len(report.findings)} problem(s)")
    for f in report.findings:
        log(f"  FAIL {f}")
    return report


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--live", action="store_true",
                     help="fetch latest.json/catalog.json from the CDN and check "
                          "everything they reference")
    src.add_argument("--from-dir",
                     help="use the latest.json/catalog.json a promote wrote here")
    ap.add_argument("--skip-artifacts", action="store_true",
                    help="check only the mutable objects (faster smoke)")
    args = ap.parse_args(argv)

    try:
        if args.from_dir:
            d = Path(args.from_dir)
            latest = json.loads((d / "latest.json").read_text(encoding="utf-8"))
            catalog = json.loads((d / "catalog.json").read_text(encoding="utf-8"))
        else:
            latest = _fetch_json(f"{BASE_URL}/updates/v1/latest.json")
            catalog = _fetch_json(f"{BASE_URL}/updates/v1/catalog.json")
        urls = urls_from_documents(latest, catalog) + bootstrap_urls()
        if args.skip_artifacts:
            urls = [u for u in urls if classify(u)[1] != IMMUTABLE]
    except CdnCheckError as exc:
        print(f"cdn_check: {exc}", file=sys.stderr)
        return 2

    return 0 if run_check(urls).ok else 1


def _fetch_json(url: str, timeout: float = 15.0) -> dict:
    req = urllib.request.Request(url)
    req.add_header("User-Agent", "tatsunari-sounds-cdn-check")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())
    except (urllib.error.URLError, json.JSONDecodeError) as exc:
        raise CdnCheckError(f"{url}: {exc}") from None


if __name__ == "__main__":
    raise SystemExit(main())
