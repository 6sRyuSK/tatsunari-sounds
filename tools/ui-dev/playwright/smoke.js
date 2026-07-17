// Factory UI · Visage — headless smoke verification (plain node, no test runner).
//
//   node smoke.js [url] [outDir]
//
// Proves the daily loop end-to-end:
//   1. gallery loads under headless SwiftShader WebGL, bridge is live
//   2. store round-trip: ui.set(depth,55) -> ui.get(depth) == 55
//   3. a simulated knob drag changes the bound store parameter
//   4. gallery.png captured + asserted non-blank (the kawaii look renders)
//   5. hot reload: ui.reloadTheme() with a new accent -> theme-edit.png,
//      proving theme -> pixel WITHOUT a rebuild; malformed theme is rejected
//
// Exit code 0 iff every assert passes.
const fs = require("fs");
const path = require("path");
const d = require("./drive.js");

const URL = process.argv[2] || "http://127.0.0.1:8080/index.html";
const OUT = process.argv[3] || __dirname;

const asserts = [];
function check(name, ok, detail) {
  asserts.push({ name, ok: !!ok, detail: detail === undefined ? "" : detail });
  console.log((ok ? "PASS " : "FAIL ") + name + (detail !== undefined ? "  (" + detail + ")" : ""));
}
const approx = (a, b, eps) => Math.abs(a - b) <= (eps === undefined ? 0.5 : eps);

(async () => {
  const { browser, page } = await d.launch();
  // Track real faults precisely: JS exceptions/crashes, and HTTP >=400 for any
  // resource other than the browser's benign auto /favicon.ico request. (bgfx's
  // SwiftShader capability-probe warnings are console WARNINGS, not errors.)
  const jsErrors = [];
  const httpErrors = [];
  page.on("pageerror", (e) => jsErrors.push("pageerror: " + e.message));
  page.on("crash", () => jsErrors.push("PAGE CRASHED"));
  page.on("response", (r) => {
    if (r.status() >= 400 && !r.url().includes("favicon"))
      httpErrors.push(r.status() + " " + r.url());
  });

  const timings = {};
  const t0 = Date.now();
  await d.waitReady(page, URL);
  timings.loadToReadyMs = Date.now() - t0;

  const webgl = await d.probeWebGL(page);
  const params = await page.evaluate(() => window.ui.list());
  check("bridge lists params", Array.isArray(params) && params.length === 5,
        (params || []).map((p) => p.id).join(","));

  // Deterministic capture: no continuous animation in P2a, but exercise freeze.
  await page.evaluate(() => window.ui.freeze(true));

  // --- 2. store round-trip -------------------------------------------------
  const rt = await page.evaluate(() => {
    window.ui.set("depth", 55);
    return window.ui.get("depth");
  });
  check("store round-trip depth=55 -> readback", approx(rt, 55), "read " + rt);

  // --- 3. simulated knob drag changes the store ----------------------------
  const drag = await page.evaluate(async () => {
    window.ui.set("depth", 30); // leave headroom to increase
    const before = window.ui.get("depth");
    const c = document.getElementById("canvas");
    const rect = c.getBoundingClientRect();
    const scaleX = rect.width / c.width;
    const scaleY = rect.height / c.height;
    const wx = window.ui.widgetX("depth");
    const wy = window.ui.widgetY("depth");
    return {
      before,
      pageX: rect.left + wx * scaleX,
      pageY: rect.top + wy * scaleY,
      dyPx: 70 * scaleY, // drag UP this many device px (increases value)
      wx, wy,
    };
  });
  // Real mouse events through visage's hit-testing -> Knob::mouseDrag -> store.
  await page.mouse.move(drag.pageX, drag.pageY);
  await page.mouse.down();
  for (let s = 1; s <= 6; s++)
    await page.mouse.move(drag.pageX, drag.pageY - (drag.dyPx * s) / 6);
  await page.mouse.up();
  await page.waitForTimeout(150);
  const after = await page.evaluate(() => window.ui.get("depth"));
  check("knob drag moved bound param", after !== drag.before, "before " + drag.before + " -> after " + after);
  check("drag up increased value", after > drag.before, drag.before + " -> " + after);

  // --- 4. hero screenshot + non-blank --------------------------------------
  await page.evaluate(() => {
    window.ui.set("depth", 68);
    window.ui.set("time", 320);
    window.ui.set("mix", 42);
    window.ui.set("sync", 1);
    window.ui.set("wide", 1);
  });
  await page.waitForTimeout(200);
  const galleryPath = path.join(OUT, "gallery.png");
  const galleryBuf = await page.locator("#canvas").screenshot({ path: galleryPath });
  const galleryStats = d.analyzePNG(galleryBuf);
  check("gallery.png non-blank", d.notBlank(galleryStats), JSON.stringify(galleryStats));

  // --- 5. hot reload theme -> pixel (no rebuild) ---------------------------
  const reload = await page.evaluate(async () => {
    const theme = await (await fetch("theme.json", { cache: "no-store" })).json();
    theme.palette.accent = "#ff33c8a6"; // coral -> mint-teal, a big visible change
    const tR = performance.now();
    const ok = window.ui.reloadTheme(JSON.stringify(theme));
    const applyMs = performance.now() - tR;
    return { ok, applyMs, accent: window.ui.accent() >>> 0 };
  });
  check("theme hot-reload accepted", reload.ok === true);
  check("theme accent applied", reload.accent === (0xff33c8a6 >>> 0),
        "0x" + reload.accent.toString(16));
  timings.themeReloadBridgeMs = +reload.applyMs.toFixed(2);
  await page.waitForTimeout(200);
  const themeEditPath = path.join(OUT, "theme-edit.png");
  const themeBuf = await page.locator("#canvas").screenshot({ path: themeEditPath });
  const themeStats = d.analyzePNG(themeBuf);
  check("theme-edit.png non-blank", d.notBlank(themeStats), JSON.stringify(themeStats));

  // Malformed theme must be rejected (strict parser) and leave the theme intact.
  const bad = await page.evaluate(() => {
    const ok = window.ui.reloadTheme('{ "palette": { "accent": "not-a-colour" } }');
    return { ok, err: window.ui.lastError(), accentStillTeal: (window.ui.accent() >>> 0) === (0xff33c8a6 >>> 0) };
  });
  check("malformed theme rejected", bad.ok === false && bad.err.length > 0, bad.err);
  check("rejected reload kept previous theme", bad.accentStillTeal);

  check("no JS/HTTP errors", jsErrors.length === 0 && httpErrors.length === 0,
        jsErrors.concat(httpErrors).slice(0, 4).join(" | "));

  await browser.close();

  const passed = asserts.every((a) => a.ok);
  const result = {
    url: URL, passed, webgl, timings,
    screenshots: { gallery: galleryPath, themeEdit: themeEditPath },
    galleryStats, themeStats,
    asserts, jsErrorCount: jsErrors.length, httpErrorCount: httpErrors.length,
  };
  fs.writeFileSync(path.join(OUT, "smoke_result.json"), JSON.stringify(result, null, 2));
  console.log("\n" + JSON.stringify({ passed, timings, webgl: webgl.renderer }, null, 2));
  process.exit(passed ? 0 : 1);
})().catch((e) => { console.error("SMOKE_FATAL:", e); process.exit(2); });
