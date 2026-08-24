#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");
const d = require("./drive.js");

function parseArgs(argv) {
  const options = {
    url: argv[0] || "http://127.0.0.1:8081/index.html",
    outDir: path.resolve(argv[1] || path.join(__dirname, "..", "artifacts", "rs-editor")),
    app: "rs-editor",
    sets: [],
    size: null,
  };
  for (let i = 2; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--app") options.app = argv[++i];
    else if (arg === "--set") options.sets.push(argv[++i]);
    else if (arg === "--size") options.size = argv[++i];
    else throw new Error(`Unknown argument: ${arg}`);
  }
  if (!["gallery", "rs-editor", "tn-vocal-tuner", "tn-equalizer"].includes(options.app)) throw new Error(`Invalid --app: ${options.app}`);
  if (options.size && !/^\d+x\d+$/.test(options.size)) throw new Error(`Invalid --size: ${options.size}`);
  for (const assignment of options.sets) {
    if (!/^[^=]+=[-+]?\d+(?:\.\d+)?$/.test(assignment)) throw new Error(`Invalid --set: ${assignment}`);
  }
  return options;
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  fs.mkdirSync(options.outDir, { recursive: true });
  const viewports = {
    "gallery": { width: 1000, height: 820 },
    "rs-editor": { width: 1420, height: 1000 },
    "tn-vocal-tuner": { width: 1040, height: 680 },
    "tn-equalizer": { width: 900, height: 700 },
  };
  const viewport = viewports[options.app];
  const { browser, page } = await d.launch(viewport);
  const diagnostics = { consoleErrors: [], pageErrors: [], httpErrors: [] };
  page.on("console", (message) => {
    if (message.type() === "error") diagnostics.consoleErrors.push(message.text());
  });
  page.on("pageerror", (error) => diagnostics.pageErrors.push(error.message));
  page.on("response", (response) => {
    if (response.status() >= 400 && !response.url().includes("favicon")) {
      diagnostics.httpErrors.push(`${response.status()} ${response.url()}`);
    }
  });

  try {
    await d.waitReady(page, options.url);
    const assignments = options.sets.map((entry) => {
      const split = entry.indexOf("=");
      return { id: entry.slice(0, split), value: Number(entry.slice(split + 1)) };
    });
    const size = options.size ? options.size.split("x").map(Number) : null;
    await page.evaluate(({ assignments: values, size: requestedSize, app }) => {
      if (app === "rs-editor" && window.rs && requestedSize) window.rs.setSize(requestedSize[0], requestedSize[1]);
      for (const item of values) window.ui.set(item.id, item.value);
    }, { assignments, size, app: options.app });
    // Let animated feeds publish at least one real frame before freezing. This
    // keeps captures deterministic without accidentally freezing an empty first
    // analyser frame on a fast machine.
    await page.waitForTimeout(350);
    await page.evaluate(() => window.ui.freeze(true));
    await page.waitForTimeout(120);

    const state = await page.evaluate((app) => {
      const params = window.ui.list();
      const specialByApp = {
        "gallery": ["preset", "spectrum", "valueSetting"],
        "rs-editor": ["preset", "plot", "mode", "quality", "footerCard", "modeCard", "footerDiv1", "footerDiv2"],
        "tn-vocal-tuner": ["preset", "status", "key", "scale", "buffer"],
        "tn-equalizer": ["preset", "plot", "curve", "panel", "bypass", "b0_node"],
      };
      const specialKeys = specialByApp[app];
      const rects = {};
      for (const key of [...params.map((param) => param.id), ...specialKeys]) {
        const rect = window.ui.widgetRect(key);
        if (rect) rects[key] = rect;
      }
      const rs = app === "rs-editor" && window.rs ? {
        selectedNode: window.rs.selectedNode(),
        listenNode: window.rs.listenNode(),
        abSlot: window.rs.abSlot(),
        presetIndex: window.ui.presetIndex(),
        plotRect: window.rs.plotRect(),
      } : null;
      const plugin = app === "tn-vocal-tuner" && window.pf ? {
        detected: window.pf.detected(), target: window.pf.target(), shift: window.pf.shift(), latency: window.pf.latency(),
      } : app === "tn-equalizer" && window.deq ? {
        selectedBand: window.deq.selectedBand(), plotRect: window.deq.plotRect(),
      } : null;
      const canvas = document.getElementById("canvas");
      const bounds = canvas.getBoundingClientRect();
      return {
        params,
        rects,
        rs,
        plugin,
        canvas: { width: canvas.width, height: canvas.height, cssWidth: bounds.width, cssHeight: bounds.height },
      };
    }, options.app);

    const screenshotPath = path.join(options.outDir, "ui.png");
    let screenshot;
    if (options.app === "rs-editor") {
      const captureSize = size || [1069, 747];
      const origin = await page.evaluate(() => {
        const bounds = document.getElementById("canvas").getBoundingClientRect();
        return { x: bounds.left, y: bounds.top };
      });
      screenshot = await page.screenshot({
        path: screenshotPath,
        clip: { x: origin.x, y: origin.y, width: captureSize[0], height: captureSize[1] },
      });
    } else {
      screenshot = await page.locator("#canvas").screenshot({ path: screenshotPath });
    }

    const report = {
      app: options.app,
      url: options.url,
      browser: d.browserInfo(),
      webgl: await d.probeWebGL(page),
      screenshot: screenshotPath,
      screenshotStats: d.analyzePNG(screenshot),
      ...state,
      diagnostics,
    };
    const statePath = path.join(options.outDir, "ui-state.json");
    fs.writeFileSync(statePath, JSON.stringify(report, null, 2));
    console.log(JSON.stringify({
      app: report.app,
      screenshot: report.screenshot,
      state: statePath,
      params: report.params.length,
      widgetRects: Object.keys(report.rects).length,
      webgl: report.webgl.renderer,
      errors: diagnostics.consoleErrors.length + diagnostics.pageErrors.length + diagnostics.httpErrors.length,
    }, null, 2));
    const ok = d.notBlank(report.screenshotStats) &&
      diagnostics.consoleErrors.length === 0 && diagnostics.pageErrors.length === 0 && diagnostics.httpErrors.length === 0;
    process.exitCode = ok ? 0 : 1;
  } finally {
    await browser.close();
  }
}

main().catch((error) => {
  console.error(`INSPECT_FATAL: ${error && error.stack ? error.stack : error}`);
  process.exit(2);
});
