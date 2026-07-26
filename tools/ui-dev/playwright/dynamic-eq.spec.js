#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");
const d = require("./drive.js");
const mouse = require("./plugin-test-utils.js");

const URL = process.argv[2] || "http://127.0.0.1:8083/index.html";
const OUT = process.argv[3] || __dirname;
fs.mkdirSync(OUT, { recursive: true });

const asserts = [];
function check(name, ok, detail) {
  asserts.push({ name, ok: !!ok, detail: detail === undefined ? "" : detail });
  console.log(`${ok ? "PASS" : "FAIL"} ${name}${detail === undefined ? "" : `  (${detail})`}`);
}
const approx = (a, b, eps = 0.6) => Math.abs(a - b) <= eps;

(async () => {
  const { browser, page } = await d.launch({ width: 900, height: 700 });
  const errors = [];
  page.on("pageerror", (error) => errors.push(`pageerror: ${error.message}`));
  page.on("console", (message) => { if (message.type() === "error") errors.push(`console: ${message.text()}`); });
  page.on("response", (response) => {
    if (response.status() >= 400 && !response.url().includes("favicon")) errors.push(`${response.status()} ${response.url()}`);
  });

  await d.waitReady(page, URL);
  const webgl = await d.probeWebGL(page);
  await page.evaluate(() => window.ui.freeze(true));
  const params = await page.evaluate(() => window.ui.list());
  check("real Dynamic EQ parameter table is exposed", params.length === 361, `n=${params.length}`);

  await page.evaluate(() => {
    window.ui.set("b0_on", 1); window.ui.set("b0_freq", 800); window.ui.set("b0_gain", 2);
    window.ui.set("b1_on", 1); window.ui.set("b1_freq", 4000); window.ui.set("b1_gain", -3);
    window.deq.selectBand(0);
  });
  await page.waitForTimeout(150);
  const geometry = await page.evaluate(() => ({
    plot: window.deq.plotRect(), node: { x: window.deq.nodeX(0), y: window.deq.nodeY(0) },
    freq: window.ui.widgetRect("b0_freq"), panel: window.ui.widgetRect("panel"),
  }));
  check("plot, node, and selected-band controls are discoverable",
        geometry.plot && geometry.node.x > 0 && geometry.node.y > 0 && geometry.freq && geometry.panel,
        JSON.stringify(geometry));

  const beforeNode = await page.evaluate(() => ({ freq: window.ui.get("b0_freq"), gain: window.ui.get("b0_gain") }));
  await mouse.dragWindow(page, geometry.node.x, geometry.node.y, geometry.node.x + 55, geometry.node.y - 28);
  const afterNode = await page.evaluate(() => ({ freq: window.ui.get("b0_freq"), gain: window.ui.get("b0_gain") }));
  check("real curve-node drag changes frequency and gain through gestures",
        afterNode.freq > beforeNode.freq && afterNode.gain > beforeNode.gain,
        `${JSON.stringify(beforeNode)} -> ${JSON.stringify(afterNode)}`);

  await page.evaluate(() => window.deq.selectBand(1));
  const selection = await page.evaluate(() => ({ selected: window.deq.selectedBand(), rect: window.ui.widgetRect("b1_gain") }));
  check("driver can select a band and inspect its rebound panel", selection.selected === 1 && selection.rect && selection.rect.w > 0,
        JSON.stringify(selection));

  const dynRect = await page.evaluate(() => window.ui.widgetRect("b1_dyn"));
  const dynBefore = await page.evaluate(() => window.ui.get("b1_dyn"));
  await mouse.clickWindow(page, dynRect.x + dynRect.w / 2, dynRect.y + dynRect.h / 2);
  const dynAfter = await page.evaluate(() => window.ui.get("b1_dyn"));
  check("panel pill click changes the rebound band parameter", dynAfter !== dynBefore, `${dynBefore} -> ${dynAfter}`);

  const typeMenu = await page.evaluate(() => ({ opened: window.ui.openDropdown("type"), count: window.ui.dropdownCount(), open: window.ui.dropdownOpen() }));
  check("band Type dropdown exposes five filter types", typeMenu.opened && typeMenu.open && typeMenu.count === 5,
        JSON.stringify(typeMenu));
  const typeRow = await page.evaluate(() => ({ x: window.ui.dropdownX(3), y: window.ui.dropdownRowY(3) }));
  await mouse.clickWindow(page, typeRow.x, typeRow.y);
  check("dropdown row click selects High Pass", (await page.evaluate(() => window.ui.get("b1_type"))) === 3);

  const slopeMenu = await page.evaluate(() => ({ opened: window.ui.openDropdown("slope"), count: window.ui.dropdownCount() }));
  check("cut-band Slope dropdown exposes eight slopes", slopeMenu.opened && slopeMenu.count === 8,
        JSON.stringify(slopeMenu));
  const slopeRow = await page.evaluate(() => ({ x: window.ui.dropdownX(2), y: window.ui.dropdownRowY(2) }));
  await mouse.clickWindow(page, slopeRow.x, slopeRow.y);
  check("Slope dropdown writes the selected choice", (await page.evaluate(() => window.ui.get("b1_slope"))) === 2);

  await page.evaluate(() => { window.ui.set("b5_lsn", 1); window.ui.set("bypass", 1); window.ui.openDropdown("preset"); });
  const presetMenu = await page.evaluate(() => ({ count: window.ui.dropdownCount(), x: window.ui.dropdownX(1), y: window.ui.dropdownRowY(1) }));
  check("real Dynamic EQ preset bank is exposed (Init + 4)", presetMenu.count === 5, presetMenu.count);
  await mouse.clickWindow(page, presetMenu.x, presetMenu.y);
  const preset = await page.evaluate(() => ({
    index: window.ui.presetIndex(), b0: window.ui.get("b0_on"), b1: window.ui.get("b1_on"),
    listen: window.ui.get("b5_lsn"), bypass: window.ui.get("bypass"), freq: window.ui.get("b0_freq"),
  }));
  check("factory preset applies through PresetSession", preset.index === 1 && preset.b0 === 1 && preset.b1 === 1 && approx(preset.freq, 3000, 2),
        JSON.stringify(preset));
  check("preset exclusions preserve bypass/listen state", preset.listen === 1 && preset.bypass === 1, JSON.stringify(preset));

  const gains = await page.evaluate(() => {
    window.ui.set("b0_gain", 0); window.ui.set("b0_dyn", 1); window.ui.set("b0_rng", -8);
    window.deq.setPhase(0); const a = window.deq.liveGain(0);
    window.deq.setPhase(1); const b = window.deq.liveGain(0);
    return { a, b };
  });
  check("synthetic dynamics feed is deterministic and controllable", Math.abs(gains.a - gains.b) > 0.2,
        JSON.stringify(gains));

  const plot = await page.evaluate(() => window.deq.plotRect());
  await page.evaluate(() => window.ui.feedSpectrum(0));
  await page.waitForTimeout(100);
  const spectrumA = await page.locator("#canvas").screenshot();
  await page.evaluate(() => window.ui.feedSpectrum(1));
  await page.waitForTimeout(100);
  const spectrumB = await page.locator("#canvas").screenshot();
  const spectrumDiff = d.regionMeanAbsDiff(spectrumA, spectrumB, plot);
  check("deterministic analyser injection changes the plotted pixels", spectrumDiff > 0.05,
        `meanAbsDiff=${spectrumDiff.toFixed(3)}`);

  const theme = await page.evaluate(() => ({
    ok: window.ui.reloadTheme('{"palette":{"accent":"#ff33c8a6"}}'),
    accent: window.ui.accent(),
  }));
  check("shared theme hot reload works on Dynamic EQ", theme.ok && theme.accent === (0xff33c8a6 >>> 0));

  const shot = await page.locator("#canvas").screenshot({ path: path.join(OUT, "dynamic-eq.png") });
  check("Dynamic EQ screenshot is non-blank", d.notBlank(d.analyzePNG(shot)), JSON.stringify(d.analyzePNG(shot)));
  check("no browser/runtime errors", errors.length === 0, errors.slice(0, 4).join(" | "));

  const passed = asserts.every((item) => item.ok);
  const result = { url: URL, passed, webgl, asserts, errors, screenshot: path.join(OUT, "dynamic-eq.png") };
  fs.writeFileSync(path.join(OUT, "dynamic_eq_result.json"), JSON.stringify(result, null, 2));
  await browser.close();
  console.log(JSON.stringify({ passed, passCount: asserts.filter((item) => item.ok).length, total: asserts.length }, null, 2));
  process.exit(passed ? 0 : 1);
})().catch((error) => { console.error("DYNAMIC_EQ_SPEC_FATAL:", error); process.exit(2); });
