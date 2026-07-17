// Factory UI · Visage — headless smoke verification (plain node, no test runner).
//
//   node smoke.js [url] [outDir]
//
// Proves the daily loop end-to-end for the P2a foundation AND the P2b widget set:
//   1. gallery loads under headless SwiftShader WebGL, bridge is live (9 params)
//   2. store round-trip + a simulated knob drag move the bound store parameter
//   3. gallery.png non-blank + theme hot reload -> pixel (theme-edit.png)
//   4. P2b interactions through real mouse events + the bridge:
//        - Segmented click            -> Choice param ("mode") changes
//        - LinkSlider drag            -> Float  param ("link") changes
//        - PresetSelectorView < / >   -> onChange (presetIndex steps, skips "Save As")
//        - ValueSetting -> Dropdown open + row click -> Choice param ("quality") changes
//        - Spectrum region non-blank + CHANGES between two synthetic frames, then
//          freezes deterministically (two frozen frames identical)
//   5. captures gallery2.png (extended gallery, frozen) + dropdown.png (open)
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
const wait = (p, ms) => p.waitForTimeout(ms);

// window px (canvas buffer coords) -> page px (accounts for the canvas offset/scale).
async function toPage(page, wx, wy) {
  return page.evaluate(({ wx, wy }) => {
    const c = document.getElementById("canvas");
    const r = c.getBoundingClientRect();
    return { x: r.left + wx * (r.width / c.width), y: r.top + wy * (r.height / c.height) };
  }, { wx, wy });
}
// Explicit move -> down -> up with small pauses. Visage's single-threaded wasm
// event loop is rAF-driven, so an instantaneous mouse.click can land its down
// before the move's hit-test is processed; pacing the phases fixes it (the same
// pattern the P2a knob-drag used).
async function clickWindow(page, wx, wy) {
  const p = await toPage(page, wx, wy);
  await page.mouse.move(p.x, p.y);
  await page.waitForTimeout(40);
  await page.mouse.down();
  await page.waitForTimeout(40);
  await page.mouse.up();
  await page.waitForTimeout(40);
}
const canvasShot = (page) => page.locator("#canvas").screenshot();

(async () => {
  // Viewport tall enough to hold the 780x720 canvas (spectrum sits low).
  const { browser, page } = await d.launch({ width: 1000, height: 820 });
  const jsErrors = [];
  const httpErrors = [];
  page.on("pageerror", (e) => jsErrors.push("pageerror: " + e.message));
  page.on("crash", () => jsErrors.push("PAGE CRASHED"));
  page.on("response", (r) => {
    if (r.status() >= 400 && !r.url().includes("favicon")) httpErrors.push(r.status() + " " + r.url());
  });

  const timings = {};
  const t0 = Date.now();
  await d.waitReady(page, URL);
  timings.loadToReadyMs = Date.now() - t0;

  const webgl = await d.probeWebGL(page);
  const params = await page.evaluate(() => window.ui.list());
  check("bridge lists params (9)", Array.isArray(params) && params.length === 9,
        (params || []).map((p) => p.id).join(","));

  // Freeze -> the spectrum holds a deterministic frame behind the static widgets.
  await page.evaluate(() => window.ui.freeze(true));

  // --- 2. store round-trip + knob drag (P2a) -------------------------------
  const rt = await page.evaluate(() => { window.ui.set("depth", 55); return window.ui.get("depth"); });
  check("store round-trip depth=55 -> readback", approx(rt, 55), "read " + rt);

  const drag = await page.evaluate(() => {
    window.ui.set("depth", 30);
    return { before: window.ui.get("depth"), wx: window.ui.widgetX("depth"), wy: window.ui.widgetY("depth") };
  });
  {
    const p = await toPage(page, drag.wx, drag.wy);
    const dy = 70 * (await page.evaluate(() => { const c = document.getElementById("canvas"); return c.getBoundingClientRect().height / c.height; }));
    await page.mouse.move(p.x, p.y);
    await page.mouse.down();
    for (let s = 1; s <= 6; s++) await page.mouse.move(p.x, p.y - (dy * s) / 6);
    await page.mouse.up();
  }
  await wait(page, 120);
  const afterDrag = await page.evaluate(() => window.ui.get("depth"));
  check("knob drag increased bound param", afterDrag > drag.before, drag.before + " -> " + afterDrag);

  // --- 3. hero gallery.png + theme hot reload (P2a) ------------------------
  await page.evaluate(() => {
    window.ui.set("depth", 68); window.ui.set("time", 320); window.ui.set("mix", 42);
    window.ui.set("sync", 1); window.ui.set("wide", 1);
  });
  await wait(page, 150);
  const galleryBuf = await canvasShot(page);
  fs.writeFileSync(path.join(OUT, "gallery.png"), galleryBuf);
  check("gallery.png non-blank", d.notBlank(d.analyzePNG(galleryBuf)));

  const reload = await page.evaluate(() => {
    const theme = JSON.parse(JSON.stringify(window.__lastTheme || {}));
    return fetch("theme.json", { cache: "no-store" }).then((r) => r.json()).then((t) => {
      t.palette.accent = "#ff33c8a6";
      const ok = window.ui.reloadTheme(JSON.stringify(t));
      return { ok, accent: window.ui.accent() >>> 0 };
    });
  });
  check("theme hot-reload accepted", reload.ok === true);
  check("theme accent applied", reload.accent === (0xff33c8a6 >>> 0), "0x" + reload.accent.toString(16));
  await wait(page, 120);
  fs.writeFileSync(path.join(OUT, "theme-edit.png"), await canvasShot(page));
  const bad = await page.evaluate(() => {
    const ok = window.ui.reloadTheme('{ "palette": { "accent": "not-a-colour" } }');
    return { ok, err: window.ui.lastError() };
  });
  check("malformed theme rejected", bad.ok === false && bad.err.length > 0, bad.err);
  // Restore the factory theme (coral) for the hero shots.
  await page.evaluate(() => fetch("theme.json", { cache: "no-store" }).then((r) => r.text()).then((t) => window.ui.reloadTheme(t)));
  await wait(page, 120);

  // --- 4a. Segmented click -> Choice param ---------------------------------
  const modeRect = await page.evaluate(() => window.ui.widgetRect("mode"));
  const modeBefore = await page.evaluate(() => window.ui.get("mode"));
  // 3 segments; click the middle one (index 1 = "Hard").
  await clickWindow(page, modeRect.x + modeRect.w * 0.5, modeRect.y + modeRect.h * 0.5);
  await wait(page, 80);
  const modeAfter = await page.evaluate(() => window.ui.get("mode"));
  check("segmented click changed choice param", modeAfter === 1 && modeAfter !== modeBefore,
        "mode " + modeBefore + " -> " + modeAfter);

  // --- 4b. LinkSlider drag -> Float param ----------------------------------
  const linkRect = await page.evaluate(() => window.ui.widgetRect("link"));
  const linkBefore = await page.evaluate(() => window.ui.get("link"));
  {
    const c = { x: linkRect.x + linkRect.w * 0.7, y: linkRect.y + linkRect.h * 0.5 };
    const p0 = await toPage(page, c.x, c.y);
    const p1 = await toPage(page, c.x - 50, c.y); // drag left -> decrease
    await page.mouse.move(p0.x, p0.y);
    await wait(page, 40);
    await page.mouse.down();
    await wait(page, 40);
    await page.mouse.move((p0.x + p1.x) / 2, p0.y);
    await wait(page, 30);
    await page.mouse.move(p1.x, p1.y);
    await wait(page, 30);
    await page.mouse.up();
  }
  await wait(page, 100);
  const linkAfter = await page.evaluate(() => window.ui.get("link"));
  check("link slider drag changed float param", linkAfter < linkBefore - 1, linkBefore + " -> " + linkAfter);

  // --- 4c. Preset selector next / prev -> onChange -------------------------
  const presetRect = await page.evaluate(() => window.ui.widgetRect("preset"));
  const arrowW = Math.min(24, presetRect.h);
  const nextX = presetRect.x + presetRect.w - arrowW * 0.5;
  const prevX = presetRect.x + arrowW * 0.5;
  const midY = presetRect.y + presetRect.h * 0.5;
  const p0 = await page.evaluate(() => window.ui.presetIndex());
  await clickWindow(page, nextX, midY); await wait(page, 60);
  const p1 = await page.evaluate(() => window.ui.presetIndex());
  await clickWindow(page, prevX, midY); await wait(page, 60);
  const p2 = await page.evaluate(() => window.ui.presetIndex());
  check("preset next stepped forward", p1 === p0 + 1, p0 + " -> " + p1);
  check("preset prev stepped back", p2 === p0, p1 + " -> " + p2);

  // --- 4d. ValueSetting -> Dropdown open + row select ----------------------
  const qBefore = await page.evaluate(() => window.ui.get("quality"));
  const vopen = await page.evaluate(() => { const ok = window.ui.openDropdown(1); return { ok, open: window.ui.dropdownOpen(), count: window.ui.dropdownCount() }; });
  check("value-setting dropdown opened", vopen.ok && vopen.open && vopen.count === 4, JSON.stringify(vopen));
  // Click item index 3 ("Ultra").
  const rowPt = await page.evaluate(() => ({ x: window.ui.dropdownX(3), y: window.ui.dropdownRowY(3) }));
  await clickWindow(page, rowPt.x, rowPt.y);
  await wait(page, 80);
  const qAfter = await page.evaluate(() => ({ q: window.ui.get("quality"), open: window.ui.dropdownOpen() }));
  check("dropdown row select changed choice param", qAfter.q === 3 && qBefore !== 3, qBefore + " -> " + qAfter.q);
  check("dropdown closed after select", qAfter.open === false);

  // --- 4e. dropdown.png: the PRESET dropdown (header + separator + Save As) --
  const dOpen = await page.evaluate(() => { const ok = window.ui.openDropdown(0); return ok && window.ui.dropdownOpen(); });
  check("preset dropdown opened for capture", dOpen === true);
  await wait(page, 100);
  const dropBuf = await canvasShot(page);
  fs.writeFileSync(path.join(OUT, "dropdown.png"), dropBuf);
  check("dropdown.png non-blank", d.notBlank(d.analyzePNG(dropBuf)));
  // Dismiss with an outside click (top-left card title area).
  await clickWindow(page, 120, 40);
  await wait(page, 60);
  check("dropdown dismissed by outside click", (await page.evaluate(() => window.ui.dropdownOpen())) === false);

  // --- 4f. Spectrum: animate -> changes -> freeze deterministically --------
  const specRect = await page.evaluate(() => window.ui.widgetRect("spectrum"));
  await page.evaluate(() => window.ui.freeze(false)); // resume animation
  await wait(page, 250);
  const specA = await canvasShot(page);
  await wait(page, 400);
  const specB = await canvasShot(page);
  const madAnim = d.regionMeanAbsDiff(specA, specB, specRect);
  check("spectrum region non-blank", d.notBlank(d.analyzeRegion(specA, specRect)), JSON.stringify(d.analyzeRegion(specA, specRect)));
  check("spectrum CHANGES between two frames", madAnim > 1.0, "meanAbsDiff " + madAnim.toFixed(2));

  // Freeze + inject a fixed frame -> deterministic (two captures identical).
  await page.evaluate(() => { window.ui.freeze(true); window.ui.feedSpectrum(0.35); });
  await wait(page, 150);
  const frz1 = await canvasShot(page);
  await page.evaluate(() => window.ui.feedSpectrum(0.35));
  await wait(page, 150);
  const frz2 = await canvasShot(page);
  const madFrozen = d.regionMeanAbsDiff(frz1, frz2, specRect);
  check("frozen spectrum deterministic", madFrozen < 0.05, "meanAbsDiff " + madFrozen.toFixed(4));

  // --- 5. gallery2.png: the full extended gallery, frozen ------------------
  fs.writeFileSync(path.join(OUT, "gallery2.png"), frz2);
  check("gallery2.png non-blank", d.notBlank(d.analyzePNG(frz2)));

  check("no JS/HTTP errors", jsErrors.length === 0 && httpErrors.length === 0,
        jsErrors.concat(httpErrors).slice(0, 4).join(" | "));

  await browser.close();

  const passed = asserts.every((a) => a.ok);
  const result = {
    url: URL, passed, webgl, timings,
    screenshots: ["gallery.png", "gallery2.png", "dropdown.png", "theme-edit.png"].map((f) => path.join(OUT, f)),
    spectrum: { animMeanAbsDiff: +madAnim.toFixed(2), frozenMeanAbsDiff: +madFrozen.toFixed(4) },
    asserts, jsErrorCount: jsErrors.length, httpErrorCount: httpErrors.length,
  };
  fs.writeFileSync(path.join(OUT, "smoke_result.json"), JSON.stringify(result, null, 2));
  console.log("\n" + JSON.stringify({ passed, timings, webgl: webgl.renderer }, null, 2));
  process.exit(passed ? 0 : 1);
})().catch((e) => { console.error("SMOKE_FATAL:", e); process.exit(2); });
