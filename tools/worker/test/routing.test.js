// Routing/allowlist tests. Run with: node --test test/
//
// Written as "the thing that breaks if this is wrong", because every one of
// these has a real failure mode: an over-broad allowlist exposes the bucket, a
// mis-signed range inflates the download count, and a counted update-check turns
// the badge poll into telemetry.

import test from 'node:test';
import assert from 'node:assert/strict';

import {
  IMMUTABLE_CACHE_CONTROL,
  POINTER_CACHE_CONTROL,
  contentTypeFor,
  dimensionsFor,
  isSafeKey,
  parseRange,
  route,
  shouldCount,
} from '../src/routing.js';

const P = '/tatsunarisounds/';
const ZIP = `${P}artifacts/tn-equalizer/0.1.0/tn-equalizer-v0_1_0-Windows.zip`;

test('published paths route to their key with the right cache policy', () => {
  const cases = [
    [ZIP, 'plugin', IMMUTABLE_CACHE_CONTROL, true],
    [`${P}artifacts/installer/1.2.0/tatsunari-darwin-arm64`, 'installer', IMMUTABLE_CACHE_CONTROL, true],
    [`${P}updates/v1/latest.json`, 'pointer', POINTER_CACHE_CONTROL, false],
    [`${P}updates/v1/catalog.json.minisig`, 'pointer', POINTER_CACHE_CONTROL, false],
    [`${P}updates/v1/history/latest-20260101T000000Z.json`, 'history', IMMUTABLE_CACHE_CONTROL, false],
    [`${P}bootstrap/1/install.sh`, 'bootstrap-payload', IMMUTABLE_CACHE_CONTROL, false],
    [`${P}install.ps1`, 'bootstrap-shim', POINTER_CACHE_CONTROL, false],
    [`${P}notes/tn-equalizer/0.1.0.md`, 'release-notes-or-notes', POINTER_CACHE_CONTROL, false],
  ];
  for (const [path, kind, cache, counted] of cases) {
    const r = route(path);
    assert.ok(r, `${path} should route`);
    assert.equal(r.key, path.slice(P.length));
    assert.equal(r.cacheControl, cache, path);
    assert.equal(r.counted, counted, path);
    if (kind !== 'release-notes-or-notes') assert.equal(r.kind, kind, path);
  }
});

test('the bootstrap shim and its payload get opposite cache policies', () => {
  // They share a filename. Swapping them either pins users to a stale entry
  // point forever or makes the payload uncacheable.
  assert.equal(route(`${P}install.sh`).cacheControl, POINTER_CACHE_CONTROL);
  assert.equal(route(`${P}bootstrap/1/install.sh`).cacheControl, IMMUTABLE_CACHE_CONTROL);
});

test('anything outside the published namespace never reaches R2', () => {
  const rejected = [
    '/',
    '/tatsunarisounds',
    '/other/artifacts/x/1.0.0/a.zip',
    `${P}`,
    `${P}artifacts`,                                  // listing attempt
    `${P}artifacts/tn-equalizer`,                     // listing attempt
    `${P}updates/v1/`,                                // listing attempt
    `${P}updates/v2/latest.json`,                     // frozen schema path
    `${P}secret.txt`,
    `${P}bootstrap/1/install.exe`,
    `${P}artifacts/tn-equalizer/0.1.0/nested/deep.zip`,
  ];
  for (const path of rejected) {
    assert.equal(route(path), null, path);
  }
});

test('traversal and encoding tricks are rejected, decoded exactly once', () => {
  const attacks = [
    `${P}artifacts/../../etc/passwd`,
    `${P}artifacts/%2e%2e/%2e%2e/etc/passwd`,   // encoded traversal
    `${P}artifacts/%252e%252e/x`,               // double-encoded: must NOT decode twice
    `${P}artifacts//tn-equalizer/0.1.0/a.zip`,  // empty segment
    `${P}artifacts/tn-equalizer/./0.1.0/a.zip`,
    `${P}/artifacts/x/1.0.0/a.zip`,             // leading slash after the prefix
    `${P}artifacts\\tn-equalizer\\a.zip`,       // backslash
    `${P}artifacts/%00/x`,                      // NUL
    `${P}artifacts/%ef%bb%bf/x`,                // BOM
    `${P}artifacts/%e2%80%ae/x`,                // RTL override
    `${P}artifacts/%`,                          // malformed percent-encoding
  ];
  for (const path of attacks) {
    assert.equal(route(path), null, path);
  }
});

test('isSafeKey rejects empty, dotted and absurdly long keys', () => {
  assert.equal(isSafeKey(''), false);
  assert.equal(isSafeKey('..'), false);
  assert.equal(isSafeKey('a/../b'), false);
  assert.equal(isSafeKey('a'.repeat(513)), false);
  assert.equal(isSafeKey('artifacts/tn-equalizer/0.1.0/a.zip'), true);
});

test('content types are declared, never sniffed', () => {
  assert.equal(contentTypeFor('updates/v1/latest.json'), 'application/json');
  assert.equal(contentTypeFor('artifacts/x/1.0.0/a.zip'), 'application/zip');
  assert.equal(contentTypeFor('install.sh'), 'text/x-shellscript; charset=utf-8');
  assert.equal(contentTypeFor('artifacts/installer/1.0.0/tatsunari-darwin-arm64'),
    'application/octet-stream');
});

test('dimensions come from the key and match the catalog vocabulary', () => {
  // These must agree with tools/promote/manifest.py TARGETS, or the aggregate
  // and the catalog describe different things.
  assert.deepEqual(
    dimensionsFor('artifacts/tn-equalizer/0.1.0/tn-equalizer-v0_1_0-Windows.zip', 'plugin'),
    { product: 'plugin', slug: 'tn-equalizer', version: '0.1.0', format: 'vst3', os: 'windows', arch: 'x86_64' });
  assert.deepEqual(
    dimensionsFor('artifacts/tn-vocal-tuner/0.1.0/tn-vocal-tuner-v0_1_0-macOS-AU.zip', 'plugin'),
    { product: 'plugin', slug: 'tn-vocal-tuner', version: '0.1.0', format: 'au', os: 'macos', arch: 'universal' });
  assert.deepEqual(
    dimensionsFor('artifacts/installer/1.2.0/tatsunari-windows-amd64.exe', 'installer'),
    { product: 'installer', slug: 'installer', version: '1.2.0', format: 'binary', os: 'windows', arch: 'amd64' });
});

test('installer downloads are kept separate from plugin downloads', () => {
  // Plan §5.2: "do not mix them". They differ by `product`, so an aggregate can
  // never accidentally sum both.
  const plugin = dimensionsFor('artifacts/tn-equalizer/0.1.0/tn-equalizer-v0_1_0-Windows.zip', 'plugin');
  const installer = dimensionsFor('artifacts/installer/1.2.0/tatsunari-darwin-arm64', 'installer');
  assert.notEqual(plugin.product, installer.product);
});

test('range parsing accepts the forms curl actually sends', () => {
  assert.deepEqual(parseRange('bytes=0-'), { offset: 0 });
  assert.deepEqual(parseRange('bytes=100-199'), { offset: 100, length: 100 });
  assert.deepEqual(parseRange('bytes=-500'), { suffix: 500 });
  assert.equal(parseRange('bytes=0-0, 100-199'), null, 'multi-range is unsupported');
  assert.equal(parseRange('items=0-10'), null);
  assert.equal(parseRange('bytes=200-100'), null, 'end before start');
  assert.equal(parseRange('bytes=-'), null);
  assert.equal(parseRange(null), null);
});

test('only real, successful, non-resumed plugin GETs are counted', () => {
  const artifact = route(ZIP);
  const pointer = route(`${P}updates/v1/latest.json`);

  assert.equal(shouldCount(artifact, 'GET', 200, null), true);
  assert.equal(shouldCount(artifact, 'GET', 206, 'bytes=0-1023'), true,
    'a range starting at 0 is the start of a download');

  // The update-check path is what the plugin badge polls. Counting it would be
  // telemetry by the back door (invariant 9).
  assert.equal(shouldCount(pointer, 'GET', 200, null), false);

  assert.equal(shouldCount(artifact, 'HEAD', 200, null), false, 'HEAD is a probe');
  assert.equal(shouldCount(artifact, 'GET', 404, null), false);
  assert.equal(shouldCount(artifact, 'GET', 416, 'bytes=99999999-'), false);
  assert.equal(shouldCount(artifact, 'GET', 304, null), false);
  assert.equal(shouldCount(artifact, 'GET', 206, 'bytes=1024-2047'), false,
    'a resumed range is not a second download');
  assert.equal(shouldCount(artifact, 'GET', 206, 'bytes=-500'), false,
    'a suffix range is a probe or a resume, never a whole download');
  assert.equal(shouldCount(null, 'GET', 200, null), false);
});
