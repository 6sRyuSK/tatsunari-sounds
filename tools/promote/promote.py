#!/usr/bin/env python3
"""Publish a release to the updates/v1 feed (plan §5 "promote", §8 Phase B/F).

The eight steps:

    validate → upload immutable assets → read-back verify → generate manifest
    → sign → smoke test → upload manifest → pointer switch

§5 writes the order as `… → upload manifest → smoke test → pointer switch`, and
smoke DOES still run after the catalog upload — but it also has to run before
it, which is why the numbering here puts it at 6. `latest.json` being switched
last protects the plugins, not the TUI installer: the installer reads
`catalog.json` DIRECTLY, so a catalog published before smoke is a catalog
already being served when smoke finds it broken. Nothing is published that has
not passed smoke first.

Four properties are load-bearing and are the reason this is a program rather
than a shell snippet:

  * **Immutable objects are never overwritten.** If an artifact key already
    exists, the promote succeeds only when the stored digest is identical;
    a different digest under the same key is a hard stop. Someone else's
    installed copy is already pinned to that URL.
  * **Read-back verification is not optional.** An upload that "succeeded" and
    a byte range that survived are different claims. Every artifact is read
    back and re-digested before it can appear in a manifest.
  * **The pointer switch is last and reversible.** The previous pointer is
    archived (with its signature) before the new one goes live, so `rollback`
    is a re-publish of a known-good signed document rather than a rebuild.
  * **Signatures are produced and verified before the first byte is
    published.** A document is never live without a signature that matches it,
    and a `minisign` failure costs nothing because it happens while nothing has
    been touched.

Modes:

    # rehearsal — no network, no credentials (what CI runs)
    promote.py publish --artifacts-dir DIR --out-dir DIR --store memory

    # production — requires R2 credentials in the environment and a
    # GitHub Environment approval on the workflow that calls it
    promote.py publish --artifacts-dir DIR --out-dir DIR --store s3 \\
        --bucket NAME --endpoint-url URL --sign

    promote.py rollback --store s3 --bucket NAME --endpoint-url URL \\
        --to updates/v1/history/latest-20260827T120000Z.json

`--store memory` is a full rehearsal: the same code path runs, the same
assertions fire, only the bytes land in a dict. That is what makes the Phase B
drills meaningful without production credentials.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import manifest as mf  # noqa: E402
import signing  # noqa: E402
from store import (  # noqa: E402
    IMMUTABLE_CACHE_CONTROL,
    POINTER_CACHE_CONTROL,
    MemoryStore,
    ObjectStore,
    S3Store,
    sha256_bytes,
    sha256_file,
)

LATEST_KEY = "updates/v1/latest.json"
CATALOG_KEY = "updates/v1/catalog.json"
HISTORY_PREFIX = "updates/v1/history/"

JSON_CONTENT_TYPE = "application/json"
SIG_CONTENT_TYPE = "text/plain"
ZIP_CONTENT_TYPE = "application/zip"
BINARY_CONTENT_TYPE = "application/octet-stream"


class PromoteError(RuntimeError):
    """A stop condition. Never caught internally: a half-done promote is worse
    than a refused one, and every raise site leaves the pointer untouched."""


# ── step 2/3: immutable artifact upload + read-back ───────────────────────────

def upload_immutable(store: ObjectStore, artifacts, log,
                     *, content_type: str = ZIP_CONTENT_TYPE) -> list[str]:
    """Upload each item under its content-addressed-by-convention key.

    Takes anything with `.object_key`, `.path` and `.sha256` — release zips and
    installer binaries go through the identical path, including the refusal to
    overwrite.

    Returns the keys that were newly written (an already-identical key is a
    no-op, which makes a re-run after a partial failure safe).
    """
    written: list[str] = []
    for a in artifacts:
        existing = store.head(a.object_key)
        if existing is not None:
            if existing.sha256 and existing.sha256 != a.sha256:
                raise PromoteError(
                    f"{a.object_key} already exists with a DIFFERENT digest "
                    f"(published {existing.sha256[:12]}…, local {a.sha256[:12]}…). "
                    "Immutable artifacts are never overwritten — someone's install "
                    "is pinned to this URL. Publish under a new version instead."
                )
            log(f"  skip (identical)   {a.object_key}")
            continue
        store.put(a.object_key, a.path.read_bytes(),
                  content_type=content_type, cache_control=IMMUTABLE_CACHE_CONTROL)
        written.append(a.object_key)
        log(f"  uploaded           {a.object_key}  sha256={a.sha256[:12]}…")
    return written


def read_back_verify(store: ObjectStore, artifacts, log) -> None:
    """Re-download every artifact and re-digest it.

    This is deliberately a full read, not a HEAD: the point is to prove the
    bytes a client will fetch hash to what the manifest is about to claim.
    """
    for a in artifacts:
        try:
            data = store.get(a.object_key)
        except KeyError:
            raise PromoteError(
                f"{a.object_key} is not readable back after upload. The manifest "
                "must not reference an object that cannot be fetched."
            ) from None
        got = sha256_bytes(data)
        if got != a.sha256:
            raise PromoteError(
                f"{a.object_key} read-back digest mismatch "
                f"(expected {a.sha256[:12]}…, got {got[:12]}…)."
            )
        log(f"  verified           {a.object_key}")


# ── step 8: archive-then-switch ───────────────────────────────────────────────

def archive_current_pointer(store: ObjectStore, stamp: str, log) -> str | None:
    """Copy the live pointer (and its signature) into history/ before replacing.

    Returns the history key, or None on a first publish. Rollback depends on
    this having happened, so a failure here stops the promote.
    """
    try:
        current = store.get(LATEST_KEY)
    except KeyError:
        log("  no live pointer yet (first publish); nothing to archive")
        return None

    history_key = f"{HISTORY_PREFIX}latest-{stamp}.json"
    store.put(history_key, current,
              content_type=JSON_CONTENT_TYPE, cache_control=IMMUTABLE_CACHE_CONTROL)
    log(f"  archived pointer   {history_key}")

    try:
        sig = store.get(LATEST_KEY + ".minisig")
    except KeyError:
        sig = None
    if sig is not None:
        store.put(history_key + ".minisig", sig,
                  content_type=SIG_CONTENT_TYPE, cache_control=IMMUTABLE_CACHE_CONTROL)
        log(f"  archived signature {history_key}.minisig")
    return history_key


def sign_document(signer: signing.Signer, key: str, data: bytes, log) -> bytes | None:
    """Produce AND verify a document's signature before anything is published.

    Signing has to happen before the first byte of the new document is live.
    Doing it the other way round has two failure modes, and the second one is
    permanent:

      * between the document PUT and the signature PUT, clients fetch the NEW
        document with the OLD signature and reject the release;
      * if `minisign` or the signature PUT then fails, that mismatch is not a
        window, it is the published state — a broken release with no operator
        action left to undo it except a rollback.

    So the signature is made and checked here, while nothing has been touched,
    and a failure at this point costs nothing.
    """
    if not signer.enabled:
        log(f"  signature          SKIPPED for {key} ({signer.reason})")
        return None
    sig = signer.sign(data)
    if sig is None or not signer.verify(data, sig):
        raise PromoteError(
            f"the signature for {key} does not verify against the public key. "
            "Publishing it would ship a release every client rejects. Nothing "
            "has been published."
        )
    log(f"  signed + verified  {key}.minisig")
    return sig


def publish_document(store: ObjectStore, key: str, data: bytes,
                     sig: bytes | None, log) -> None:
    """Upload a pointer document with the short-TTL policy.

    The SIGNATURE GOES FIRST. Two objects cannot be swapped atomically, so one
    ordering has to be chosen and defended: with the signature first, the
    document is never live without a signature that matches it, and the only
    remaining window is the reverse pair (old document + new signature), which a
    client sees as a failed verification and retries — the pointer is
    short-TTL precisely so that retry is cheap.
    """
    if sig is not None:
        store.put(key + ".minisig", sig, content_type=SIG_CONTENT_TYPE,
                  cache_control=POINTER_CACHE_CONTROL)
        log(f"  published          {key}.minisig")
    store.put(key, data, content_type=JSON_CONTENT_TYPE,
              cache_control=POINTER_CACHE_CONTROL)
    log(f"  published          {key}")


# ── smoke ─────────────────────────────────────────────────────────────────────

def smoke(store: ObjectStore, latest: dict, catalog: dict,
          artifacts, signatures: dict[str, bytes | None],
          signer: signing.Signer, log) -> None:
    """Prove the set is internally consistent.

    Run on the CANDIDATE bytes before either document is published, and again
    against the store afterwards. Running it only after the upload was the
    original mistake: `latest.json` would still point at the old version, but
    the TUI installer reads `catalog.json` DIRECTLY, so a catalog that failed
    smoke would already be the one being served. Nothing may be published that
    has not passed this first.

    Checks what a broken promote actually breaks: the two documents agree, every
    asset either names is present in the store with the digest it claims, the
    installer binaries the one-liners resolve are present, and a signature
    exists for every document when signing is on.
    """
    latest_versions = {p["slug"]: p["latest"] for p in latest["plugins"]}
    catalog_versions = {p["slug"]: p["latest"] for p in catalog["plugins"]}
    if latest_versions != catalog_versions:
        raise PromoteError(
            f"latest.json and catalog.json disagree: {latest_versions} vs {catalog_versions}"
        )

    by_key = {a.object_key: a for a in artifacts}
    for p in catalog["plugins"]:
        for v in p["versions"]:
            for asset in v["assets"]:
                key = _key_from_url(asset["url"])
                info = store.head(key)
                if info is None:
                    # Carried-over versions from a previous catalog live in the
                    # bucket already; a missing one means the catalog points at
                    # an object that was deleted out from under it.
                    raise PromoteError(
                        f"catalog references {key} but it is not in the store. "
                        "A published version must never lose its artifacts."
                    )
                if key in by_key and info.sha256 and info.sha256 != asset["sha256"]:
                    raise PromoteError(
                        f"catalog sha256 for {key} does not match the stored object."
                    )
    # The installer binaries the bootstrap one-liners resolve. A catalog whose
    # client assets are missing from the store is a `curl | sh` that 404s.
    client_assets = (catalog.get("client") or {}).get("assets") or []
    if not client_assets:
        raise PromoteError(
            "catalog has no client.assets — the published one-liners would have "
            "no installer to download."
        )
    for asset in client_assets:
        key = _key_from_url(asset["url"])
        if store.head(key) is None:
            raise PromoteError(
                f"catalog references installer binary {key} but it is not in the "
                "store. The one-liner install would 404."
            )

    log(f"  smoke              {len(catalog['plugins'])} plugin(s), "
        f"{len(client_assets)} client asset(s), documents consistent")

    if signer.enabled:
        missing = [k for k, v in signatures.items() if v is None]
        if missing:
            raise PromoteError(
                f"signing is on but no signature was produced for: {missing}. "
                "Publishing an unsigned document under a signed promote ships a "
                "release clients reject."
            )
        log("  smoke              every document has a verified signature")


def _key_from_url(url: str) -> str:
    prefix = mf.BASE_URL + "/"
    if not url.startswith(prefix):
        raise PromoteError(
            f"asset url {url!r} is not under {mf.BASE_URL}. Shipped binaries "
            "validate the host and path prefix, so an off-host url is unusable."
        )
    return url[len(prefix):]


def _canonical(doc: dict) -> bytes:
    """Byte form of a document: what we upload, digest and sign is identical."""
    return (json.dumps(doc, indent=2, ensure_ascii=False, sort_keys=False) + "\n").encode()


# ── commands ──────────────────────────────────────────────────────────────────

def make_store(args) -> ObjectStore:
    if args.store == "memory":
        return MemoryStore()
    if not args.bucket or not args.endpoint_url:
        raise PromoteError("--store s3 requires --bucket and --endpoint-url")
    return S3Store(args.bucket, args.endpoint_url)


def cmd_publish(args) -> int:
    log = _logger(args)
    store = make_store(args)
    signer = signing.Signer.from_args(sign=args.sign,
                                      secret_key_env=args.secret_key_env,
                                      public_key=args.public_key)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = _stamp()

    log("step 1/8 validate")
    meta = mf.load_plugin_meta()
    artifacts = mf.collect_artifacts(Path(args.artifacts_dir), sha256_file)
    for a in artifacts:
        log(f"  {a.slug} {a.version} {a.fmt}/{a.os_id}/{a.arch}  {a.size} B")

    client_assets: list[mf.ClientAsset] = []
    if args.installer_dir:
        if not args.installer_version:
            raise PromoteError("--installer-dir requires --installer-version")
        client_assets = mf.collect_installer_assets(
            Path(args.installer_dir), args.installer_version, sha256_file)
        for c in client_assets:
            log(f"  installer {c.version} {c.os_id}/{c.arch}  {c.size} B")

    log("step 2/8 upload immutable assets")
    upload_immutable(store, artifacts, log)
    if client_assets:
        upload_immutable(store, client_assets, log,
                         content_type=BINARY_CONTENT_TYPE)

    log("step 3/8 read-back verify")
    read_back_verify(store, artifacts, log)
    read_back_verify(store, client_assets, log)

    log("step 4/8 generate manifest")
    previous = _load_json(store, CATALOG_KEY)
    latest = mf.build_latest(artifacts, meta)
    client = (mf.build_client(client_assets, version=args.installer_version)
              if client_assets else None)
    catalog = mf.build_catalog(artifacts, meta, channel=args.channel,
                               previous=previous, client=client)
    latest_bytes, catalog_bytes = _canonical(latest), _canonical(catalog)
    (out_dir / "latest.json").write_bytes(latest_bytes)
    (out_dir / "catalog.json").write_bytes(catalog_bytes)
    log(f"  wrote {out_dir / 'latest.json'} and {out_dir / 'catalog.json'}")

    log("step 5/8 sign (before anything is published)")
    log(f"  {signer.describe()}")
    signatures = {
        CATALOG_KEY: sign_document(signer, CATALOG_KEY, catalog_bytes, log),
        LATEST_KEY: sign_document(signer, LATEST_KEY, latest_bytes, log),
    }

    log("step 6/8 smoke (on the candidate bytes — nothing is live yet)")
    smoke(store, latest, catalog, artifacts, signatures, signer, log)

    log("step 7/8 upload manifest (catalog first — the pointer is switched last)")
    publish_document(store, CATALOG_KEY, catalog_bytes, signatures[CATALOG_KEY], log)
    smoke(store, latest, catalog, artifacts, signatures, signer, log)

    log("step 8/8 pointer switch")
    history_key = archive_current_pointer(store, stamp, log)
    publish_document(store, LATEST_KEY, latest_bytes, signatures[LATEST_KEY], log)

    summary = {
        "channel": args.channel,
        "store": args.store,
        "stamp": stamp,
        "artifacts": [a.object_key for a in artifacts],
        "clientAssets": [c.object_key for c in client_assets],
        "previousPointer": history_key,
        "signed": signer.enabled,
    }
    (out_dir / "promote-result.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    log(f"done. rollback target: {history_key or '(none — first publish)'}")
    return 0


def cmd_rollback(args) -> int:
    """Re-publish a previously archived pointer.

    Rollback restores the POINTER only. Artifacts are immutable and stay where
    they are, and installed plugin bundles are never rolled back automatically
    (plan §8): a user who wants the older build picks it in the TUI.
    """
    log = _logger(args)
    store = make_store(args)
    signer = signing.Signer.from_args(sign=args.sign,
                                      secret_key_env=args.secret_key_env,
                                      public_key=args.public_key)

    target = args.to
    if target == "previous":
        history = [k for k in store.list_prefix(HISTORY_PREFIX)
                   if k.endswith(".json")]
        if not history:
            raise PromoteError(
                "no archived pointer to roll back to. A rollback can only restore a "
                "pointer a previous promote archived."
            )
        target = history[-1]
        log(f"resolved 'previous' to {target}")

    try:
        data = store.get(target)
    except KeyError:
        raise PromoteError(f"archived pointer not found: {target}") from None

    doc = json.loads(data)
    versions = {p.get("slug"): p.get("latest") for p in doc.get("plugins", [])}
    log(f"restoring pointer {target}: {versions}")

    if not args.yes:
        raise PromoteError(
            "refusing to switch the live pointer without --yes. "
            "Rollback is a production-visible action; run it deliberately."
        )

    sig = sign_document(signer, LATEST_KEY, data, log)
    archive_current_pointer(store, _stamp(), log)
    publish_document(store, LATEST_KEY, data, sig, log)
    log("rollback complete")
    return 0


def _load_json(store: ObjectStore, key: str) -> dict | None:
    try:
        return json.loads(store.get(key))
    except KeyError:
        return None
    except json.JSONDecodeError as exc:
        raise PromoteError(f"published {key} is not valid JSON: {exc}") from None


def _stamp() -> str:
    import datetime as dt
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def _logger(args):
    def log(msg: str) -> None:
        if not getattr(args, "quiet", False):
            print(msg)
    return log


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)

    def add_common(p):
        p.add_argument("--store", choices=["memory", "s3"], default="memory",
                       help="memory = full rehearsal, no network (default)")
        p.add_argument("--bucket", help="R2/S3 bucket (--store s3)")
        p.add_argument("--endpoint-url", help="R2 S3 endpoint (--store s3)")
        p.add_argument("--sign", action="store_true",
                       help="sign the manifests with minisign (needs the secret key env)")
        p.add_argument("--secret-key-env", default="MINISIGN_SECRET_KEY",
                       help="env var holding the minisign secret key (never logged)")
        p.add_argument("--public-key",
                       help="minisign public key used to verify what we just signed")
        p.add_argument("--quiet", action="store_true")

    p_pub = sub.add_parser("publish", help="run the eight-step promote")
    add_common(p_pub)
    p_pub.add_argument("--artifacts-dir", required=True)
    p_pub.add_argument("--out-dir", required=True)
    p_pub.add_argument("--channel", default="stable")
    p_pub.add_argument("--installer-dir",
                       help="directory of built installer binaries "
                            "(tatsunari-sounds-installer-<os>-<arch>[.exe]); they become "
                            "catalog.json's client.assets, which is what the bootstrap "
                            "one-liners resolve the executable from. Omit only when the "
                            "published catalog already carries a client section to carry over")
    p_pub.add_argument("--installer-version",
                       help="version for --installer-dir (its immutable path)")
    p_pub.set_defaults(func=cmd_publish)

    p_rb = sub.add_parser("rollback", help="restore a previously archived pointer")
    add_common(p_rb)
    p_rb.add_argument("--to", default="previous",
                      help="history key, or 'previous' for the most recent archive")
    p_rb.add_argument("--yes", action="store_true",
                      help="required: rollback switches the live pointer")
    p_rb.set_defaults(func=cmd_rollback)
    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (PromoteError, mf.ManifestError, signing.SigningError) as exc:
        print(f"promote: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
