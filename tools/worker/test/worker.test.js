// End-to-end tests for the Worker's fetch handler against a fake R2 bucket and
// a fake Analytics Engine dataset. Run with: node --test test/*.test.js
//
// These cover the behaviours that only show up when the pieces are wired
// together: what a resumed download does to the count, what an unroutable path
// does to the bucket, and what actually ends up in a data point.

import test from 'node:test';
import assert from 'node:assert/strict';

import worker from '../src/index.js';

const HOST = 'https://6sryusk.com';
const P = '/tatsunarisounds/';
const ZIP_KEY = 'artifacts/tn-equalizer/0.1.0/tn-equalizer-v0_1_0-Windows.zip';
const BODY = new Uint8Array(64).fill(7);

function makeEnv({ objects = { [ZIP_KEY]: BODY }, analytics = true } = {}) {
  const gets = [];
  const points = [];
  const env = {
    BUCKET: {
      async get(key, opts = {}) {
        gets.push({ key, opts });
        const data = objects[key];
        if (data === undefined) return null;
        const etag = `"etag-${key.length}"`;
        const uploaded = new Date('2026-08-01T00:00:00Z');

        // Conditional request: R2 returns an object with no body.
        if (opts.onlyIf?.etagDoesNotMatch === etag) {
          return { size: data.length, httpEtag: etag, uploaded, body: null };
        }

        const range = opts.range;
        if (!range) {
          return { size: data.length, httpEtag: etag, uploaded, body: data };
        }
        const offset = range.offset ?? data.length - range.suffix;
        const length = range.length ?? data.length - offset;
        return {
          size: data.length, httpEtag: etag, uploaded,
          body: data.slice(offset, offset + length),
          range: { offset, length },
        };
      },
    },
    gets,
    points,
  };
  if (analytics) {
    env.DOWNLOADS = { writeDataPoint: (p) => points.push(p) };
  }
  return env;
}

// The real ExecutionContext defers work; running it inline is what lets a test
// assert on the data point that was written.
function ctx() {
  const pending = [];
  return { waitUntil: (p) => pending.push(p), settle: () => Promise.all(pending) };
}

async function call(env, path, { method = 'GET', headers = {} } = {}) {
  const c = ctx();
  const res = await worker.fetch(new Request(HOST + path, { method, headers }), env, c);
  await c.settle();
  return res;
}

test('a plugin download is served immutable and counted once', async () => {
  const env = makeEnv();
  const res = await call(env, P + ZIP_KEY);

  assert.equal(res.status, 200);
  assert.equal(res.headers.get('cache-control'), 'public, max-age=31536000, immutable');
  assert.equal(res.headers.get('content-type'), 'application/zip');
  assert.equal(res.headers.get('accept-ranges'), 'bytes');
  assert.equal(res.headers.get('content-length'), '64');
  assert.ok(res.headers.get('etag'));
  assert.equal((await res.arrayBuffer()).byteLength, 64);

  assert.equal(env.points.length, 1);
  assert.deepEqual(env.points[0].indexes, ['tn-equalizer']);
  assert.deepEqual(env.points[0].doubles, [1]);
  const blobs = env.points[0].blobs;
  assert.deepEqual(blobs.slice(1), ['plugin', 'tn-equalizer', '0.1.0', 'windows', 'vst3', 'x86_64', '200']);
  assert.match(blobs[0], /^\d{4}-\d{2}-\d{2}$/, 'a date, with no clock time');
});

test('the update-check path is served short-TTL and records nothing', async () => {
  // This is the plugin badge's poll. Counting it would be telemetry in the
  // plugins by another route (invariant 9).
  const env = makeEnv({ objects: { 'updates/v1/latest.json': new Uint8Array([123, 125]) } });
  const res = await call(env, `${P}updates/v1/latest.json`);

  assert.equal(res.status, 200);
  assert.equal(res.headers.get('cache-control'), 'public, max-age=60, must-revalidate');
  assert.equal(res.headers.get('content-type'), 'application/json');
  assert.equal(env.points.length, 0);
});

test('a conditional poll gets a 304 and is not counted', async () => {
  const env = makeEnv();
  const first = await call(env, P + ZIP_KEY);
  const etag = first.headers.get('etag');
  env.points.length = 0;

  const res = await call(env, P + ZIP_KEY, { headers: { 'if-none-match': etag } });
  assert.equal(res.status, 304);
  assert.equal(res.headers.get('cache-control'), 'public, max-age=31536000, immutable');
  assert.equal(env.points.length, 0, 'a 304 transferred no bytes');
});

test('HEAD returns the same headers, no body, and no count', async () => {
  const env = makeEnv();
  const res = await call(env, P + ZIP_KEY, { method: 'HEAD' });

  assert.equal(res.status, 200);
  assert.equal(res.headers.get('content-length'), '64');
  assert.equal(res.headers.get('cache-control'), 'public, max-age=31536000, immutable');
  assert.equal((await res.arrayBuffer()).byteLength, 0);
  assert.equal(env.points.length, 0);
});

test('a range from byte 0 is a 206 and counts as one download', async () => {
  const env = makeEnv();
  const res = await call(env, P + ZIP_KEY, { headers: { range: 'bytes=0-15' } });

  assert.equal(res.status, 206);
  assert.equal(res.headers.get('content-range'), 'bytes 0-15/64');
  assert.equal(res.headers.get('content-length'), '16');
  assert.equal((await res.arrayBuffer()).byteLength, 16);
  assert.equal(env.points.length, 1);
});

test('a resumed range is served but not counted again', async () => {
  // A flaky connection that resumes three times is one download, not four.
  const env = makeEnv();
  const res = await call(env, P + ZIP_KEY, { headers: { range: 'bytes=16-31' } });

  assert.equal(res.status, 206);
  assert.equal(res.headers.get('content-range'), 'bytes 16-31/64');
  assert.equal(env.points.length, 0);
});

test('an unparseable or multi-range request is 416, never a silent full body', async () => {
  const env = makeEnv();
  for (const range of ['bytes=0-10, 20-30', 'bytes=abc', 'bytes=99-1']) {
    const res = await call(env, P + ZIP_KEY, { headers: { range } });
    assert.equal(res.status, 416, range);
    assert.equal(env.points.length, 0, range);
  }
});

test('an unroutable path is 404 and the bucket is never touched', async () => {
  const env = makeEnv();
  for (const path of [`${P}artifacts/../secret`, `${P}nope.txt`, '/elsewhere', `${P}artifacts`]) {
    const res = await call(env, path);
    assert.equal(res.status, 404, path);
  }
  assert.equal(env.gets.length, 0, 'the allowlist runs before R2, not after');
  assert.equal(env.points.length, 0);
});

test('a missing object is 404 and is not counted', async () => {
  const env = makeEnv({ objects: {} });
  const res = await call(env, P + ZIP_KEY);
  assert.equal(res.status, 404);
  assert.equal(env.points.length, 0, 'a failed response is not a download');
});

test('writes are refused outright', async () => {
  const env = makeEnv();
  for (const method of ['PUT', 'POST', 'DELETE']) {
    const res = await call(env, P + ZIP_KEY, { method });
    assert.equal(res.status, 405, method);
    assert.equal(res.headers.get('allow'), 'GET, HEAD');
  }
  assert.equal(env.gets.length, 0);
});

test('the query string reaches neither R2 nor the data point', async () => {
  const env = makeEnv();
  await call(env, `${P}${ZIP_KEY}?utm_source=newsletter&uid=deadbeef`, {
    headers: {
      cookie: 'sid=deadbeef',
      'user-agent': 'Mozilla/5.0 tracking-me',
      'cf-connecting-ip': '203.0.113.9',
      referer: 'https://example.invalid/somewhere',
    },
  });

  assert.equal(env.gets[0].key, ZIP_KEY, 'the key is the path, without the query');
  const recorded = JSON.stringify(env.points);
  for (const secret of ['deadbeef', 'utm_source', 'Mozilla', '203.0.113.9', 'example.invalid']) {
    assert.equal(recorded.includes(secret), false, `${secret} must not be recorded`);
  }
});

test('an unbound analytics dataset degrades to serving without counting', async () => {
  // Preview and dev deployments have no dataset. A download must still work.
  const env = makeEnv({ analytics: false });
  const res = await call(env, P + ZIP_KEY);
  assert.equal(res.status, 200);
});

test('a failing analytics write never fails the download', async () => {
  const env = makeEnv();
  env.DOWNLOADS = { writeDataPoint() { throw new Error('dataset over quota'); } };
  const res = await call(env, P + ZIP_KEY);
  assert.equal(res.status, 200);
  assert.equal((await res.arrayBuffer()).byteLength, 64);
});
