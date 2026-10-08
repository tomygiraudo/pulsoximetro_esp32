#!/usr/bin/env node
// Local stand-in for the two Firebase services PulsOx uses, to test the whole
// cloud path (tools/cloud/fake_device.py -> database -> web app) with no Firebase
// project, no internet and no credentials:
//
//   * Realtime Database REST API: GET / PUT / POST / PATCH / DELETE on
//     /<path>.json, `{".sv":"timestamp"}`, push keys, orderBy+limitToLast, and
//     the streaming protocol (Accept: text/event-stream: initial `put`, then
//     `put` / `patch` events, `keep-alive`), with CORS headers like Firebase.
//     Optionally answers streams with a 307 to a second port (Firebase may
//     redirect the stream to another host; --redirect-port reproduces that).
//   * Firebase Auth REST (the endpoints fake_device.py / check_rules.py use):
//     signInWithPassword, signUp, delete, and the token refresh.
//
// Access control comes from the repository's own tools/cloud/database.rules.json,
// interpreted by a small subset evaluator: `.read` / `.write` (cascading, with
// `auth`) and `.validate` (checked on the written subtree and its ancestors, with
// `newData`, `now`, `$wildcard` children and `$other`). So fake_device.py's
// payloads and check_rules.py can be tried against the rules AS WRITTEN. It is a
// test double, not Firebase's rules engine (no `data`/`root`, no `.indexOn`
// enforcement, no query rules): the real service is still the final check
// (tools/cloud/check_rules.py without --mock).
//
//   node tools/cloud/mock_rtdb.js [--port 9000] [--redirect-port 9001]
//        [--keepalive-ms 30000] [--token-ttl 3600] [--rules path/to/rules.json]
//
// Fake credentials (not secrets): device@pulsox.test / mock-password, UID
// mock-device-uid; override with MOCK_EMAIL, MOCK_PASSWORD, MOCK_DEVICE_UID.

"use strict";
const http = require("node:http");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");

const PUSH_CHARS = "-0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";
const pushState = { last: 0, rand: new Array(12).fill(0) };

/** Firebase-style push key: 8 chars of time + 12 random, so keys sort chronologically. */
function createPushId() {
  let now = Date.now();
  const duplicate = now === pushState.last;
  pushState.last = now;
  let id = "";
  for (let i = 7; i >= 0; i--) {
    id = PUSH_CHARS[now % 64] + id;
    now = Math.floor(now / 64);
  }
  if (!duplicate) {
    for (let i = 0; i < 12; i++) pushState.rand[i] = Math.floor(Math.random() * 64);
  } else {
    let i = 11;
    for (; i >= 0 && pushState.rand[i] === 63; i--) pushState.rand[i] = 0;
    pushState.rand[i] += 1;
  }
  for (let i = 0; i < 12; i++) id += PUSH_CHARS[pushState.rand[i]];
  return id;
}

const isObject = (v) => v !== null && typeof v === "object" && !Array.isArray(v);

/** Replaces `{".sv":"timestamp"}` placeholders with the server time; drops nulls. */
function resolveServerValues(value, now) {
  if (isObject(value)) {
    if (value[".sv"] === "timestamp") return now;
    const out = {};
    for (const [k, v] of Object.entries(value)) {
      const resolved = resolveServerValues(v, now);
      if (resolved !== null) out[k] = resolved; // the database stores no nulls
    }
    return Object.keys(out).length ? out : null;
  }
  return value === undefined ? null : value;
}

function getAt(tree, segments) {
  let node = tree;
  for (const key of segments) {
    if (!isObject(node) || !(key in node)) return null;
    node = node[key];
  }
  return node === undefined ? null : node;
}

/** Sets (or, with null, deletes) the value at `segments`; empty parents vanish. */
function setAt(tree, segments, value) {
  if (segments.length === 0) return value;
  const root = isObject(tree) ? tree : {};
  const trail = [root];
  let node = root;
  for (let i = 0; i < segments.length - 1; i++) {
    if (!isObject(node[segments[i]])) {
      if (value === null) return Object.keys(root).length ? root : null;
      node[segments[i]] = {};
    }
    node = node[segments[i]];
    trail.push(node);
  }
  const last = segments[segments.length - 1];
  if (value === null) delete node[last];
  else node[last] = value;
  for (let i = trail.length - 1; i > 0 && Object.keys(trail[i]).length === 0; i--) delete trail[i - 1][segments[i - 1]];
  return Object.keys(root).length ? root : null;
}

// ------------------------------------------------------------ rules (subset)

/** Loads the rules file; the device-uid literal is swapped for the mock user's uid. */
function loadRules(file, deviceUid) {
  const text = fs.readFileSync(file, "utf8").replace(/auth\.uid === '[^']*'/g, `auth.uid === '${deviceUid}'`);
  return JSON.parse(text).rules;
}

/** The `newData` object rule expressions see, for a plain JSON value. */
function newDataShim(v) {
  return {
    isString: () => typeof v === "string",
    isNumber: () => typeof v === "number",
    isBoolean: () => typeof v === "boolean",
    exists: () => v !== null && v !== undefined,
    val: () => v,
    hasChildren: (keys) => isObject(v) && (keys ? keys.every((k) => k in v) : Object.keys(v).length > 0),
    hasChild: (k) => isObject(v) && k in v,
  };
}

function evalRule(expr, { auth = null, newData = null, now = Date.now() } = {}) {
  if (typeof expr === "boolean") return expr;
  // the expressions are plain JS syntax (&&, ||, comparisons, method calls on newData)
  return Boolean(new Function("auth", "newData", "now", `"use strict"; return (${expr});`)(auth, newData, now));
}

/** The rule node for `key` under `rule`: the literal key, else the `$wildcard`. */
function childRule(rule, key) {
  if (!isObject(rule)) return undefined;
  if (!key.startsWith(".") && Object.prototype.hasOwnProperty.call(rule, key)) return rule[key];
  const wildcard = Object.keys(rule).find((k) => k.startsWith("$"));
  return wildcard ? rule[wildcard] : undefined;
}

/** Rule nodes from the root down to `segments` (undefined once a path is not covered). */
function ruleChain(rules, segments) {
  const chain = [rules];
  for (const seg of segments) chain.push(childRule(chain[chain.length - 1], seg));
  return chain;
}

/** `.read` / `.write` cascade: one `true` at or above the location grants access. */
function allowed(kind, chain, ctx) {
  return chain.some((node) => isObject(node) && node[kind] !== undefined && evalRule(node[kind], ctx));
}

/** `.validate` on the new value and everything below it (deletes are not validated). */
function validateSubtree(rule, value, now) {
  if (value === null || value === undefined || !isObject(rule)) return true;
  if (rule[".validate"] !== undefined && !evalRule(rule[".validate"], { newData: newDataShim(value), now })) return false;
  if (isObject(value)) {
    for (const [key, child] of Object.entries(value)) {
      if (!validateSubtree(childRule(rule, key), child, now)) return false;
    }
  }
  return true;
}

/** `.validate` of a write at `segments`: ancestors see the post-write value too. */
function validateWrite(rules, tree, segments, now) {
  const chain = ruleChain(rules, segments);
  for (let i = 0; i < segments.length; i++) {
    const rule = chain[i];
    const value = getAt(tree, segments.slice(0, i));
    if (value === null || !isObject(rule) || rule[".validate"] === undefined) continue;
    if (!evalRule(rule[".validate"], { newData: newDataShim(value), now })) return false;
  }
  return validateSubtree(chain[segments.length], getAt(tree, segments), now);
}

function start(options = {}) {
  const port = options.port ?? 9000;
  const redirectPort = options.redirectPort ?? null;
  const keepAliveMs = options.keepAliveMs ?? 30000;
  const tokenTtlSec = options.tokenTtlSec ?? 3600;
  const deviceUid = options.deviceUid ?? process.env.MOCK_DEVICE_UID ?? "mock-device-uid";
  const email = options.email ?? process.env.MOCK_EMAIL ?? "device@pulsox.test";
  const password = options.password ?? process.env.MOCK_PASSWORD ?? "mock-password";
  const rules = loadRules(options.rulesFile ?? path.join(__dirname, "database.rules.json"), deviceUid);

  let tree = null;
  const listeners = new Set(); // { segments, res }
  const tokens = new Map(); // idToken -> { uid, exp }
  const refreshTokens = new Map(); // refreshToken -> uid
  const users = new Map(); // email -> { uid, password }
  users.set(email, { uid: deviceUid, password });

  const stats = { reads: 0, writes: 0, streams: 0, redirects: 0, denied: 0, logins: 0, refreshes: 0 };

  const cors = {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, PUT, POST, PATCH, DELETE, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type, Authorization, X-Firebase-Locale",
    "Access-Control-Max-Age": "3600",
  };

  const send = (res, status, body, extra = {}) => {
    res.writeHead(status, { ...cors, "Content-Type": "application/json; charset=utf-8", ...extra });
    res.end(body === undefined ? "" : JSON.stringify(body));
  };

  const denied = (res) => {
    stats.denied += 1;
    send(res, 401, { error: "Permission denied" });
  };

  const readBody = (req) =>
    new Promise((resolve) => {
      const chunks = [];
      req.on("data", (c) => chunks.push(c));
      req.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
    });

  const sseWrite = (res, event, data) => res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`);

  /** Tells every stream whose location is affected by a write at `writeSegs`. */
  function notify(writeSegs, kind, body) {
    for (const l of listeners) {
      const common = Math.min(l.segments.length, writeSegs.length);
      if (!l.segments.slice(0, common).every((k, i) => k === writeSegs[i])) continue; // unrelated location
      if (writeSegs.length < l.segments.length || (writeSegs.length === l.segments.length && kind !== "patch")) {
        // write above the listener, or a replace of exactly its location: it gets its whole new value
        sseWrite(l.res, "put", { path: "/", data: getAt(tree, l.segments) });
      } else if (writeSegs.length === l.segments.length) {
        sseWrite(l.res, "patch", { path: "/", data: body }); // PATCH of exactly its location: the changed children
      } else {
        const rel = "/" + writeSegs.slice(l.segments.length).join("/");
        sseWrite(l.res, kind === "patch" ? "patch" : "put", { path: rel, data: body });
      }
    }
  }

  /** The `auth` the rules see: { uid } for a valid, unexpired idToken, else null. */
  function authFor(url) {
    const token = url.searchParams.get("auth");
    const entry = token && tokens.get(token);
    return entry && entry.exp > Date.now() ? { uid: entry.uid } : null;
  }

  async function handleDatabase(req, res, url) {
    const raw = decodeURIComponent(url.pathname.replace(/\.json$/, ""));
    const segments = raw.split("/").filter(Boolean);
    const auth = authFor(url);
    const now = Date.now();

    if (req.method === "GET") {
      if (!allowed(".read", ruleChain(rules, segments), { auth, now })) return denied(res);
      if ((req.headers.accept || "").includes("text/event-stream")) return handleStream(req, res, url, segments);
      stats.reads += 1;
      let value = getAt(tree, segments);
      const orderBy = url.searchParams.get("orderBy");
      const limit = Number(url.searchParams.get("limitToLast"));
      if (orderBy && isObject(value)) {
        const field = JSON.parse(orderBy);
        const byField = ([, a], [, b]) => (a?.[field] ?? -Infinity) - (b?.[field] ?? -Infinity);
        const sorted = Object.entries(value).sort(byField);
        value = Object.fromEntries(limit > 0 ? sorted.slice(-limit) : sorted);
      }
      return send(res, 200, value);
    }

    // ---- writes
    if (!allowed(".write", ruleChain(rules, segments), { auth, now })) return denied(res);

    let body = null;
    if (req.method !== "DELETE") {
      try {
        body = JSON.parse(await readBody(req));
      } catch {
        return send(res, 400, { error: "Invalid data; couldn't parse JSON object, array, or value." });
      }
    }
    const value = resolveServerValues(body, now);
    const copy = JSON.parse(JSON.stringify(tree));
    let next;
    let written = [segments]; // the locations this write touches (for .validate)
    let notifySegs = segments;
    let notifyKind = "put";
    let notifyBody = value;
    let reply = value;

    switch (req.method) {
      case "PUT":
        next = setAt(copy, segments, value);
        break;
      case "DELETE":
        next = setAt(copy, segments, null);
        notifyBody = null;
        reply = null;
        break;
      case "POST": {
        const key = createPushId();
        notifySegs = [...segments, key];
        written = [notifySegs];
        next = setAt(copy, notifySegs, value);
        reply = { name: key };
        break;
      }
      case "PATCH": {
        if (!isObject(body)) return send(res, 400, { error: "PATCH needs a JSON object" });
        next = copy;
        notifyBody = {};
        written = [];
        for (const [k, v] of Object.entries(body)) {
          const resolved = resolveServerValues(v, now);
          const target = [...segments, ...k.split("/").filter(Boolean)];
          written.push(target);
          next = setAt(next, target, resolved);
          notifyBody[k] = resolved;
        }
        notifyKind = "patch";
        break;
      }
      default:
        return send(res, 405, { error: "Method not allowed" });
    }
    // a failed .validate is reported like a denied write (401 "Permission denied")
    if (!written.every((target) => validateWrite(rules, next, target, now))) return denied(res);

    tree = next;
    stats.writes += 1;
    notify(notifySegs, notifyKind, notifyBody);
    if (url.searchParams.get("print") === "silent") {
      res.writeHead(204, cors);
      return res.end();
    }
    return send(res, 200, reply);
  }

  function handleStream(req, res, url, segments) {
    if (redirectPort && req.socket.localPort === port) {
      stats.redirects += 1;
      const host = (req.headers.host || "localhost").replace(/:\d+$/, "");
      res.writeHead(307, { ...cors, Location: `http://${host}:${redirectPort}${url.pathname}${url.search}` });
      return res.end();
    }
    stats.streams += 1;
    res.writeHead(200, { ...cors, "Content-Type": "text/event-stream", "Cache-Control": "no-cache", Connection: "keep-alive" });
    sseWrite(res, "put", { path: "/", data: getAt(tree, segments) });
    const listener = { segments, res };
    listeners.add(listener);
    const timer = setInterval(() => sseWrite(res, "keep-alive", null), keepAliveMs);
    req.on("close", () => {
      clearInterval(timer);
      listeners.delete(listener);
    });
  }

  // ---- Firebase Auth (identitytoolkit / securetoken)

  function issueTokens(uid) {
    const idToken = `mock.${uid}.${crypto.randomBytes(12).toString("hex")}`;
    const refreshToken = `mockrefresh.${crypto.randomBytes(12).toString("hex")}`;
    tokens.set(idToken, { uid, exp: Date.now() + tokenTtlSec * 1000 });
    refreshTokens.set(refreshToken, uid);
    return { idToken, refreshToken };
  }

  async function handleAuth(req, res, url) {
    const raw = await readBody(req);
    const name = url.pathname.split("/").pop();
    if (name === "token") {
      const form = new URLSearchParams(raw);
      const uid = refreshTokens.get(form.get("refresh_token"));
      if (form.get("grant_type") !== "refresh_token" || !uid) {
        return send(res, 400, { error: { message: "INVALID_REFRESH_TOKEN" } });
      }
      stats.refreshes += 1;
      const { idToken, refreshToken } = issueTokens(uid);
      return send(res, 200, { id_token: idToken, refresh_token: refreshToken, expires_in: String(tokenTtlSec), user_id: uid });
    }
    let body;
    try {
      body = JSON.parse(raw || "{}");
    } catch {
      return send(res, 400, { error: { message: "INVALID_JSON" } });
    }
    if (name === "accounts:signInWithPassword") {
      const user = users.get(body.email);
      if (!user || user.password !== body.password) {
        return send(res, 400, { error: { message: "INVALID_LOGIN_CREDENTIALS" } });
      }
      stats.logins += 1;
      const { idToken, refreshToken } = issueTokens(user.uid);
      return send(res, 200, { idToken, refreshToken, expiresIn: String(tokenTtlSec), localId: user.uid, email: body.email });
    }
    if (name === "accounts:signUp") {
      const uid = `mock-user-${crypto.randomBytes(6).toString("hex")}`;
      if (body.email) users.set(body.email, { uid, password: body.password });
      const { idToken, refreshToken } = issueTokens(uid);
      return send(res, 200, { idToken, refreshToken, expiresIn: String(tokenTtlSec), localId: uid });
    }
    if (name === "accounts:delete") {
      tokens.delete(body.idToken);
      return send(res, 200, {});
    }
    return send(res, 404, { error: { message: "UNKNOWN_ENDPOINT" } });
  }

  function onRequest(req, res) {
    const url = new URL(req.url, "http://localhost");
    if (req.method === "OPTIONS") {
      res.writeHead(204, cors);
      return res.end();
    }
    // debugging aids (not Firebase endpoints)
    if (url.pathname === "/_stats") return send(res, 200, { ...stats, listeners: listeners.size });
    if (url.pathname === "/_tree") return send(res, 200, tree);
    if (/^\/(identitytoolkit|securetoken)\//.test(url.pathname)) return handleAuth(req, res, url);
    if (url.pathname.endsWith(".json")) return handleDatabase(req, res, url);
    return send(res, 404, { error: "Not found" });
  }

  const ports = redirectPort ? [port, redirectPort] : [port];
  const servers = ports.map(() => http.createServer(onRequest));
  return Promise.all(servers.map((server, i) => new Promise((resolve) => server.listen(ports[i], "127.0.0.1", resolve)))).then(
    () => ({
      url: `http://localhost:${port}`,
      port,
      redirectPort,
      email,
      password,
      deviceUid,
      stats,
      getTree: () => tree,
      /** Drops the open streams (like a network cut) without stopping the servers. */
      dropStreams: () => {
        for (const l of listeners) l.res.destroy();
        listeners.clear();
      },
      close: () =>
        new Promise((resolve) => {
          for (const l of listeners) l.res.destroy();
          listeners.clear();
          let open = servers.length;
          for (const s of servers) {
            s.closeAllConnections?.();
            s.close(() => {
              open -= 1;
              if (open === 0) resolve();
            });
          }
        }),
    })
  );
}

module.exports = { start };

if (require.main === module) {
  const args = process.argv.slice(2);
  const arg = (name, fallback) => {
    const i = args.indexOf(`--${name}`);
    return i >= 0 ? args[i + 1] : fallback;
  };
  start({
    port: Number(arg("port", 9000)),
    redirectPort: arg("redirect-port") ? Number(arg("redirect-port")) : null,
    keepAliveMs: Number(arg("keepalive-ms", 30000)),
    tokenTtlSec: Number(arg("token-ttl", 3600)),
    rulesFile: arg("rules"),
  }).then((mock) => {
    const redirect = mock.redirectPort ? ` (streams redirected to :${mock.redirectPort})` : "";
    console.log(`mock RTDB + Auth listening on ${mock.url}${redirect}`);
    console.log(`  databaseURL   ${mock.url}`);
    console.log(`  device login  ${mock.email} / ${mock.password}  (uid ${mock.deviceUid})`);
    console.log("  a test double, not a security emulation: see the header of this file");
  });
}
