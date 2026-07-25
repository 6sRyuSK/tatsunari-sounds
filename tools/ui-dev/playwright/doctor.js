#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");

function parseArgs(argv) {
  const result = { buildDir: path.resolve(__dirname, "..", "build", "dev"), json: false };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--build-dir") result.buildDir = path.resolve(argv[++i]);
    else if (arg === "--json") result.json = true;
    else if (arg === "-h" || arg === "--help") {
      console.log("Usage: node doctor.js [--build-dir <dir>] [--json]");
      process.exit(0);
    } else {
      throw new Error(`Unknown argument: ${arg}`);
    }
  }
  return result;
}

function fileCheck(file) {
  return { path: file, exists: fs.existsSync(file) };
}

function main() {
  const options = parseArgs(process.argv.slice(2));
  let driver;
  try {
    driver = require("./drive.js");
  } catch (error) {
    console.error("The Playwright driver could not be loaded. Run `npm ci` and check CHROME_BIN.");
    console.error(String(error && error.message ? error.message : error));
    process.exit(2);
  }

  const browser = driver.browserInfo();
  const builds = {
    gallery: fileCheck(path.join(options.buildDir, "web", "index.html")),
    rsEditor: fileCheck(path.join(options.buildDir, "web-rs", "index.html")),
    pitchFix: fileCheck(path.join(options.buildDir, "web-pf", "index.html")),
    dynamicEq: fileCheck(path.join(options.buildDir, "web-deq", "index.html")),
  };
  const report = {
    ok: browser.available,
    node: process.version,
    platform: process.platform,
    browser,
    buildDir: options.buildDir,
    builds,
  };

  if (options.json) {
    console.log(JSON.stringify(report, null, 2));
  } else {
    console.log(`Node       ${report.node} (${report.platform})`);
    console.log(`Chromium   ${browser.available ? "ok" : "missing"}${browser.path ? ` (${browser.source}: ${browser.path})` : ""}`);
    console.log(`Gallery    ${builds.gallery.exists ? "built" : "not built"} (${builds.gallery.path})`);
    console.log(`RS editor  ${builds.rsEditor.exists ? "built" : "not built"} (${builds.rsEditor.path})`);
    console.log(`Pitch Fix  ${builds.pitchFix.exists ? "built" : "not built"} (${builds.pitchFix.path})`);
    console.log(`Dynamic EQ ${builds.dynamicEq.exists ? "built" : "not built"} (${builds.dynamicEq.path})`);
    if (!browser.available) {
      console.log("Install the managed browser with `npx playwright install chromium`, or set CHROME_BIN.");
    }
  }
  process.exit(report.ok ? 0 : 1);
}

try {
  main();
} catch (error) {
  console.error(`DOCTOR_FATAL: ${error && error.stack ? error.stack : error}`);
  process.exit(2);
}
