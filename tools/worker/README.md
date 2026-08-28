# tools/worker — delivery Worker for `6sryusk.com/tatsunarisounds/*`

The Cloudflare Worker that serves every published object from a **private** R2
bucket and counts plugin/installer downloads. Plan §5.1 (GitHub Releases
retirement) and §5.2 (download counting).

```
tools/worker/
  src/routing.js   pure path → object-key routing, allowlist, count rules
  src/index.js     the fetch handler (R2 read, Range/conditional, data point)
  test/            node --test, no dependencies, no Cloudflare runtime needed
  wrangler.toml    bindings + route. No secrets, ever.
```

## Why there is a Worker at all

Two reasons, and only two:

1. **The bucket stays private.** Every shipped binary validates that its URLs
   are under `https://6sryusk.com/tatsunarisounds/` (invariant 7). If the R2
   `*.r2.dev` URL were public, an artifact would have a second addressable form,
   and anything pinned to it is orphaned the day hosting moves. Keep the dev URL
   **disabled**.
2. **Counting has to happen somewhere that isn't the plugin.** Invariant 9 bans
   telemetry in the plugins, so the count is a server-side property of the
   download itself.

## What it must never do

* Read the client IP, cookies, User-Agent, `Referer` or the query string into
  anything it stores. `route()` takes a pathname and nothing else, and
  `dimensionsFor()` takes an object key — the requester is not reachable from
  the code that writes a data point, by construction.
* Record anything on the update-check path (`updates/v1/…`). That is what the
  in-plugin badge polls; counting it would be telemetry by the back door.
* Count a HEAD, a failed response, or a resumed range. See below.

## Counting rules

A data point is written for `artifacts/**` only, and only when **all** of:

| rule | why |
|---|---|
| method is `GET` | HEAD is a metadata probe, not a download |
| status is 200 or 206 | plan §5.2: failed responses are not successes |
| a 206's range starts at byte 0 | a resume is part of a download already counted |

Recorded: date (UTC, no clock time), product (`plugin` / `installer`), slug,
version, os, format, arch, HTTP status. Plugin and installer downloads carry
different `product` values so an aggregate can never accidentally sum both.

Two deliberate omissions:

* **channel** — the artifact path carries no channel, and deriving one per
  request would mean fetching the catalog on every download. Which channel a
  version belonged to is a join against `catalog.json` at aggregation time.
* **bot exclusion** — an aggregation-time rule, not a request-time one.
  Excluding bots at the edge needs a User-Agent judgement per request, i.e.
  exactly the data we promised not to key on. Document the exclusion rule with
  the query, not in this code.

## Running the tests

```bash
cd tools/worker
npm test          # node --test test/*.test.js — no install step, no deps
```

`factory-tools-ci.yml` runs the same command. The tests use a fake R2 bucket and
a fake Analytics dataset, so they need neither credentials nor `wrangler`.

## Deploying (human only)

```bash
cd tools/worker
npx wrangler deploy
```

Not in CI: a deploy needs a Cloudflare API token, and production changes are
behind human approval (plan §6, and "Ask a human" in `CLAUDE.md`). The bucket
name and dataset are bindings resolved at deploy time; nothing secret belongs in
`wrangler.toml`.

## Fallback when the Worker is unhealthy

Remove the route and bind an R2 custom domain to the same hostname: the same
keys are served from the same paths, and the only thing lost is counting. This
is why the Worker sets exactly the `Cache-Control` values `promote` already put
on the objects — taking the Worker out must not change what clients see. The
drill is in the release runbook.

## Related

* `tools/promote/store.py` — writes the same two `Cache-Control` values onto the
  objects. The strings are compared across languages by
  `tools/tests/test_cdn_check.py`.
* `tools/promote/cdn_check.py` — fetches the published URLs and asserts the edge
  really returns them.
* `tools/promote/manifest.py` — `TARGETS` / `ZIP_RE`, which `routing.js` mirrors
  so the aggregate and the catalog use the same os/format/arch vocabulary.
