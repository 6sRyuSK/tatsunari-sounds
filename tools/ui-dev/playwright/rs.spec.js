// Resonance TatSuppressor · Visage editor — headless verification (plain node).
//
//   node rs.spec.js [url] [outDir]
//
// Drives the RS editor end-to-end under headless SwiftShader WebGL and asserts:
//   1. bridge lists the full 64-param RS surface
//   2. every main knob is bound (host-set -> readback) + a real knob DRAG moves it
//   3. MODE segmented click + QUALITY dropdown pick change their Choice params
//   4. DELTA / LINK / Bypass pill toggles flip their bool params
//   5. a node DRAG on the curve moves b0_freq / b0_sens
//   6. clicking a node opens the NodePanel; a panel TYPE button writes b0_type
//   7. the NodePanel Listen badge writes listenNode (via the RsFeed hook); deselect drops it
//   8. an undo -> redo round-trip restores a parameter, and a preset load clears history
//   9. preset next / prev step the model; an A/B switch swaps a param value
//  10. resize renders at the min (940x657) and max (1320x922) layout sizes
//  11. node-on-curve: an isolated band's dot rides the combined profile curve
//  12. knob three-zone donut: the ring reads accent / accentDim / panelLo
//  13. mini-knob needle angle: the SENS mini-knob points at its value's angle
//      (shared value->angle mapping, ~+33 deg from top for 7.40 dB / [-30,30])
// and captures rs-default.png / rs-busy.png / rs-min.png / rs-max.png / rs-knob-depth.png.
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

async function toPage(page, wx, wy) {
  return page.evaluate(({ wx, wy }) => {
    const c = document.getElementById("canvas");
    const r = c.getBoundingClientRect();
    return { x: r.left + wx * (r.width / c.width), y: r.top + wy * (r.height / c.height) };
  }, { wx, wy });
}
async function clickWindow(page, wx, wy) {
  const p = await toPage(page, wx, wy);
  await page.mouse.move(p.x, p.y); await page.waitForTimeout(35);
  await page.mouse.down(); await page.waitForTimeout(35);
  await page.mouse.up(); await page.waitForTimeout(35);
}
// Alt+click (Alt held across the whole press) — drives the alt-click-to-default
// reset on knobs / nodes (fix 5). Emscripten forwards event.altKey -> kModifierAlt.
async function altClickWindow(page, wx, wy) {
  const p = await toPage(page, wx, wy);
  await page.keyboard.down("Alt");
  await page.mouse.move(p.x, p.y); await page.waitForTimeout(35);
  await page.mouse.down(); await page.waitForTimeout(35);
  await page.mouse.up(); await page.waitForTimeout(35);
  await page.keyboard.up("Alt");
}
async function dragWindow(page, x0, y0, x1, y1) {
  const a = await toPage(page, x0, y0), b = await toPage(page, x1, y1);
  await page.mouse.move(a.x, a.y); await page.waitForTimeout(30);
  await page.mouse.down(); await page.waitForTimeout(30);
  await page.mouse.move((a.x + b.x) / 2, (a.y + b.y) / 2); await page.waitForTimeout(25);
  await page.mouse.move(b.x, b.y); await page.waitForTimeout(25);
  await page.mouse.up(); await page.waitForTimeout(40);
}
const canvasShot = (page) => page.locator("#canvas").screenshot();
// The canvas is fixed at the max size (1320x922) and the editor renders into its
// top-left (w x h) sub-rect, so screenshots are clipped to the editor rect.
async function editorShot(page, w, h) {
  const box = await page.evaluate(() => {
    const c = document.getElementById("canvas");
    const r = c.getBoundingClientRect();
    return { left: r.left, top: r.top };
  });
  return page.screenshot({ clip: { x: box.left, y: box.top, width: w, height: h } });
}
const rectCentre = (r) => ({ x: r.x + r.w * 0.5, y: r.y + r.h * 0.5 });

(async () => {
  const { browser, page } = await d.launch({ width: 1420, height: 1000 });
  const jsErrors = [], httpErrors = [];
  page.on("pageerror", (e) => jsErrors.push("pageerror: " + e.message));
  page.on("crash", () => jsErrors.push("PAGE CRASHED"));
  page.on("response", (r) => { if (r.status() >= 400 && !r.url().includes("favicon")) httpErrors.push(r.status() + " " + r.url()); });

  const timings = {};
  const t0 = Date.now();
  await d.waitReady(page, URL);
  timings.loadToReadyMs = Date.now() - t0;
  const webgl = await d.probeWebGL(page);

  const params = await page.evaluate(() => window.ui.list());
  check("bridge lists the RS param surface (64)", Array.isArray(params) && params.length === 64, "n=" + (params || []).length);

  // Freeze the analyser -> deterministic behind the static chrome for all tests.
  await page.evaluate(() => window.ui.freeze(true));
  await wait(page, 120);

  // --- rs-default.png ------------------------------------------------------
  const defBuf = await editorShot(page, 1069, 747);
  fs.writeFileSync(path.join(OUT, "rs-default.png"), defBuf);
  check("rs-default.png non-blank", d.notBlank(d.analyzePNG(defBuf)));

  // --- 1. undo -> redo round-trip + preset clears history ------------------
  await page.evaluate(() => { window.rs.setClock(0); window.rs.uiEdit("depth", 40); });
  await page.evaluate(() => { window.rs.setClock(1); window.rs.uiEdit("depth", 70); });
  const uAfterEdits = await page.evaluate(() => ({ v: window.ui.get("depth"), canU: window.rs.canUndo() }));
  await page.evaluate(() => window.rs.undo());
  const uUndo = await page.evaluate(() => window.ui.get("depth"));
  await page.evaluate(() => window.rs.redo());
  const uRedo = await page.evaluate(() => window.ui.get("depth"));
  check("undo restores previous param value", approx(uUndo, 40), "70 -> " + uUndo);
  check("redo re-applies param value", approx(uRedo, 70), uUndo + " -> " + uRedo);
  check("edits are undoable", uAfterEdits.canU === true);
  await page.evaluate(() => window.rs.presetLoad(0)); // Init -> resets + clears undo
  const clearedU = await page.evaluate(() => window.rs.canUndo());
  check("preset load clears undo history", clearedU === false);

  // --- 2. all main knobs bound (host-set -> readback) ----------------------
  const knobSet = { depth: 55, detail: 60, attack: 25, release: 120, tilt: 30, mix: 80, out: 6, linkAmt: 40 };
  const rb = await page.evaluate((ks) => { const o = {}; for (const k in ks) { window.ui.set(k, ks[k]); o[k] = window.ui.get(k); } return o; }, knobSet);
  let allBound = true; for (const k in knobSet) if (!approx(rb[k], knobSet[k], 0.6)) allBound = false;
  check("all main knobs bound (set -> readback)", allBound, JSON.stringify(rb));

  // knob DRAG (Depth) moves the store value
  {
    const r = await page.evaluate(() => window.ui.widgetRect("depth"));
    const c = rectCentre(r);
    await page.evaluate(() => window.ui.set("depth", 30));
    const before = await page.evaluate(() => window.ui.get("depth"));
    await dragWindow(page, c.x, c.y, c.x, c.y - 70); // drag up = increase
    const after = await page.evaluate(() => window.ui.get("depth"));
    check("knob drag increased the bound param", after > before + 1, before + " -> " + after);
  }

  // --- 3. MODE segmented + QUALITY dropdown --------------------------------
  {
    const r = await page.evaluate(() => window.ui.widgetRect("mode"));
    const before = await page.evaluate(() => window.ui.get("mode"));
    await clickWindow(page, r.x + r.w * 0.75, r.y + r.h * 0.5); // 2nd segment "Hard"
    const after = await page.evaluate(() => window.ui.get("mode"));
    check("MODE segmented click changed choice", after === 1 && after !== before, before + " -> " + after);
  }
  {
    const before = await page.evaluate(() => window.ui.get("quality"));
    const open = await page.evaluate(() => { const ok = window.rs.openDropdown(0); return { ok, open: window.rs.dropdownOpen(), n: window.rs.dropdownCount() }; });
    check("QUALITY dropdown opened", open.ok && open.open && open.n === 3, JSON.stringify(open));
    const row = await page.evaluate(() => ({ x: window.rs.dropdownX(2), y: window.rs.dropdownRowY(2) })); // "High"
    await clickWindow(page, row.x, row.y);
    const after = await page.evaluate(() => ({ q: window.ui.get("quality"), open: window.rs.dropdownOpen() }));
    check("QUALITY dropdown pick changed choice", after.q === 2 && before !== 2, before + " -> " + after.q);
    check("QUALITY dropdown closed after pick", after.open === false);
  }

  // --- 4. DELTA / LINK / Bypass pill toggles -------------------------------
  for (const id of ["delta", "link", "bypass"]) {
    const r = await page.evaluate((k) => window.ui.widgetRect(k), id);
    const before = await page.evaluate((k) => window.ui.get(k), id);
    const c = rectCentre(r);
    await clickWindow(page, c.x, c.y);
    const after = await page.evaluate((k) => window.ui.get(k), id);
    check("pill toggle '" + id + "' flipped", after !== before, before + " -> " + after);
  }

  // --- 5. node DRAG on the curve moves b0_freq / b0_sens -------------------
  await page.evaluate(() => window.rs.selectNode(-1));
  {
    const before = await page.evaluate(() => ({ f: window.ui.get("b0_freq"), s: window.ui.get("b0_sens") }));
    const pos = await page.evaluate(() => ({ x: window.rs.nodeX(2), y: window.rs.nodeY(2) })); // node 2 = band 0
    check("band node 0 has a screen position", pos.x > 0 && pos.y > 0, JSON.stringify(pos));
    await dragWindow(page, pos.x, pos.y, pos.x + 80, pos.y - 40); // right = higher freq, up = higher sens
    const after = await page.evaluate(() => ({ f: window.ui.get("b0_freq"), s: window.ui.get("b0_sens") }));
    check("node drag changed b0_freq", after.f > before.f + 1, before.f.toFixed(0) + " -> " + after.f.toFixed(0));
    check("node drag changed b0_sens", after.s > before.s + 0.5, before.s.toFixed(1) + " -> " + after.s.toFixed(1));
  }

  // --- 6. node click opens NodePanel; a TYPE button writes b0_type ---------
  {
    await page.evaluate(() => window.rs.selectNode(2));
    const sel = await page.evaluate(() => window.rs.selectedNode());
    const pr = await page.evaluate(() => window.ui.widgetRect("nodePanel"));
    check("node select opens the NodePanel", sel === 2 && pr && pr.w > 0, "sel=" + sel);
    const before = await page.evaluate(() => window.ui.get("b0_type"));
    // TYPE button i centre (panel-local): (76 + 36*i, 71); click i=3 (Band Shelf).
    await clickWindow(page, pr.x + 76 + 36 * 3, pr.y + 71);
    const after = await page.evaluate(() => window.ui.get("b0_type"));
    check("NodePanel TYPE button wrote b0_type", after === 3 && before !== 3, before + " -> " + after);
  }

  // --- 7. NodePanel Listen badge writes listenNode ------------------------
  {
    await page.evaluate(() => window.rs.selectNode(3)); // band 1
    const pr = await page.evaluate(() => window.ui.widgetRect("nodePanel"));
    // Listen badge centre (panel-local): (211, 25).
    await clickWindow(page, pr.x + 211, pr.y + 25);
    const on = await page.evaluate(() => window.rs.listenNode());
    check("Listen badge wrote listenNode", on === 3, "listen=" + on);
    await page.evaluate(() => window.rs.selectNode(-1)); // deselect drops Listen
    const off = await page.evaluate(() => window.rs.listenNode());
    check("deselect drops Listen", off === -1, "listen=" + off);
  }

  // --- 8. preset next / prev step the model -------------------------------
  {
    const pr = await page.evaluate(() => window.ui.widgetRect("preset"));
    const arrowW = Math.min(24, pr.h);
    const nextX = pr.x + pr.w - arrowW * 0.5, prevX = pr.x + arrowW * 0.5, midY = pr.y + pr.h * 0.5;
    const p0 = await page.evaluate(() => window.rs.presetIndex());
    await clickWindow(page, nextX, midY); await wait(page, 50);
    const p1 = await page.evaluate(() => window.rs.presetIndex());
    await clickWindow(page, prevX, midY); await wait(page, 50);
    const p2 = await page.evaluate(() => window.rs.presetIndex());
    check("preset next stepped forward", p1 === p0 + 1, p0 + " -> " + p1);
    check("preset prev stepped back", p2 === p0, p1 + " -> " + p2);
  }

  // --- 9. A/B switch swaps a param value ----------------------------------
  {
    await page.evaluate(() => { window.ui.set("depth", 22); window.rs.setAb(1); window.ui.set("depth", 88); window.rs.setAb(0); });
    const a = await page.evaluate(() => window.ui.get("depth"));
    await page.evaluate(() => window.rs.setAb(1));
    const b = await page.evaluate(() => window.ui.get("depth"));
    check("A/B switch swaps the param value", approx(a, 22) && approx(b, 88), "A=" + a + " B=" + b);
  }

  // --- rs-busy.png: several bands on + a node selected + NodePanel open -----
  await page.evaluate(() => {
    window.ui.set("b2_sens", 9); window.ui.set("b4_on", 1); window.ui.set("b4_sens", 7);
    window.ui.set("b5_on", 1); window.ui.set("b5_sens", -6); window.ui.set("depth", 62);
    window.ui.freeze(true); window.rs.selectNode(4); // open panel on band 2
  });
  await wait(page, 150);
  const busyBuf = await editorShot(page, 1069, 747);
  fs.writeFileSync(path.join(OUT, "rs-busy.png"), busyBuf);
  check("rs-busy.png non-blank", d.notBlank(d.analyzePNG(busyBuf)));

  // --- 10. resize renders at min + max ------------------------------------
  await page.evaluate(() => window.rs.selectNode(-1));
  await page.evaluate(() => window.rs.setSize(940, 657));
  await wait(page, 250);
  const minBuf = await editorShot(page, 940, 657);
  fs.writeFileSync(path.join(OUT, "rs-min.png"), minBuf);
  const minStats = d.analyzePNG(minBuf);
  check("rs-min.png (940x657) non-blank", d.notBlank(minStats), minStats.width + "x" + minStats.height);

  await page.evaluate(() => window.rs.setSize(1320, 922));
  await wait(page, 250);
  const maxBuf = await editorShot(page, 1320, 922);
  fs.writeFileSync(path.join(OUT, "rs-max.png"), maxBuf);
  const maxStats = d.analyzePNG(maxBuf);
  check("rs-max.png (1320x922) non-blank", d.notBlank(maxStats), maxStats.width + "x" + maxStats.height);
  check("resize preserved the design aspect", Math.abs(940 / 657 - 1320 / 922) < 0.01);

  // --- 11. node-on-curve: a band's dot rides the combined profile curve -----
  // Isolate one band (cuts off, all other bands off) so the combined profile at
  // the band's frequency is that band's own peak == its sens; then the node
  // handle (y = sensToY(sens)) must land on the profile curve's y at that x.
  await page.evaluate(() => window.rs.selectNode(-1));
  await page.evaluate(() => window.rs.setSize(1069, 747));
  await wait(page, 200);
  {
    await page.evaluate(() => {
      window.ui.set("lc_on", 0); window.ui.set("hc_on", 0);
      for (let b = 0; b < 8; b++) window.ui.set("b" + b + "_on", 0);
      window.ui.set("b0_type", 0); window.ui.set("b0_width", 0.5);
      window.ui.set("b0_freq", 1000); window.ui.set("b0_sens", 12); window.ui.set("b0_on", 1);
      window.ui.freeze(true);
    });
    await wait(page, 150);
    const plot = await page.evaluate(() => window.rs.plotRect());
    const st = await page.evaluate(() => ({
      nf: window.ui.get("b0_freq"), ns: window.ui.get("b0_sens"),
      nodeY: window.rs.nodeY(2), curveDb: window.rs.profileDbAt(window.ui.get("b0_freq")),
    }));
    // sens axis (window px): sensToY(s) = plot.y + plot.h - (clamp(s,-30,30)+30)/60 * plot.h
    const sensToYWin = (s) => plot.y + plot.h - (Math.max(-30, Math.min(30, s)) + 30) / 60 * plot.h;
    const curveY = sensToYWin(st.curveDb);
    check("band node dot rides the combined profile curve (y within a few px)",
      st.nodeY > 0 && Math.abs(st.nodeY - curveY) <= 4.0,
      "nodeY=" + st.nodeY.toFixed(1) + " curveY=" + curveY.toFixed(1) +
      " (profileDb=" + st.curveDb.toFixed(2) + " sens=" + st.ns.toFixed(2) + ")");
  }

  // --- 12. knob three-zone donut: sample one px in each ring zone ------------
  // depth = 50 (%) -> value angle at 12 o'clock, so the ring's LEFT half is the
  // accent fill, the RIGHT half is the accentDim remainder, and the BOTTOM is the
  // panelLo dead zone. Sample the ring centreline (0.85R) at those three angles.
  {
    await page.evaluate(() => { window.ui.set("depth", 50); window.ui.freeze(true); });
    await wait(page, 150);
    const r = await page.evaluate(() => window.ui.widgetRect("depth"));
    const cx = r.x + r.w * 0.5, cy = r.y + r.h * 0.5;
    // Big-knob dial radius after the round-3 fix-7 profile: name row 16 + value row
    // 17 = 33 px reserved, 0 dial inset (fills the cell) — see Knob::draw + RsEditor.
    const R = Math.min(r.w, (r.h - 33)) * 0.5;
    const arcR = R * 0.78;                                // inside the solid ring band (clear of the AA edges)
    const shot = await canvasShot(page);
    const accent = d.samplePixel(shot, cx - arcR, cy, 1);        // left  -> accent  #ff7a6b
    const dim    = d.samplePixel(shot, cx + arcR, cy, 1);        // right -> accentDim #ffd6cd
    const dead   = d.samplePixel(shot, cx, cy + arcR, 1);        // bottom-> panelLo  #fff4ee
    const want = { accent: { r: 255, g: 122, b: 107 }, dim: { r: 255, g: 214, b: 205 }, dead: { r: 255, g: 244, b: 238 } };
    const dA = d.colorDist(accent, want.accent), dD = d.colorDist(dim, want.dim), dZ = d.colorDist(dead, want.dead);
    fs.writeFileSync(path.join(OUT, "rs-knob-depth.png"),
      await page.screenshot({ clip: { x: Math.max(0, r.x - 8), y: Math.max(0, r.y - 8), width: r.w + 16, height: r.h + 16 } }));
    check("knob ring zones read accent / accentDim / panelLo",
      dA <= 30 && dD <= 30 && dZ <= 30,
      "accent " + JSON.stringify(accent) + " d" + dA + " | dim " + JSON.stringify(dim) + " d" + dD + " | dead " + JSON.stringify(dead) + " d" + dZ);
  }

  // --- 13. mini-knob needle angle (A2) -------------------------------------
  // Open a band's NodePanel, set b0_sens to a known value, read the SENS mini-
  // knob's needle centre+tip, and check the tip ANGLE (deg clockwise from 12
  // o'clock) matches the INDEPENDENT oracle knobAngleForNorm = 225 + norm*270
  // (the shipped v2.1.0 sweep), mapped to +/- from top. SENS 7.40 dB on [-30,30]
  // -> norm 0.6233 -> ~+33.3 deg. This locks the mini-knob to the shared Knob's
  // value->angle mapping (one implementation) so the needle can't drift again.
  {
    await page.evaluate(() => { window.rs.selectNode(2); window.ui.set("b0_sens", 7.4); window.ui.freeze(true); });
    await wait(page, 120);
    const tip = await page.evaluate(() => window.rs.miniKnobTip(1)); // 1 = SENS
    const norm = (7.4 + 30) / 60;
    const expectedDeg = norm * 270 - 135;                        // deg clockwise from top (== +33.3)
    const measuredDeg = tip ? Math.atan2(tip.tx - tip.cx, tip.cy - tip.ty) * 180 / Math.PI : NaN;
    check("mini-knob SENS needle points at the value's angle (~+33 deg from top)",
      !!tip && Math.abs(measuredDeg - expectedDeg) <= 2.0 && Math.abs(expectedDeg - 33.3) < 1.0,
      "measured=" + (tip ? measuredDeg.toFixed(1) : "null") + " expected=" + expectedDeg.toFixed(1));
  }

  // --- 14. mini-knob value ring END lines up with the needle (fix 6) ---------
  // The mini arc used to be drawn with a single canvas.arc that skipped the −90°
  // screen offset the shared fillArcBand applies, so the accent ring landed 90°
  // off from the needle. Set SENS = 0 dB (norm 0.5 -> value angle 0deg, straight
  // up): the accent (salmon #ff9472) fill then spans the LEFT side (dial [-135,0]),
  // and the accentDim (#ffd6cd) remainder the RIGHT side (dial [0,135]) — exactly
  // like the big-knob three-zone assert at depth=50. Sample the ring centreline at
  // 9 o'clock (must read the accent) and 3 o'clock (must read the paler dim); on a
  // tiny mini-knob the thin band AA-lightens both toward the white body, so assert
  // the RELATIVE saturation (accent-side clearly more orange: lower green + blue)
  // rather than an absolute hue. With the old 90°-off arc the two sides swap, so
  // this fails; with the fix the accent sits on the value side.
  {
    await page.evaluate(() => { window.rs.selectNode(2); window.ui.set("b0_sens", 0); window.ui.freeze(true); });
    await wait(page, 150);
    const dial = await page.evaluate(() => window.rs.miniKnobDial(1)); // 1 = SENS
    const shot = await canvasShot(page);
    const at = (deg) => ({ x: dial.cx + dial.arcR * Math.sin(deg * Math.PI / 180),
                           y: dial.cy - dial.arcR * Math.cos(deg * Math.PI / 180) });
    const left  = at(-90), right = at(90);
    const acc = d.samplePixel(shot, left.x,  left.y,  1); // 9 o'clock -> accent (salmon)
    const dim = d.samplePixel(shot, right.x, right.y, 1); // 3 o'clock -> accentDim (pale)
    const moreOrange = acc.g <= dim.g - 12 && acc.b <= dim.b - 12; // accent side is clearly more saturated
    const notPale    = acc.g <= 210;                                // and is actually on the coloured band
    check("mini-knob accent ring ends at the needle angle (arc == needle, no 90deg drift)",
      !!dial && moreOrange && notPale,
      "accent(9h) " + JSON.stringify(acc) + " | dim(3h) " + JSON.stringify(dim));
  }

  // --- 15. Pre/Post/Both is a TRUE 3-way segmented selector (fix 8) -----------
  // Was: clicking the chip anywhere CYCLED the mode. Now each segment is its own
  // click target — clicking segment i selects mode i (0=Pre,1=Post,2=Both).
  {
    await page.evaluate(() => window.rs.selectNode(-1));
    await wait(page, 60);
    const cv = await page.evaluate(() => window.ui.widgetRect("curve"));
    // mode chip is frame-local {12,12,132,22}; segment i centre = (36 + 42*i, 23).
    const segCentre = (i) => ({ x: cv.x + 36 + 42 * i, y: cv.y + 23 });
    const results = [];
    let ok = true;
    for (let i = 0; i < 3; i++) {
      const c = segCentre(i);
      await clickWindow(page, c.x, c.y);
      const seg = await page.evaluate(() => window.rs.analyzerModeSegment());
      results.push(i + "->" + seg);
      if (seg !== i) ok = false;
    }
    // Re-click the already-active segment: must STAY (no cycle).
    const c2 = segCentre(2);
    await clickWindow(page, c2.x, c2.y);
    const stay = await page.evaluate(() => window.rs.analyzerModeSegment());
    check("Pre/Post/Both selects the clicked segment (not a cycle)",
      ok && stay === 2, results.join(" ") + " restay=" + stay);
  }

  // --- 16. alt-click resets a knob to its ParamStore default (fix 5) ---------
  {
    await page.evaluate(() => window.rs.selectNode(-1));
    const params = await page.evaluate(() => window.ui.list());
    const depthDef = params.find((p) => p.id === "depth").default;
    const target = depthDef > 50 ? depthDef - 25 : depthDef + 25;
    await page.evaluate((v) => window.ui.set("depth", v), target);
    const before = await page.evaluate(() => window.ui.get("depth"));
    const r = await page.evaluate(() => window.ui.widgetRect("depth"));
    const c = rectCentre(r);
    await altClickWindow(page, c.x, c.y);
    const after = await page.evaluate(() => window.ui.get("depth"));
    check("alt-click resets a knob to its default",
      approx(after, depthDef, 0.6) && Math.abs(before - depthDef) > 1,
      "def=" + depthDef + " before=" + before + " after=" + after);
  }

  // --- 17. alt-click a node resets its freq + sens (not width) to defaults (fix 5)
  {
    const params = await page.evaluate(() => window.ui.list());
    const fDef = params.find((p) => p.id === "b0_freq").default;
    const sDef = params.find((p) => p.id === "b0_sens").default;
    await page.evaluate(() => {
      window.ui.set("b0_on", 1); window.ui.set("b0_freq", 5000);
      window.ui.set("b0_sens", 16); window.ui.set("b0_width", 1.2);
      window.rs.selectNode(-1); window.ui.freeze(true);
    });
    await wait(page, 120);
    const wBefore = await page.evaluate(() => window.ui.get("b0_width"));
    const pos = await page.evaluate(() => ({ x: window.rs.nodeX(2), y: window.rs.nodeY(2) }));
    await altClickWindow(page, pos.x, pos.y);
    const after = await page.evaluate(() => ({ f: window.ui.get("b0_freq"), s: window.ui.get("b0_sens"), w: window.ui.get("b0_width") }));
    check("alt-click a node resets freq + sens to defaults (width untouched)",
      approx(after.f, fDef, Math.max(1, fDef * 0.02)) && approx(after.s, sDef, 0.5) && approx(after.w, wBefore, 0.001),
      "f " + after.f.toFixed(0) + "/" + fDef + " s " + after.s.toFixed(1) + "/" + sDef + " w " + after.w.toFixed(2) + "(was " + wBefore.toFixed(2) + ")");
  }

  // --- round-3 fidelity screenshots (for the human) --------------------------
  // widgetRect returns WINDOW px; page.screenshot clips in PAGE px, so add the
  // canvas's page offset (the canvas is centred in the page).
  const canvasBox = await page.evaluate(() => {
    const r = document.getElementById("canvas").getBoundingClientRect();
    return { left: r.left, top: r.top };
  });
  const clipWin = async (wx, wy, w, h) => ({
    x: Math.max(0, canvasBox.left + wx), y: Math.max(0, canvasBox.top + wy), width: w, height: h,
  });
  // (a) full editor, clean default state.
  await page.evaluate(() => {
    window.rs.presetLoad(0); window.rs.selectNode(-1);
    window.rs.setSize(1069, 747); window.ui.freeze(true);
  });
  await wait(page, 200);
  fs.writeFileSync(path.join(OUT, "r3-full-default.png"), await editorShot(page, 1069, 747));
  // (d) A/B header closeup (A|B strip + directional copy button).
  {
    const ab = await page.evaluate(() => window.ui.widgetRect("ab"));
    const cp = await page.evaluate(() => window.ui.widgetRect("copy"));
    const x0 = ab.x - 14, x1 = cp.x + cp.w + 14;
    fs.writeFileSync(path.join(OUT, "r3-abheader.png"),
      await page.screenshot({ clip: await clipWin(x0, ab.y - 12, x1 - x0, ab.h + 24) }));
  }
  // (c) bottom knob-row closeup (DEPTH/DETAIL big vs ATK/REL/TILT mini).
  {
    const dr = await page.evaluate(() => window.ui.widgetRect("depth"));
    const tr = await page.evaluate(() => window.ui.widgetRect("tilt"));
    const x0 = dr.x - 10, x1 = tr.x + tr.w + 10;
    fs.writeFileSync(path.join(OUT, "r3-knobrow.png"),
      await page.screenshot({ clip: await clipWin(x0, dr.y - 8, x1 - x0, dr.h + 16) }));
  }
  // (b) node panel open on a mid band.
  await page.evaluate(() => {
    window.ui.set("b2_on", 1); window.ui.set("b2_sens", 10); window.rs.selectNode(4); window.ui.freeze(true);
  });
  await wait(page, 200);
  fs.writeFileSync(path.join(OUT, "r3-band-open.png"), await editorShot(page, 1069, 747));
  await page.evaluate(() => window.rs.selectNode(-1));

  check("no JS/HTTP errors", jsErrors.length === 0 && httpErrors.length === 0, jsErrors.concat(httpErrors).slice(0, 4).join(" | "));

  await browser.close();

  const passed = asserts.every((a) => a.ok);
  const result = {
    url: URL, passed, webgl: webgl.renderer, timings,
    screenshots: ["rs-default.png", "rs-busy.png", "rs-min.png", "rs-max.png", "rs-knob-depth.png"].map((f) => path.join(OUT, f)),
    asserts, jsErrorCount: jsErrors.length, httpErrorCount: httpErrors.length,
  };
  fs.writeFileSync(path.join(OUT, "rs_result.json"), JSON.stringify(result, null, 2));
  console.log("\n" + JSON.stringify({ passed, passCount: asserts.filter((a) => a.ok).length, total: asserts.length, webgl: webgl.renderer }, null, 2));
  process.exit(passed ? 0 : 1);
})().catch((e) => { console.error("RS_SPEC_FATAL:", e); process.exit(2); });
