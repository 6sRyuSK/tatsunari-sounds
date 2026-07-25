#!/usr/bin/env node
"use strict";

const fs = require("fs");
const http = require("http");
const net = require("net");
const path = require("path");
const { spawn, spawnSync } = require("child_process");

const UI_DEV_DIR = path.resolve(__dirname, "..");
const REPO_DIR = path.resolve(UI_DEV_DIR, "..", "..");

function usage() {
  console.log(`Usage: node verify.js [options]

Options:
  --app gallery|rs-editor|pitch-fix|dynamic-eq|all
                                App to verify (default: rs-editor)
  --mode test|inspect          Run the regression test or capture UI state
  --build-dir <dir>            CMake build dir (default: ../build/dev)
  --out <dir>                  Artifact root (default: ../artifacts)
  --url <url>                  Use an already-running server (single app only)
  --set <id=value>             Inspector-only parameter override (repeatable)
  --size <width>x<height>      Inspector-only capture size
  --headed                     Show the browser window
  -h, --help                   Show this help`);
}

function takeValue(argv, index, option) {
  if (index + 1 >= argv.length) throw new Error(`${option} requires a value`);
  return argv[index + 1];
}

function parseArgs(argv) {
  const options = {
    app: "rs-editor",
    mode: "test",
    buildDir: path.resolve(UI_DEV_DIR, "build", "dev"),
    outDir: path.resolve(UI_DEV_DIR, "artifacts"),
    url: null,
    sets: [],
    size: null,
    headed: false,
  };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--app") options.app = takeValue(argv, i++, arg);
    else if (arg === "--mode") options.mode = takeValue(argv, i++, arg);
    else if (arg === "--build-dir") options.buildDir = path.resolve(takeValue(argv, i++, arg));
    else if (arg === "--out") options.outDir = path.resolve(takeValue(argv, i++, arg));
    else if (arg === "--url") options.url = takeValue(argv, i++, arg);
    else if (arg === "--set") options.sets.push(takeValue(argv, i++, arg));
    else if (arg === "--size") options.size = takeValue(argv, i++, arg);
    else if (arg === "--headed") options.headed = true;
    else if (arg === "-h" || arg === "--help") {
      usage();
      process.exit(0);
    } else throw new Error(`Unknown argument: ${arg}`);
  }
  if (!["gallery", "rs-editor", "pitch-fix", "dynamic-eq", "all"].includes(options.app)) throw new Error(`Invalid --app: ${options.app}`);
  if (!["test", "inspect"].includes(options.mode)) throw new Error(`Invalid --mode: ${options.mode}`);
  if (options.url && options.app === "all") throw new Error("--url cannot be combined with --app all");
  if (options.mode === "test" && (options.sets.length || options.size)) throw new Error("--set/--size are only available with --mode inspect");
  return options;
}

function appConfig(app, buildDir) {
  if (app === "gallery") {
    return {
      app,
      webDir: path.join(buildDir, "web"),
      themeFile: null,
      script: path.join(__dirname, "smoke.js"),
    };
  }
  if (app === "pitch-fix") {
    return {
      app,
      webDir: path.join(buildDir, "web-pf"),
      themeFile: null,
      script: path.join(__dirname, "pitch-fix.spec.js"),
    };
  }
  if (app === "dynamic-eq") {
    return {
      app,
      webDir: path.join(buildDir, "web-deq"),
      themeFile: null,
      script: path.join(__dirname, "dynamic-eq.spec.js"),
    };
  }
  return {
    app,
    webDir: path.join(buildDir, "web-rs"),
    themeFile: path.join(REPO_DIR, "plugins", "resonance-suppressor", "ui", "theme-rs.json"),
    script: path.join(__dirname, "rs.spec.js"),
  };
}

function findPython() {
  const candidates = process.env.PYTHON_BIN
    ? [{ command: process.env.PYTHON_BIN, prefix: [] }]
    : process.platform === "win32"
    ? [{ command: "python", prefix: [] }, { command: "py", prefix: ["-3"] }]
    : [{ command: "python3", prefix: [] }, { command: "python", prefix: [] }];
  for (const candidate of candidates) {
    const probe = spawnSync(candidate.command, [...candidate.prefix, "--version"], { stdio: "ignore" });
    if (!probe.error && probe.status === 0) return candidate;
  }
  throw new Error("Python 3 was not found. Run tools/ui-dev/setup first, or set PYTHON_BIN to its executable.");
}

function reservePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => {
      const address = server.address();
      const port = address.port;
      server.close((error) => error ? reject(error) : resolve(port));
    });
  });
}

function requestOk(url) {
  return new Promise((resolve) => {
    const request = http.get(url, (response) => {
      response.resume();
      resolve(response.statusCode >= 200 && response.statusCode < 300);
    });
    request.setTimeout(1000, () => request.destroy());
    request.once("error", () => resolve(false));
  });
}

async function waitForServer(url, child, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (child.exitCode !== null) throw new Error(`dev server exited early with code ${child.exitCode}`);
    if (await requestOk(url)) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`Timed out waiting for ${url}`);
}

function runNode(script, args, env) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [script, ...args], { env, stdio: "inherit" });
    child.once("error", reject);
    child.once("exit", (code, signal) => resolve(signal ? 1 : (code ?? 1)));
  });
}

async function stopServer(child) {
  if (!child || child.exitCode !== null) return;
  await new Promise((resolve) => {
    const timer = setTimeout(() => {
      if (child.exitCode === null) child.kill("SIGKILL");
      resolve();
    }, 3000);
    child.once("exit", () => {
      clearTimeout(timer);
      resolve();
    });
    child.kill("SIGTERM");
  });
}

async function runApp(options, app) {
  const config = appConfig(app, options.buildDir);
  const indexPath = path.join(config.webDir, "index.html");
  if (!options.url && !fs.existsSync(indexPath)) {
    throw new Error(`Missing ${indexPath}. Build it first with tools/ui-dev/dev${process.platform === "win32" ? ".ps1" : ".sh"}.`);
  }

  const outDir = path.join(options.outDir, app);
  fs.mkdirSync(outDir, { recursive: true });
  let server = null;
  let serverLog = "";
  let url = options.url;
  try {
    if (!url) {
      const python = findPython();
      const port = await reservePort();
      url = `http://127.0.0.1:${port}/index.html`;
      const args = [
        ...python.prefix,
        path.join(UI_DEV_DIR, "dev_server.py"),
        "--web-dir", config.webDir,
        "--port", String(port),
      ];
      if (config.themeFile) args.push("--theme-file", config.themeFile);
      server = spawn(python.command, args, { cwd: UI_DEV_DIR, stdio: ["ignore", "pipe", "pipe"] });
      const remember = (chunk) => { serverLog += chunk.toString(); };
      server.stdout.on("data", remember);
      server.stderr.on("data", remember);
      await waitForServer(`http://127.0.0.1:${port}/healthz`, server, 10000);
    }

    console.log(`\n== ${options.mode} ${app} (${url}) ==`);
    const env = { ...process.env };
    if (options.headed) env.PW_HEADLESS = "0";
    const script = options.mode === "inspect" ? path.join(__dirname, "inspect.js") : config.script;
    const args = [url, outDir];
    if (options.mode === "inspect") {
      args.push("--app", app);
      for (const assignment of options.sets) args.push("--set", assignment);
      if (options.size) args.push("--size", options.size);
    }
    const code = await runNode(script, args, env);
    if (code !== 0) throw new Error(`${path.basename(script)} failed with exit code ${code}`);
    console.log(`Artifacts: ${outDir}`);
  } catch (error) {
    if (serverLog.trim()) console.error(`\nDev server output:\n${serverLog.trim()}`);
    throw error;
  } finally {
    await stopServer(server);
  }
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  const apps = options.app === "all"
    ? ["gallery", "rs-editor", "pitch-fix", "dynamic-eq"]
    : [options.app];
  for (const app of apps) await runApp(options, app);
}

main().catch((error) => {
  console.error(`VERIFY_FATAL: ${error && error.stack ? error.stack : error}`);
  process.exit(2);
});
