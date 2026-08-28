// Pure request-to-object routing for the delivery Worker (plan §5.1, §5.2).
//
// Everything here is a pure function of the request path so it can be tested
// with `node --test` and no Cloudflare runtime. The Worker itself (index.js) is
// then thin enough to read in one sitting, which matters: this is the only code
// between a user and every binary we ship.

// Cache policy. These two strings MUST stay identical to
// tools/promote/store.py (IMMUTABLE_CACHE_CONTROL / POINTER_CACHE_CONTROL) —
// promote sets them on the object, the Worker sets them on the response, and
// tools/promote/cdn_check.py verifies what the edge finally returns. Three
// places, one policy; tools/tests/test_cdn_check.py fails if they drift.
export const IMMUTABLE_CACHE_CONTROL = 'public, max-age=31536000, immutable';
export const POINTER_CACHE_CONTROL = 'public, max-age=60, must-revalidate';

// The published namespace, as an allowlist. A path that matches nothing here
// never reaches R2: the Worker is a relay for objects we publish, not a proxy
// for the bucket. `counted` marks the download paths — the badge/update-check
// path (updates/v1) deliberately records NOTHING (plan §5.2).
const ROUTES = [
  { kind: 'plugin',            re: /^artifacts\/(?!installer\/)[^/]+\/[^/]+\/[^/]+$/,   cache: IMMUTABLE_CACHE_CONTROL, counted: true },
  { kind: 'installer',         re: /^artifacts\/installer\/[^/]+\/[^/]+$/,              cache: IMMUTABLE_CACHE_CONTROL, counted: true },
  { kind: 'pointer',           re: /^updates\/v1\/(latest|catalog)\.json(\.minisig)?$/, cache: POINTER_CACHE_CONTROL,   counted: false },
  { kind: 'history',           re: /^updates\/v1\/history\/[^/]+$/,                     cache: IMMUTABLE_CACHE_CONTROL, counted: false },
  { kind: 'bootstrap-payload', re: /^bootstrap\/[^/]+\/install\.(sh|ps1)$/,             cache: IMMUTABLE_CACHE_CONTROL, counted: false },
  { kind: 'bootstrap-shim',    re: /^install\.(sh|ps1)$/,                               cache: POINTER_CACHE_CONTROL,   counted: false },
  { kind: 'notes',             re: /^notes\/[^/]+\/[^/]+\.md$/,                         cache: POINTER_CACHE_CONTROL,   counted: false },
];

export const PREFIX = '/tatsunarisounds/';

const CONTENT_TYPES = {
  '.json': 'application/json',
  '.minisig': 'text/plain; charset=utf-8',
  '.md': 'text/markdown; charset=utf-8',
  '.sh': 'text/x-shellscript; charset=utf-8',
  '.ps1': 'text/plain; charset=utf-8',
  '.zip': 'application/zip',
};

// Release zip naming, from release.yml's package step. Kept in sync with
// tools/promote/manifest.py ZIP_RE / TARGETS: the aggregate is only useful if
// its os/format/arch mean the same thing as the catalog's.
const ZIP_RE = /^(?<slug>[a-z0-9]+(?:-[a-z0-9]+)*)-v(?<v>\d+_\d+_\d+)-(?<target>macOS-AU|macOS-VST3|Windows)\.zip$/;
const TARGETS = {
  'macOS-AU': { format: 'au', os: 'macos', arch: 'universal' },
  'macOS-VST3': { format: 'vst3', os: 'macos', arch: 'universal' },
  Windows: { format: 'vst3', os: 'windows', arch: 'x86_64' },
};

/**
 * Resolve a URL path to an object key, or null when it is not something we
 * publish.
 *
 * The query string never reaches this function and is never looked at again —
 * invariant 9 says we do not persist arbitrary identifiers, and the cheapest
 * way to keep that true is for this code to have no access to them at all.
 */
export function route(pathname) {
  if (!pathname.startsWith(PREFIX)) return null;
  let key = pathname.slice(PREFIX.length);

  // Decode once, then validate. Validating the encoded form would let `%2e%2e`
  // through; decoding repeatedly would let `%252e` through. Once, then check.
  try {
    key = decodeURIComponent(key);
  } catch {
    return null; // malformed percent-encoding
  }
  if (!isSafeKey(key)) return null;

  for (const r of ROUTES) {
    if (r.re.test(key)) {
      return { key, kind: r.kind, cacheControl: r.cache, counted: r.counted };
    }
  }
  return null;
}

/** Reject anything that is not a plain, forward-only key. */
export function isSafeKey(key) {
  if (key === '' || key.length > 512) return false;
  if (key.startsWith('/') || key.includes('//')) return false;
  if (key.includes('\\')) return false;
  if (/[\u0000-\u001f\u007f]/.test(key)) return false;
  // Unicode separators and bidi controls have no business in an object key and
  // are a classic way to make a path read as something it is not.
  if (/[\u00a0\u1680\u2000-\u200f\u2028\u2029\u202a-\u202e\u2066-\u2069\u3000\ufeff]/.test(key)) return false;
  for (const seg of key.split('/')) {
    if (seg === '' || seg === '.' || seg === '..') return false;
  }
  return true;
}

export function contentTypeFor(key) {
  const dot = key.lastIndexOf('.');
  if (dot < 0) return 'application/octet-stream';
  return CONTENT_TYPES[key.slice(dot)] ?? 'application/octet-stream';
}

/**
 * The dimensions recorded for a counted download.
 *
 * Derived from the KEY only. Nothing about the requester is available to this
 * function by construction — no headers, no IP, no cookies, no query — because
 * the safest way to not record something is to be unable to.
 *
 * `channel` is deliberately absent: the artifact path carries no channel, and
 * inventing one per request would mean fetching the catalog on every download.
 * Which channel a version belonged to is a join against catalog.json at
 * aggregation time, which is where that question actually gets asked.
 */
export function dimensionsFor(key, kind) {
  const parts = key.split('/');
  if (kind === 'installer') {
    // artifacts/installer/<version>/tatsunari-sounds-installer-<os>-<arch>[.exe],
    // which is what installer.yml actually builds (plan §11.5). Matching a
    // shorter prefix here would parse to an empty os/arch and quietly record
    // every installer download as unattributed.
    const [, , version, file] = parts;
    const m = /^tatsunari-sounds-installer-([a-z0-9]+)-([a-z0-9_]+)(\.exe)?$/.exec(file ?? '');
    return {
      product: 'installer', slug: 'installer', version: version ?? '',
      format: 'binary', os: m ? m[1] : '', arch: m ? m[2] : '',
    };
  }
  // artifacts/<slug>/<version>/<zip>
  const [, slug, version, file] = parts;
  const m = ZIP_RE.exec(file ?? '');
  const t = m ? TARGETS[m.groups.target] : null;
  return {
    product: 'plugin', slug: slug ?? '', version: version ?? '',
    format: t ? t.format : '', os: t ? t.os : '', arch: t ? t.arch : '',
  };
}

/**
 * Should this response be counted as one download?
 *
 * Three rules, each of which exists because breaking it inflates the number:
 *   * HEAD is a metadata probe, not a download;
 *   * a failed response is not a download (plan §5.2 states this outright), so
 *     only 200 and 206 count;
 *   * a ranged request that does not start at byte 0 is a RESUME of a download
 *     already counted. Counting it would make every flaky connection look like
 *     extra users.
 */
export function shouldCount(routed, method, status, rangeHeader) {
  if (!routed || !routed.counted) return false;
  if (method !== 'GET') return false;
  if (status !== 200 && status !== 206) return false;
  if (status === 206) {
    const r = parseRange(rangeHeader);
    if (!r || r.offset !== 0) return false;
  }
  return true;
}

/**
 * Parse a single-range `bytes=` header. Multi-range is not supported and
 * returns null rather than being silently served as a full body.
 */
export function parseRange(header) {
  if (!header) return null;
  const m = /^bytes=(\d*)-(\d*)$/.exec(header.trim());
  if (!m) return null;
  const [, startRaw, endRaw] = m;
  if (startRaw === '' && endRaw === '') return null;
  if (startRaw === '') {
    const suffix = Number(endRaw);
    if (!Number.isFinite(suffix) || suffix <= 0) return null;
    return { suffix };
  }
  const offset = Number(startRaw);
  if (!Number.isFinite(offset)) return null;
  if (endRaw === '') return { offset };
  const end = Number(endRaw);
  if (!Number.isFinite(end) || end < offset) return null;
  return { offset, length: end - offset + 1 };
}
