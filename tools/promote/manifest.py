"""Build the updates/v1 documents promote publishes (plan §3 schema).

Two documents come out of a promote:

  * `latest.json`  — the small pointer the plugins poll (schemas/updates/v1/
    latest.schema.json). Slug -> newest stable version, plus optional
    highlights / changelog link.
  * `catalog.json` — the richer document the TUI installer reads: per plugin,
    every published version with its per-(format, os, arch) assets.

Everything here is derived from things that already exist in the repository —
`plugins/<slug>/plugin.toml` for identity and version, `check_plugin_ids.py` for
the CLAP id / AU subtype, and the release zips for sizes and digests. Nothing is
typed in twice: a manifest that disagrees with the binaries is the failure mode
this whole pipeline exists to prevent.
"""

from __future__ import annotations

import datetime as _dt
import re
import subprocess
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SCHEMA_VERSION = 1
BASE_URL = "https://6sryusk.com/tatsunarisounds"
ARTIFACT_BASE = f"{BASE_URL}/artifacts"
NOTES_BASE = f"{BASE_URL}/notes"

VENDOR = "Tatsunari Sounds"

# Release zip names, from release.yml's package step:
#   <slug>-v<major>_<minor>_<patch>-macOS-AU.zip
#   <slug>-v<major>_<minor>_<patch>-macOS-VST3.zip
#   <slug>-v<major>_<minor>_<patch>-Windows.zip
ZIP_RE = re.compile(
    r"^(?P<slug>[a-z0-9]+(?:-[a-z0-9]+)*)-v(?P<v>\d+_\d+_\d+)-"
    r"(?P<target>macOS-AU|macOS-VST3|Windows)\.zip$"
)

# (format, os, arch) per release target, plus the install subpath the installer
# would compose for it.
#
# Two values here are dictated by the CLIENT and must not be "tidied":
#   * subpath MUST match install.DefaultSubpath in the Go installer (plan §11.4)
#     — VST3/CLAP get the vendor folder, AU never does.
#   * arch for a PLUGIN asset must be one of universal / x86_64 / arm64. The
#     installer's own client assets use amd64 instead, and that asymmetry is
#     real: parse.go validates the two sets separately. Writing "amd64" on a
#     plugin asset makes the client drop it as an unknown arch, which is a
#     plugin that silently does not install.
TARGETS = {
    "macOS-AU":   ("au",   "macos",   "universal", "Components"),
    "macOS-VST3": ("vst3", "macos",   "universal", "VST3/tatsunari-sounds"),
    "Windows":    ("vst3", "windows", "x86_64",    "VST3/tatsunari-sounds"),
}

# The TUI installer's own binaries, from installer.yml's build matrix
# (plan §11.5). These are the `client.assets[]` of catalog.json and are what the
# bootstrap one-liners resolve the executable from — publish a catalog without
# them and `curl | sh` stops working on every platform.
#
# NOTE the arch vocabulary: a CLIENT asset uses amd64/arm64, while a PLUGIN
# asset uses universal/x86_64/arm64. parse.go validates the two sets
# separately; they are not interchangeable.
INSTALLER_RE = re.compile(
    r"^tatsunari-sounds-installer-(?P<os>darwin|windows)-(?P<arch>amd64|arm64)(?P<ext>\.exe)?$"
)
INSTALLER_OS = {"darwin": "macos", "windows": "windows"}

# Bundle extension per format, used to name the bundle inside the zip.
BUNDLE_EXT = {"vst3": ".vst3", "au": ".component", "clap": ".clap"}


class ManifestError(RuntimeError):
    pass


@dataclass
class Artifact:
    """One release zip that promote will publish."""

    path: Path
    slug: str
    version: str
    fmt: str
    os_id: str
    arch: str
    subpath: str
    sha256: str
    size: int

    @property
    def object_key(self) -> str:
        return f"artifacts/{self.slug}/{self.version}/{self.path.name}"

    @property
    def url(self) -> str:
        return f"{ARTIFACT_BASE}/{self.slug}/{self.version}/{self.path.name}"


@dataclass
class ClientAsset:
    """One installer binary that will appear in catalog.json's `client.assets`."""

    path: Path
    version: str
    os_id: str
    arch: str
    sha256: str
    size: int

    @property
    def object_key(self) -> str:
        return f"artifacts/installer/{self.version}/{self.path.name}"

    @property
    def url(self) -> str:
        return f"{ARTIFACT_BASE}/installer/{self.version}/{self.path.name}"


@dataclass
class PluginMeta:
    slug: str
    name: str
    category: str
    version: str
    clap_id: str
    au_subtype: str
    au_manufacturer: str
    state_compat_version: str = "1.0.0"
    highlights: list[str] = field(default_factory=list)


def parse_zip_name(name: str) -> tuple[str, str, str] | None:
    """(slug, version, target) for a release zip, or None when it is not one."""
    m = ZIP_RE.match(name)
    if not m:
        return None
    return m.group("slug"), m.group("v").replace("_", "."), m.group("target")


def bundle_name_for(plugin_name_or_slug: str, fmt: str) -> str:
    """The bundle basename inside the zip.

    Per plan §11.1 the bundle basename is the SLUG, not the display name, so
    this takes whatever the caller resolved the basename to be and only appends
    the format's extension.
    """
    return plugin_name_or_slug + BUNDLE_EXT[fmt]


def load_plugin_meta(repo_root: Path | None = None) -> dict[str, PluginMeta]:
    """Read identity + version for every active plugin from the repository."""
    root = repo_root or REPO_ROOT
    ids = _load_plugin_ids(root)

    out: dict[str, PluginMeta] = {}
    plugins_dir = root / "plugins"
    for toml_path in sorted(plugins_dir.glob("*/plugin.toml")):
        data = tomllib.loads(toml_path.read_text(encoding="utf-8"))["plugin"]
        slug = data["slug"]
        ident = ids.get(slug)
        if ident is None:
            raise ManifestError(
                f"{slug}: no CLAP id / AU subtype found. check_plugin_ids.py is the "
                "source of truth for identity; a plugin missing from it would ship "
                "a manifest the host cannot match to the binary."
            )
        out[slug] = PluginMeta(
            slug=slug,
            name=data["name"],
            category=data.get("category", ""),
            version=data["version"],
            clap_id=ident["clapId"],
            au_subtype=ident["auSubtype"],
            au_manufacturer=ident["auManufacturer"],
        )
    if not out:
        raise ManifestError(f"no plugins found under {plugins_dir}")
    return out


def _load_plugin_ids(root: Path) -> dict[str, dict[str, str]]:
    """Stable-variant identity per slug, from check_plugin_ids.py --emit."""
    script = root / "tools" / "check_plugin_ids.py"
    res = subprocess.run([sys.executable, str(script), "--emit"],
                         capture_output=True, text=True, check=True)
    ids: dict[str, dict[str, str]] = {}
    for line in res.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) != 5:
            continue
        slug, variant, clap_id, au_subtype, au_mfr = parts
        if variant != "stable":
            continue
        ids[slug] = {"clapId": clap_id, "auSubtype": au_subtype, "auManufacturer": au_mfr}
    return ids


def collect_artifacts(artifacts_dir: Path, sha256_file) -> list[Artifact]:
    """Every release zip under artifacts_dir, in a stable order.

    Files that do not match the release naming convention are REJECTED rather
    than skipped: a silently-ignored artifact is a plugin that quietly does not
    ship, which is exactly the class of failure a promote must not have.
    """
    if not artifacts_dir.is_dir():
        raise ManifestError(f"artifacts dir not found: {artifacts_dir}")

    artifacts: list[Artifact] = []
    unknown: list[str] = []
    for path in sorted(artifacts_dir.rglob("*")):
        if not path.is_file():
            continue
        parsed = parse_zip_name(path.name)
        if parsed is None:
            unknown.append(path.name)
            continue
        slug, version, target = parsed
        fmt, os_id, arch, subpath = TARGETS[target]
        artifacts.append(Artifact(
            path=path, slug=slug, version=version, fmt=fmt, os_id=os_id,
            arch=arch, subpath=subpath, sha256=sha256_file(path), size=path.stat().st_size,
        ))
    if unknown:
        raise ManifestError(
            "artifacts dir holds files that are not release zips: "
            + ", ".join(sorted(unknown))
            + ". Refusing to promote a set whose contents are not fully understood."
        )
    if not artifacts:
        raise ManifestError(f"no release zips found under {artifacts_dir}")
    return artifacts


def collect_installer_assets(installer_dir: Path, version: str,
                             sha256_file) -> list[ClientAsset]:
    """Every installer binary under installer_dir, in a stable order.

    Same refusal as collect_artifacts: an unrecognised file is an error, not a
    skip. A silently-dropped installer binary is a platform whose one-liner
    install stops working, and nothing downstream would notice.
    """
    if not installer_dir.is_dir():
        raise ManifestError(f"installer dir not found: {installer_dir}")

    assets: list[ClientAsset] = []
    unknown: list[str] = []
    for path in sorted(installer_dir.rglob("*")):
        if not path.is_file():
            continue
        m = INSTALLER_RE.match(path.name)
        if not m:
            unknown.append(path.name)
            continue
        assets.append(ClientAsset(
            path=path, version=version, os_id=INSTALLER_OS[m.group("os")],
            arch=m.group("arch"), sha256=sha256_file(path), size=path.stat().st_size,
        ))
    if unknown:
        raise ManifestError(
            "installer dir holds files that are not installer binaries: "
            + ", ".join(sorted(unknown))
            + f". Expected {INSTALLER_RE.pattern}."
        )
    if not assets:
        raise ManifestError(f"no installer binaries found under {installer_dir}")
    return assets


def build_client(assets: list[ClientAsset], *, version: str) -> dict:
    """The catalog's `client` section (schemas/updates/v1/catalog.schema.json)."""
    return {
        "latest": version,
        "changelogUrl": f"{NOTES_BASE}/installer/{version}.md",
        "assets": [
            {
                "os": a.os_id,
                "arch": a.arch,
                "url": a.url,
                "size": a.size,
                "sha256": a.sha256,
            }
            for a in sorted(assets, key=lambda x: (x.os_id, x.arch))
        ],
    }


def _now_iso() -> str:
    return _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z")


def build_latest(artifacts: list[Artifact], meta: dict[str, PluginMeta],
                 *, generated: str | None = None) -> dict:
    """The small pointer document the plugins poll."""
    slugs = sorted({a.slug for a in artifacts})
    plugins = []
    for slug in slugs:
        m = _require_meta(meta, slug)
        version = _version_of(artifacts, slug)
        entry = {
            "slug": slug,
            "latest": version,
            "changelogUrl": f"{NOTES_BASE}/{slug}/{version}.md",
        }
        if m.highlights:
            entry["highlights"] = list(m.highlights)
        plugins.append(entry)
    return {
        "schema": SCHEMA_VERSION,
        "generated": generated or _now_iso(),
        "plugins": plugins,
    }


def build_catalog(artifacts: list[Artifact], meta: dict[str, PluginMeta],
                  *, channel: str = "stable", generated: str | None = None,
                  previous: dict | None = None, client: dict | None = None) -> dict:
    """The richer document the TUI installer reads.

    `previous` is the currently-published catalog. Two things are CARRIED OVER
    from it, for the same reason: a promote publishes what it was given, it does
    not retract what it was not given.

      * plugin VERSIONS already published — they are installed on people's
        machines (plan §8 "compatibility と rollback");
      * the `client` section — the installer binaries. This one is load-bearing
        in a way that is easy to miss: `tools/installer/bootstrap/install.sh`
        and `install.ps1` resolve the executable to download from
        `catalog.json`'s `client.assets[]`. Publishing a catalog without it
        breaks `curl | sh` and `irm | iex` on every platform at once, and
        nothing else in the pipeline would notice.
    """
    by_slug: dict[str, list[Artifact]] = {}
    for a in artifacts:
        by_slug.setdefault(a.slug, []).append(a)

    prev_versions = _previous_versions(previous)

    plugins = []
    for slug in sorted(by_slug):
        m = _require_meta(meta, slug)
        version = _version_of(artifacts, slug)
        assets = [
            {
                "format": a.fmt,
                "os": a.os_id,
                "arch": a.arch,
                "url": a.url,
                "size": a.size,
                "sha256": a.sha256,
                "subpath": a.subpath,
                "bundleName": bundle_name_for(slug, a.fmt),
            }
            for a in sorted(by_slug[slug], key=lambda x: (x.os_id, x.fmt, x.arch))
        ]
        versions = [v for v in prev_versions.get(slug, []) if v.get("version") != version]
        versions.append({
            "version": version,
            "channel": channel,
            "releasedAt": generated or _now_iso(),
            "stateCompatVersion": m.state_compat_version,
            "assets": assets,
        })
        versions.sort(key=lambda v: _semver_key(v.get("version", "0.0.0")))

        plugins.append({
            "slug": slug,
            "variant": channel if channel != "stable" else "stable",
            "name": {"en": m.name},
            "category": m.category,
            "vendor": VENDOR,
            "pluginIds": {
                "clapId": m.clap_id,
                "auSubtype": m.au_subtype,
                "auManufacturer": m.au_manufacturer,
            },
            "latest": version,
            "versions": versions,
        })

    doc = {
        "schema": SCHEMA_VERSION,
        "generated": generated or _now_iso(),
        "channels": [{"id": channel, "name": {"en": channel.capitalize()}}],
        "plugins": plugins,
    }

    resolved_client = client if client is not None else (previous or {}).get("client")
    if not resolved_client:
        raise ManifestError(
            "the catalog would have no `client` section, so the published "
            "one-liners could not resolve an installer to download and "
            "`curl | sh` / `irm | iex` would stop working. Pass "
            "--installer-dir with the built installer binaries, or promote "
            "against a catalog that already carries a client section."
        )
    doc["client"] = resolved_client
    return doc


def _require_meta(meta: dict[str, PluginMeta], slug: str) -> PluginMeta:
    m = meta.get(slug)
    if m is None:
        raise ManifestError(
            f"artifact for unknown plugin {slug!r}: it has no plugins/{slug}/plugin.toml. "
            "Publishing it would put a product in the catalog that the repository "
            "cannot identify."
        )
    return m


def _version_of(artifacts: list[Artifact], slug: str) -> str:
    versions = {a.version for a in artifacts if a.slug == slug}
    if len(versions) != 1:
        raise ManifestError(
            f"{slug}: artifacts disagree on the version ({sorted(versions)}). "
            "One promote publishes exactly one version per plugin."
        )
    return versions.pop()


def _previous_versions(previous: dict | None) -> dict[str, list[dict]]:
    if not previous:
        return {}
    out: dict[str, list[dict]] = {}
    for p in previous.get("plugins", []):
        slug = p.get("slug")
        if isinstance(slug, str):
            out[slug] = list(p.get("versions", []))
    return out


def _semver_key(v: str) -> tuple[int, int, int]:
    parts = v.split(".")
    try:
        return (int(parts[0]), int(parts[1]), int(parts[2]))
    except (IndexError, ValueError):
        return (0, 0, 0)
