// Cloudflare Worker: serve /tatsunarisounds/* from R2 and count downloads.
//
// Plan §5.1 (Cloudflare is the only delivery path) and §5.2 (download counting
// via Workers Analytics Engine). The Worker exists for exactly two reasons:
//
//   1. it keeps the R2 bucket private, so the only addressable form of every
//      artifact is the fixed https://6sryusk.com/tatsunarisounds/... path that
//      shipped binaries validate (invariant 7) — a *.r2.dev URL leaking would
//      orphan every installed client the day hosting changes;
//   2. it is the ONLY place a download can be counted without putting telemetry
//      in the plugins (invariant 9).
//
// Everything it must NOT do is as load-bearing as what it does:
//   * it never reads the client IP, cookies, User-Agent or query string into
//     anything it writes. `route()` is given a pathname and nothing else;
//   * it records nothing at all on the update-check path (updates/v1), which is
//     what the plugin badge polls — that path must stay a plain static fetch;
//   * it never counts a failed response, a HEAD, or a resumed range.
//
// FALLBACK: if this Worker is unhealthy, the route can be removed and an R2
// custom domain bound to the same hostname serves the same keys directly. The
// only thing lost is counting. That drill is in the runbook and is why response
// headers here are set to the same values promote already put on the objects —
// removing the Worker must not change what clients see.

import {
  PREFIX,
  contentTypeFor,
  dimensionsFor,
  parseRange,
  route,
  shouldCount,
} from './routing.js';

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    if (request.method !== 'GET' && request.method !== 'HEAD') {
      return new Response('method not allowed', {
        status: 405,
        headers: { allow: 'GET, HEAD', 'cache-control': 'no-store' },
      });
    }

    // Only the pathname is passed on. The query string stops here.
    const routed = route(url.pathname);
    if (!routed) {
      return notFound();
    }

    const rangeHeader = request.headers.get('range');
    const response = await serve(env, request, routed, rangeHeader);

    if (shouldCount(routed, request.method, response.status, rangeHeader)) {
      // Fire-and-forget: the count must never delay or fail a download.
      ctx.waitUntil(record(env, routed, response.status));
    }
    return response;
  },
};

async function serve(env, request, routed, rangeHeader) {
  // Conditional GET, which is the other half of the short-TTL pointer model:
  // the plugins poll latest.json often and must get a 304 almost every time.
  const ifNoneMatch = request.headers.get('if-none-match') ?? undefined;
  const ifModifiedSince = parseHttpDate(request.headers.get('if-modified-since'));

  let range;
  if (rangeHeader) {
    range = parseRange(rangeHeader);
    if (!range) {
      // Unparseable or multi-range. Serving the whole body instead would look
      // like success to a resuming client and silently corrupt its file.
      return new Response('range not satisfiable', {
        status: 416,
        headers: { 'accept-ranges': 'bytes', 'cache-control': 'no-store' },
      });
    }
  }

  const object = await env.BUCKET.get(routed.key, {
    range,
    onlyIf: {
      etagDoesNotMatch: ifNoneMatch,
      uploadedAfter: ifModifiedSince,
    },
  });

  if (object === null) {
    return notFound();
  }

  const headers = new Headers();
  headers.set('etag', object.httpEtag);
  headers.set('cache-control', routed.cacheControl);
  headers.set('accept-ranges', 'bytes');
  headers.set('content-type', contentTypeFor(routed.key));
  // Belt and braces for the script and zip routes: a browser must download
  // these, never run them in our origin.
  headers.set('x-content-type-options', 'nosniff');
  if (object.uploaded) headers.set('last-modified', object.uploaded.toUTCString());

  // A precondition match comes back without a body — R2 signals it by omitting
  // the body, not by throwing.
  if (!object.body) {
    return new Response(null, { status: 304, headers });
  }

  if (object.range && typeof object.range.offset === 'number') {
    const start = object.range.offset;
    const length = object.range.length ?? object.size - start;
    const end = start + length - 1;
    if (start >= object.size) {
      return new Response('range not satisfiable', {
        status: 416,
        headers: { 'accept-ranges': 'bytes', 'content-range': `bytes */${object.size}` },
      });
    }
    headers.set('content-range', `bytes ${start}-${end}/${object.size}`);
    headers.set('content-length', String(length));
    return new Response(object.body, { status: 206, headers });
  }

  headers.set('content-length', String(object.size));
  // HEAD gets the identical headers and no body. Cloudflare would drop the body
  // for us, but being explicit keeps content-length honest.
  if (request.method === 'HEAD') {
    return new Response(null, { status: 200, headers });
  }
  return new Response(object.body, { status: 200, headers });
}

/**
 * Write one download data point.
 *
 * The blobs are derived from the object key and the response status only. There
 * is deliberately no requester dimension of any kind: plan §5.2 lists IP,
 * User-Agent and arbitrary identifiers as things we do not record, and this
 * function is not given them.
 *
 * Bot exclusion is an AGGREGATION-TIME rule, documented in the runbook, not a
 * request-time one: dropping requests here would need a User-Agent judgement
 * per request, which is exactly the data we promised not to key on.
 */
async function record(env, routed, status) {
  const ds = env.DOWNLOADS;
  if (!ds) return; // analytics unbound (preview/dev) — never a reason to 500
  const d = dimensionsFor(routed.key, routed.kind);
  try {
    ds.writeDataPoint({
      // index1 is the sampling/grouping key: per-plugin totals are the question
      // asked most often.
      indexes: [d.slug],
      blobs: [
        new Date().toISOString().slice(0, 10), // date (UTC), no clock time
        d.product,
        d.slug,
        d.version,
        d.os,
        d.format,
        d.arch,
        String(status),
      ],
      doubles: [1],
    });
  } catch {
    // Counting is best-effort by design. A download that succeeded is a
    // download that succeeded.
  }
}

function notFound() {
  // Deliberately identical for "not in the allowlist" and "not in the bucket":
  // the response must not be an oracle for what exists in R2.
  return new Response('not found', {
    status: 404,
    headers: { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store' },
  });
}

function parseHttpDate(value) {
  if (!value) return undefined;
  const t = Date.parse(value);
  return Number.isNaN(t) ? undefined : new Date(t);
}

export { PREFIX, serve };
