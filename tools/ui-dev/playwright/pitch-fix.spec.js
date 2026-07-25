#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");
const d = require("./drive.js");
const mouse = require("./plugin-test-utils.js");

const URL = process.argv[2] || "http://127.0.0.1:8082/index.html";
const OUT = process.argv[3] || __dirname;
fs.mkdirSync(OUT, { recursive: true });

const asserts = [];
function check(name, ok, detail) {
  asserts.push({ name, ok: !!ok, detail: detail === undefined ? "" : detail });
  console.log(`${ok ? "PASS" : "FAIL"} ${name}${detail === undefined ? "" : `  (${detail})`}`);
}
const approx = (a, b, eps = 0.6) => Math.abs(a - b) <= eps;

(async () => {
  const { browser, page } = await d.launch({ width: 1040, height: 680 });
  const errors = [];
  page.on("pageerror", (error) => errors.push(`pageerror: ${error.message}`));
  page.on("console", (message) => { if (message.type() === "error") errors.push(`console: ${message.text()}`); });
  page.on("response", (response) => {
    if (response.status() >= 400 && !response.url().includes("favicon")) errors.push(`${response.status()} ${response.url()}`);
  });

  await d.waitReady(page, URL);
  const webgl = await d.probeWebGL(page);
  await page.evaluate(() => window.ui.freeze(true));
  await page.waitForTimeout(150);

  const params = await page.evaluate(() => window.ui.list());
  check("real Pitch Fix parameter table is exposed", params.length === 14,
        params.map((param) => param.id).join(","));
  const rects = await page.evaluate(() => Object.fromEntries(
    window.ui.list().map((param) => [param.id, window.ui.widgetRect(param.id)])));
  check("every parameter has discoverable widget geometry",
        Object.values(rects).every((rect) => rect && rect.w > 0 && rect.h > 0));

  const hostValue = await page.evaluate(() => { window.ui.set("retune", 173); return window.ui.get("retune"); });
  check("host-driven parameter round-trip", approx(hostValue, 173), hostValue);

  const amount = await page.evaluate(() => {
    window.ui.set("amount", 30);
    return { rect: window.ui.widgetRect("amount"), before: window.ui.get("amount") };
  });
  await mouse.dragWindow(page, amount.rect.x + amount.rect.w / 2, amount.rect.y + amount.rect.h / 2,
                         amount.rect.x + amount.rect.w / 2, amount.rect.y + amount.rect.h / 2 - 65);
  const amountAfter = await page.evaluate(() => window.ui.get("amount"));
  check("real knob drag writes through the UI gesture path", amountAfter > amount.before + 2,
        `${amount.before} -> ${amountAfter}`);

  const scaleRect = rects.scale;
  await mouse.clickWindow(page, scaleRect.x + scaleRect.w * 0.5, scaleRect.y + scaleRect.h * 0.5);
  check("Scale segmented control selects Minor/Major row", (await page.evaluate(() => window.ui.get("scale"))) === 1);

  const dropdown = await page.evaluate(() => ({
    opened: window.ui.openDropdown(0), open: window.ui.dropdownOpen(), count: window.ui.dropdownCount(),
  }));
  check("Key dropdown opens with all 12 notes", dropdown.opened && dropdown.open && dropdown.count === 12,
        JSON.stringify(dropdown));
  const noteRow = await page.evaluate(() => ({ x: window.ui.dropdownX(5), y: window.ui.dropdownRowY(5) }));
  await mouse.clickWindow(page, noteRow.x, noteRow.y);
  check("real dropdown row click changes Key", (await page.evaluate(() => window.ui.get("key"))) === 5);

  await page.evaluate(() => { window.ui.set("key", 7); window.ui.openDropdown(1); });
  const presetMenu = await page.evaluate(() => ({ count: window.ui.dropdownCount(), x: window.ui.dropdownX(5), y: window.ui.dropdownRowY(5) }));
  check("real preset bank is exposed (Init + 10)", presetMenu.count === 11, presetMenu.count);
  await mouse.clickWindow(page, presetMenu.x, presetMenu.y);
  const preset = await page.evaluate(() => ({
    index: window.ui.presetIndex(), key: window.ui.get("key"), retune: window.ui.get("retune"), buffer: window.ui.get("buffer"),
  }));
  check("factory preset applies through PresetSession", preset.index === 5 && approx(preset.retune, 140) && preset.buffer === 2,
        JSON.stringify(preset));
  check("preset exclusion preserves musical context", preset.key === 7, `key=${preset.key}`);

  const feed = await page.evaluate(() => {
    window.pf.setFeed(233.5, 246.94, 91.2, 2048, 96000);
    return { detected: window.pf.detected(), target: window.pf.target(), shift: window.pf.shift(), latency: window.pf.latency() };
  });
  check("synthetic status feed is controllable", approx(feed.detected, 233.5, 0.05) && feed.latency === 2048,
        JSON.stringify(feed));

  const theme = await page.evaluate(() => {
    const before = window.ui.accent();
    const ok = window.ui.reloadTheme('{"palette":{"accent":"#ff33c8a6"}}');
    const changed = window.ui.accent();
    const bad = window.ui.reloadTheme('{"palette":{"accent":"bad"}}');
    return { before, ok, changed, bad, error: window.ui.lastError() };
  });
  check("shared theme hot reload works on the plugin editor", theme.ok && theme.changed === (0xff33c8a6 >>> 0));
  check("malformed theme is rejected with a diagnostic", theme.bad === false && theme.error.length > 0, theme.error);

  const shot = await page.locator("#canvas").screenshot({ path: path.join(OUT, "pitch-fix.png") });
  check("Pitch Fix screenshot is non-blank", d.notBlank(d.analyzePNG(shot)), JSON.stringify(d.analyzePNG(shot)));
  check("no browser/runtime errors", errors.length === 0, errors.slice(0, 4).join(" | "));

  const passed = asserts.every((item) => item.ok);
  const result = { url: URL, passed, webgl, asserts, errors, screenshot: path.join(OUT, "pitch-fix.png") };
  fs.writeFileSync(path.join(OUT, "pitch_fix_result.json"), JSON.stringify(result, null, 2));
  await browser.close();
  console.log(JSON.stringify({ passed, passCount: asserts.filter((item) => item.ok).length, total: asserts.length }, null, 2));
  process.exit(passed ? 0 : 1);
})().catch((error) => { console.error("PITCH_FIX_SPEC_FATAL:", error); process.exit(2); });
