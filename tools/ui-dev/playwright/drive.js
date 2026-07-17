// Reusable Playwright helpers for the Visage gallery (adapted from spike S1).
//
// Chromium is driven via executablePath at the preinstalled build, so the
// Playwright package version need not match the browser revision. WebGL in
// headless-as-root works with the SwiftShader flag set below — the load-bearing
// flag is --enable-unsafe-swiftshader (modern Chromium refuses SwiftShader for
// WebGL without it, and the canvas comes up blank).
const { chromium } = require("playwright");
const { PNG } = require("pngjs");

const CHROME =
  process.env.CHROME_BIN || "/opt/pw-browsers/chromium-1194/chrome-linux/chrome";

const FLAGS = (process.env.CHROME_FLAGS
  ? process.env.CHROME_FLAGS.split(/\s+/)
  : [
      "--no-sandbox",
      "--disable-dev-shm-usage",
      "--use-gl=angle",
      "--use-angle=swiftshader",
      "--enable-unsafe-swiftshader",
      "--ignore-gpu-blocklist",
      "--enable-webgl",
      "--disable-gpu-sandbox",
    ]
).filter(Boolean);

// A rendered UI has many distinct colours and real luminance spread; a blank or
// uniform canvas has ~1 colour and ~0 stddev.
function analyzePNG(buf) {
  const png = PNG.sync.read(buf);
  const { width, height, data } = png;
  const buckets = new Set();
  let n = 0, sum = 0, sumSq = 0;
  const step = 4;
  for (let y = 0; y < height; y += step) {
    for (let x = 0; x < width; x += step) {
      const i = (y * width + x) * 4;
      const r = data[i], g = data[i + 1], b = data[i + 2];
      buckets.add(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
      const luma = 0.299 * r + 0.587 * g + 0.114 * b;
      n++; sum += luma; sumSq += luma * luma;
    }
  }
  const mean = sum / n;
  const variance = sumSq / n - mean * mean;
  return {
    width, height, sampledPixels: n,
    distinctColors: buckets.size,
    lumaMean: +mean.toFixed(2),
    lumaStdDev: +Math.sqrt(Math.max(0, variance)).toFixed(2),
  };
}

function notBlank(stats) {
  return stats.distinctColors >= 8 && stats.lumaStdDev >= 8;
}

// Stats over a sub-rectangle of a PNG buffer (window px == canvas buffer px here).
function analyzeRegion(buf, rect) {
  const png = PNG.sync.read(buf);
  const { width, height, data } = png;
  const x0 = Math.max(0, Math.floor(rect.x)), y0 = Math.max(0, Math.floor(rect.y));
  const x1 = Math.min(width, Math.ceil(rect.x + rect.w)), y1 = Math.min(height, Math.ceil(rect.y + rect.h));
  const buckets = new Set();
  let n = 0, sum = 0, sumSq = 0;
  for (let y = y0; y < y1; y += 2) {
    for (let x = x0; x < x1; x += 2) {
      const i = (y * width + x) * 4;
      const r = data[i], g = data[i + 1], b = data[i + 2];
      buckets.add(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
      const luma = 0.299 * r + 0.587 * g + 0.114 * b;
      n++; sum += luma; sumSq += luma * luma;
    }
  }
  const mean = sum / Math.max(1, n);
  const variance = sumSq / Math.max(1, n) - mean * mean;
  return { sampledPixels: n, distinctColors: buckets.size,
           lumaMean: +mean.toFixed(2), lumaStdDev: +Math.sqrt(Math.max(0, variance)).toFixed(2) };
}

// Mean absolute per-channel pixel difference between two PNG buffers over a rect.
function regionMeanAbsDiff(bufA, bufB, rect) {
  const a = PNG.sync.read(bufA), b = PNG.sync.read(bufB);
  const width = Math.min(a.width, b.width), height = Math.min(a.height, b.height);
  const x0 = Math.max(0, Math.floor(rect.x)), y0 = Math.max(0, Math.floor(rect.y));
  const x1 = Math.min(width, Math.ceil(rect.x + rect.w)), y1 = Math.min(height, Math.ceil(rect.y + rect.h));
  let sum = 0, n = 0;
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) {
      const i = (y * width + x) * 4;
      sum += Math.abs(a.data[i] - b.data[i]) + Math.abs(a.data[i + 1] - b.data[i + 1]) + Math.abs(a.data[i + 2] - b.data[i + 2]);
      n += 3;
    }
  }
  return n ? sum / n : 0;
}

async function launch(viewport) {
  const browser = await chromium.launch({
    executablePath: CHROME,
    headless: true,
    args: FLAGS,
  });
  const context = await browser.newContext({
    viewport: viewport || { width: 1000, height: 640 },
    deviceScaleFactor: 1,
  });
  const page = await context.newPage();
  return { browser, context, page };
}

// Navigate + wait until the canvas is revealed (opacity 1) AND the wasm bridge is
// live (main() has run and ui_list_params returns a non-empty surface).
async function waitReady(page, url, timeoutMs) {
  timeoutMs = timeoutMs || 60000;
  await page.goto(url, { waitUntil: "load", timeout: timeoutMs });
  await page.waitForFunction(
    () => {
      const c = document.getElementById("canvas");
      if (!c) return false;
      const cs = getComputedStyle(c);
      if (!(c.width > 0 && c.height > 0 && parseFloat(cs.opacity) > 0.99)) return false;
      // Gate on runtime init (shell sets __runtimeReady) — calling an export
      // before init trips an -sASSERTIONS abort that halts the module + main().
      if (!window.__runtimeReady) return false;
      try {
        return (
          !!(window.Module && Module.ccall) &&
          JSON.parse(Module.ccall("ui_list_params", "string", [], [])).length > 0
        );
      } catch (e) {
        return false;
      }
    },
    { timeout: timeoutMs }
  );
}

async function probeWebGL(page) {
  return page.evaluate(() => {
    try {
      const t = document.createElement("canvas");
      const gl = t.getContext("webgl2") || t.getContext("webgl");
      if (!gl) return { ok: false };
      const dbg = gl.getExtension("WEBGL_debug_renderer_info");
      return {
        ok: true,
        version: gl.getParameter(gl.VERSION),
        renderer: dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER),
      };
    } catch (e) {
      return { ok: false, err: String(e) };
    }
  });
}

module.exports = { CHROME, FLAGS, analyzePNG, notBlank, analyzeRegion, regionMeanAbsDiff, launch, waitReady, probeWebGL };

// Standalone use: `node drive.js <url> <out.png>` — load + non-blank check.
if (require.main === module) {
  (async () => {
    const url = process.argv[2] || "http://127.0.0.1:8080/index.html";
    const out = process.argv[3] || "gallery.png";
    const { browser, page } = await launch();
    const errors = [];
    page.on("pageerror", (e) => errors.push("pageerror: " + e.message));
    await waitReady(page, url);
    await page.waitForTimeout(400);
    const buf = await page.locator("#canvas").screenshot({ path: out });
    const stats = analyzePNG(buf);
    console.log(JSON.stringify({ url, out, stats, notBlank: notBlank(stats), errors }, null, 2));
    await browser.close();
    process.exit(notBlank(stats) && errors.length === 0 ? 0 : 3);
  })().catch((e) => { console.error("DRIVER_FATAL:", e); process.exit(2); });
}
