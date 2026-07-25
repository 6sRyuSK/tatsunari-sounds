// Reusable Playwright helpers for the Visage gallery (adapted from spike S1).
//
// Chromium resolves from CHROME_BIN, Playwright's managed browser, or a common
// system Chrome/Edge install. WebGL in headless mode uses the SwiftShader flags
// below — the load-bearing
// flag is --enable-unsafe-swiftshader (modern Chromium refuses SwiftShader for
// WebGL without it, and the canvas comes up blank).
const { chromium } = require("playwright");
const { PNG } = require("pngjs");
const fs = require("fs");
const path = require("path");

function firstExisting(candidates) {
  for (const candidate of candidates.filter(Boolean)) {
    if (fs.existsSync(candidate)) return candidate;
  }
  return null;
}

// Any Chromium build already sitting in PLAYWRIGHT_BROWSERS_PATH, newest revision
// first. Pre-provisioned images (the agent sandbox pins one at /opt/pw-browsers)
// rarely carry the exact revision our package-lock's Playwright asks for, and the
// mismatch is not a reason to download a second copy. Scanning beats hardcoding a
// revision, which silently rots the moment either side moves.
function browsersPathChromium() {
  const root = process.env.PLAYWRIGHT_BROWSERS_PATH;
  if (!root || !fs.existsSync(root)) return null;
  let entries;
  try {
    entries = fs.readdirSync(root).filter((name) => name.startsWith("chromium-"));
  } catch (error) {
    return null;
  }
  const revision = (name) => Number(name.slice("chromium-".length)) || 0;
  entries.sort((a, b) => revision(b) - revision(a));
  // chrome-linux64 is the modern layout, chrome-linux the older one.
  const relative = process.platform === "win32"
    ? [path.join("chrome-win", "chrome.exe")]
    : process.platform === "darwin"
    ? [path.join("chrome-mac", "Chromium.app", "Contents", "MacOS", "Chromium")]
    : [path.join("chrome-linux64", "chrome"), path.join("chrome-linux", "chrome")];
  for (const entry of entries) {
    const found = firstExisting(relative.map((suffix) => path.join(root, entry, suffix)));
    if (found) return found;
  }
  return null;
}

// Prefer Playwright's pinned browser, but keep the harness usable on developer
// machines that already have Chrome/Edge and intentionally skipped the browser
// download. CHROME_BIN always wins and is validated so failures are actionable.
function resolveChromiumExecutable() {
  if (process.env.CHROME_BIN) {
    const explicit = path.resolve(process.env.CHROME_BIN);
    if (!fs.existsSync(explicit)) {
      throw new Error(`CHROME_BIN does not exist: ${explicit}`);
    }
    return { path: explicit, source: "CHROME_BIN", available: true };
  }

  const managed = chromium.executablePath();
  if (managed && fs.existsSync(managed)) {
    return { path: managed, source: "playwright", available: true };
  }

  const provisioned = browsersPathChromium();
  if (provisioned) {
    return { path: provisioned, source: "PLAYWRIGHT_BROWSERS_PATH", available: true };
  }

  const localAppData = process.env.LOCALAPPDATA;
  const programFiles = process.env.ProgramFiles;
  const programFilesX86 = process.env["ProgramFiles(x86)"];
  const home = process.env.HOME;
  const system = firstExisting(process.platform === "win32" ? [
    localAppData && path.join(localAppData, "Google", "Chrome", "Application", "chrome.exe"),
    programFiles && path.join(programFiles, "Google", "Chrome", "Application", "chrome.exe"),
    programFilesX86 && path.join(programFilesX86, "Google", "Chrome", "Application", "chrome.exe"),
    programFiles && path.join(programFiles, "Microsoft", "Edge", "Application", "msedge.exe"),
    programFilesX86 && path.join(programFilesX86, "Microsoft", "Edge", "Application", "msedge.exe"),
  ] : process.platform === "darwin" ? [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
    home && path.join(home, "Applications", "Google Chrome.app", "Contents", "MacOS", "Google Chrome"),
  ] : [
    "/usr/bin/google-chrome",
    "/usr/bin/google-chrome-stable",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
  ]);
  return system
    ? { path: system, source: "system", available: true }
    : { path: null, source: "missing", available: false };
}

const BROWSER = resolveChromiumExecutable();
const CHROME = BROWSER.path;

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

// Mean RGB over a small (2*rad+1)² neighbourhood around (x,y) in a PNG buffer
// (window px == canvas buffer px for a #canvas locator screenshot). Robust to a
// 1px anti-aliased seam; used to assert a knob ring zone's solid colour.
function samplePixel(buf, x, y, rad) {
  const png = PNG.sync.read(buf);
  const { width, height, data } = png;
  rad = rad === undefined ? 1 : rad;
  const cx = Math.round(x), cy = Math.round(y);
  let r = 0, g = 0, b = 0, n = 0;
  for (let yy = cy - rad; yy <= cy + rad; yy++) {
    for (let xx = cx - rad; xx <= cx + rad; xx++) {
      if (xx < 0 || yy < 0 || xx >= width || yy >= height) continue;
      const i = (yy * width + xx) * 4;
      r += data[i]; g += data[i + 1]; b += data[i + 2]; n++;
    }
  }
  return n ? { r: Math.round(r / n), g: Math.round(g / n), b: Math.round(b / n) } : null;
}
// Max per-channel abs difference between two {r,g,b} colours.
function colorDist(a, b) {
  return Math.max(Math.abs(a.r - b.r), Math.abs(a.g - b.g), Math.abs(a.b - b.b));
}

async function launch(viewport) {
  if (!BROWSER.available) {
    throw new Error(
      "No Chromium executable was found. Run `npx playwright install chromium` " +
      "in tools/ui-dev/playwright, or set CHROME_BIN."
    );
  }
  const browser = await chromium.launch({
    executablePath: CHROME,
    headless: process.env.PW_HEADLESS !== "0",
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
  try {
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
      undefined,
      { timeout: timeoutMs }
    );
  } catch (error) {
    const state = await page.evaluate(() => {
      const canvas = document.getElementById("canvas");
      const status = document.getElementById("status");
      return {
        documentReady: document.readyState,
        runtimeReady: !!window.__runtimeReady,
        moduleCcall: !!(window.Module && Module.ccall),
        canvas: canvas ? {
          width: canvas.width,
          height: canvas.height,
          opacity: getComputedStyle(canvas).opacity,
        } : null,
        status: status ? status.textContent : null,
      };
    }).catch((diagnosticError) => ({ diagnosticError: String(diagnosticError) }));
    throw new Error(`Visage UI did not become ready within ${timeoutMs}ms: ${JSON.stringify(state)}`, { cause: error });
  }
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

function browserInfo() {
  return { ...BROWSER };
}

module.exports = { CHROME, FLAGS, browserInfo, analyzePNG, notBlank, analyzeRegion, regionMeanAbsDiff, samplePixel, colorDist, launch, waitReady, probeWebGL };

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
