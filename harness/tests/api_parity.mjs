#!/usr/bin/env node
// api_parity.mjs - the API parity check (design: docs/api-parity.md,
// scripts/parity.py).
//
// Deployment-integrity test, stdlib-only Node. Boots the daemon on a
// scratch port with a temp data dir, then asserts:
//
//   1. the daemon's self-reported route inventory (GET /api/v1/routes)
//      matches the documented contract (harness/tests/api_contract.json)
//      in both directions - a route added to the daemon without the
//      contract (or vice versa) fails;
//   2. every contract route answers its expected status (SSE routes:
//      200 + text/event-stream + first bytes);
//   3. the built web/dist is servable: index.html and every asset it
//      references (one level of JS/CSS reference expansion) come back
//      byte-identical (SHA-256) to the on-disk build.
//
// The contract is the single route inventory: the Vite dev proxy's /api
// rewrite (web/vite.config.ts) and this script both derive from it.
// After a daemon route change, re-derive the contract and review the
// diff like a golden file:
//
//   node harness/tests/api_parity.mjs [path-to-nyaxus_daemon] --record
//
// Usage: node harness/tests/api_parity.mjs [path-to-nyaxus_daemon] [--record]
import http from "node:http";
import crypto from "node:crypto";
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const CONTRACT = path.join(ROOT, "harness", "tests", "api_contract.json");
const DIST = path.join(ROOT, "web", "dist");
const DAEMON = process.argv[2] && !process.argv[2].startsWith("--")
  ? path.resolve(process.argv[2])
  : path.join(ROOT, "harness", "nyaxus_daemon.exe");
const RECORD = process.argv.includes("--record");
const DAEMON_PORT = 8973;
const STATIC_PORT = 8974;
const BASE = `http://127.0.0.1:${DAEMON_PORT}`;
const SENTINEL = "parity-probe";

let failures = 0;
const fail = (msg) => { failures += 1; console.error(`  FAIL  ${msg}`); };
const pass = (msg) => console.log(`  PASS  ${msg}`);
const section = (name) => console.log(`\n== ${name} ==`);

// ── HTTP helper ─────────────────────────────────────────────────────────────────
function request(method, urlPath, { body, timeoutMs = 8000 } = {}) {
  return new Promise((resolve, reject) => {
    let settled = false;
    const done = (value) => { if (!settled) { settled = true; resolve(value); } };
    const req = http.request(
      { host: "127.0.0.1", port: DAEMON_PORT, path: urlPath, method,
        headers: body ? { "content-type": "application/json",
                         "content-length": Buffer.byteLength(body) } : {} },
      (res) => {
        let data = "";
        let bytes = 0;
        const timer = setTimeout(() => { req.destroy(); }, timeoutMs);
        const finish = () => {
          clearTimeout(timer);
          done({ status: res.statusCode, headers: res.headers, body: data });
        };
        res.on("data", (c) => {
          bytes += c.length;
          if (bytes < 1024 * 1024) data += c;
          if (String(res.headers["content-type"] || "")
              .includes("text/event-stream") && data.length > 0) {
            // SSE: the first bytes are enough to prove the stream opened.
            res.destroy();
            finish();
          }
        });
        res.on("end", finish);
        res.on("close", finish);
      }
    );
    req.on("error", (e) => { if (!settled) { settled = true; reject(e); } });
    if (body) req.write(body);
    req.end();
  });
}

// ── Probe: fill placeholders, send, return the status ───────────────────────
function fillPath(p) {
  return p.replace(/\{[a-z_]+\}/g, SENTINEL);
}

async function probe(route) {
  const method = route.method === "SSE" ? "GET" : route.method;
  return request(method, fillPath(route.path), { body: method === "POST" ? "{}" : undefined });
}

// ── Daemon lifecycle ────────────────────────────────────────────────────────
async function waitHealthy(tries = 60) {
  for (let i = 0; i < tries; i += 1) {
    try {
      const r = await request("GET", "/api/v1/health");
      if (r.status === 200) return true;
    } catch { /* not up yet */ }
    await new Promise((r) => setTimeout(r, 500));
  }
  return false;
}

// ── Static server over web/dist ─────────────────────────────────────────────
function startStaticServer() {
  const types = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css",
                  ".json": "application/json", ".svg": "image/svg+xml",
                  ".png": "image/png", ".ico": "image/x-icon", ".woff2": "font/woff2" };
  const server = http.createServer((req, res) => {
    const urlPath = decodeURIComponent((req.url || "/").split("?")[0]);
    let file = path.join(DIST, urlPath === "/" ? "index.html" : urlPath);
    if (!file.startsWith(DIST)) { res.writeHead(400); res.end(); return; }
    fs.readFile(file, (err, buf) => {
      if (err) { res.writeHead(404); res.end("not found"); return; }
      res.writeHead(200, { "content-type": types[path.extname(file)] || "application/octet-stream" });
      res.end(buf);
    });
  });
  return new Promise((resolve) => server.listen(STATIC_PORT, "127.0.0.1", () => resolve(server)));
}

async function fetchAsset(rel) {
  const res = await new Promise((resolve, reject) => {
    http.get({ host: "127.0.0.1", port: STATIC_PORT, path: rel }, (r) => {
      const chunks = [];
      r.on("data", (c) => chunks.push(c));
      r.on("end", () => resolve({ status: r.statusCode, body: Buffer.concat(chunks) }));
    }).on("error", reject);
  });
  return res;
}

const sha256 = (buf) => crypto.createHash("sha256").update(buf).digest("hex");

// ── Check 3: web/dist servability + byte-identity ───────────────────────────
async function checkDist() {
  section("web/dist: index.html + referenced assets byte-identical");
  if (!fs.existsSync(path.join(DIST, "index.html"))) {
    fail("web/dist/index.html missing - run `npm run build` in web/ first");
    return;
  }
  const server = await startStaticServer();
  try {
    const seen = new Set();
    const queue = ["/"];
    let checked = 0;
    while (queue.length > 0) {
      const rel = queue.shift();
      if (seen.has(rel)) continue;
      seen.add(rel);
      const got = await fetchAsset(rel);
      if (got.status !== 200) { fail(`served ${rel}: HTTP ${got.status} (want 200)`); continue; }
      const diskPath = path.join(DIST, rel === "/" ? "index.html" : rel.replace(/^\//, ""));
      const disk = fs.readFileSync(diskPath);
      if (sha256(got.body) !== sha256(disk)) {
        fail(`served ${rel} differs from disk (SHA-256 mismatch)`);
        continue;
      }
      checked += 1;
      // Expand one level of references from HTML/JS/CSS.
      const text = got.body.toString("utf8");
      const refs = [...text.matchAll(/(?:src|href|url\()="?['"]?(\.{1,2}\/[^"')\s?]+|\/assets\/[^"')\s?]+)/g)]
        .map((m) => m[1]);
      for (const ref of refs) {
        const norm = path.posix.normalize("/" + ref.replace(/^\.\//, ""));
        if (!seen.has(norm) && fs.existsSync(path.join(DIST, norm.slice(1)))) queue.push(norm);
      }
    }
    pass(`${checked} asset(s) served byte-identical (index.html + ${seen.size - 1} referenced)`);
  } finally {
    server.close();
  }
}

// ── Main ────────────────────────────────────────────────────────────────────────
async function main() {
  const tmpdir = fs.mkdtempSync(path.join(os.tmpdir(), "nyaxus-parity-"));
  const logFile = fs.openSync(path.join(tmpdir, "daemon.log"), "w");
  const daemon = spawn(DAEMON, ["--port", String(DAEMON_PORT)], {
    cwd: tmpdir, stdio: ["ignore", logFile, logFile],
  });
  try {
  if (!fs.existsSync(DAEMON)) { console.error(`daemon binary not found: ${DAEMON}`); return; }
  console.log(`booting daemon on :${DAEMON_PORT} (data dir ${tmpdir})`);
  if (!(await waitHealthy())) {
    fail("daemon did not become healthy (see " + path.join(tmpdir, "daemon.log") + ")");
    return;
  }
  pass("daemon healthy");

  const inventory = await request("GET", "/api/v1/routes");
  if (inventory.status !== 200) { fail(`GET /api/v1/routes: HTTP ${inventory.status}`); return; }
  const daemonRoutes = new Map(
    JSON.parse(inventory.body).routes.map((r) => [r.method + " " + r.path, r])
  );
  console.log(`daemon self-reports ${daemonRoutes.size} routes`);

  if (RECORD) {
    section("--record: deriving the contract from the live daemon");
    const routes = [];
    for (const key of [...daemonRoutes.keys()].sort()) {
      const route = daemonRoutes.get(key);
      const r = await probe(route);
      routes.push({ method: route.method, path: route.path, expect: r.status });
      console.log(`  ${route.method} ${route.path} -> ${r.status}`);
    }
    fs.writeFileSync(CONTRACT, JSON.stringify({
      prefix: "/api",
      note: "Single route inventory (docs/api-parity.md). Re-derive with --record after daemon route changes; review the diff.",
      routes,
    }, null, 2) + "\n");
    pass(`contract written to ${path.relative(ROOT, CONTRACT)} (${routes.length} routes)`);
    return;
  }

  let contract;
  try {
    contract = JSON.parse(fs.readFileSync(CONTRACT, "utf8"));
  } catch (e) {
    fail(`contract missing/unreadable (${e.message}) - run with --record first`);
    return;
  }

  section("inventory: daemon registration == documented contract");
  const contractKeys = new Set(contract.routes.map((r) => r.method + " " + r.path));
  for (const key of contractKeys) {
    if (!daemonRoutes.has(key)) fail(`in contract but not registered by the daemon: ${key}`);
  }
  for (const key of daemonRoutes.keys()) {
    if (!contractKeys.has(key)) fail(`registered by the daemon but missing from the contract: ${key}`);
  }
  if ([...contractKeys].every((k) => daemonRoutes.has(k)) &&
      [...daemonRoutes.keys()].every((k) => contractKeys.has(k))) {
    pass(`${contractKeys.size} routes in both, no drift`);
  }

  section("probes: every contract route answers its expected status");
  for (const route of contract.routes) {
    let r;
    try {
      r = await probe(route);
    } catch (e) {
      fail(`${route.method} ${route.path}: transport error (${e.message})`);
      continue;
    }
    const expect = Array.isArray(route.expect) ? route.expect : [route.expect];
    if (!expect.includes(r.status)) {
      fail(`${route.method} ${route.path}: HTTP ${r.status} (want ${expect.join("|")})`);
      continue;
    }
    if (route.method === "SSE") {
      const ct = String(r.headers["content-type"] || "");
      if (!ct.includes("text/event-stream")) {
        fail(`${route.method} ${route.path}: content-type ${ct || "(none)"} (want text/event-stream)`);
        continue;
      }
      if (!r.body) {
        fail(`${route.method} ${route.path}: stream opened but delivered no bytes`);
        continue;
      }
    }
    pass(`${route.method} ${route.path} -> ${r.status}`);
  }

    await checkDist();
  } catch (e) {
    fail(`unexpected: ${e.stack || e.message}`);
  } finally {
    try { daemon.kill(); } catch { /* already gone */ }
  }
}

await main();
console.log(failures === 0 ? "\nAPI_PARITY_PASSED" : `\nAPI_PARITY_FAILED (${failures})`);
process.exit(failures === 0 ? 0 : 1);
