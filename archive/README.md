# archive/ — archived plugins

Plugins retired from the factory during the clap-wrapper / Visage migration
(2026-07): their DSP quality did not meet the bar, so everything except
resonance-suppressor and dynamic-eq was archived. They may return later, but
with the DSP rebuilt from scratch.

What "archived" means:

- **Not built by default**: the root CMakeLists only globs `plugins/*`.
  Opt in locally with `-DFACTORY_INCLUDE_ARCHIVED=ON` (combines with
  `-DFACTORY_PLUGINS=<slug>` narrowing as usual). CI and releases never set it.
- **Excluded from CI**: `ci.yml` / `clap.yml` are path-scoped to `plugins/**`
  and shared code, so changes under `archive/` trigger no plugin builds.
  `factory-tools-ci.yml` still gates README-catalog freshness against
  `archive/plugins/*/plugin.toml` (the Archived section).
- **Excluded from releases and the installer**: `tools/release_plan.py` scans
  `plugins/*` only, so archived slugs drop out of the next release manifest;
  `gen_catalog.py --emit-json` (catalog.json) never includes them.

The layout under `archive/plugins/<slug>/` is unchanged from `plugins/`, so
un-archiving a plugin is a `git mv` back — but per the migration decision, a
revival starts with new DSP, not a straight restore.
