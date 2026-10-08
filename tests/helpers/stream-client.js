// Child process of tests/stream-integration.test.js (run with --experimental-eventsource).
// Runs the REAL web/js/cloud.js against tools/cloud/mock_rtdb.js over HTTP with
// Node's built-in EventSource: SSE parsing, the 307 redirect Firebase may send
// before a stream, `put` / `patch` events, keep-alive, a dropped stream and the
// automatic reconnection. (Browser-only concerns — CORS, mixed content — are not
// covered here; see the README for the manual checks.)

const assert = require("node:assert/strict");
const net = require("node:net");
const { loadWeb } = require("./load-web");
const mockRtdb = require("../../tools/cloud/mock_rtdb");

const { resolveCloudConfig, CloudSource, CloudHistory, DEFAULT_THRESHOLDS } = loadWeb(
  ["protocol.js", "history.js", "cloud.js"],
  ["resolveCloudConfig", "CloudSource", "CloudHistory", "DEFAULT_THRESHOLDS"],
  { ...console, warn() {} }
);

const freePort = () =>
  new Promise((resolve) => {
    const s = net.createServer().listen(0, "127.0.0.1", () => {
      const { port } = s.address();
      s.close(() => resolve(port));
    });
  });

async function waitFor(label, cond, ms = 8000) {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > ms) throw new Error(`timeout waiting for: ${label}`);
    await new Promise((r) => setTimeout(r, 20));
  }
}

async function main() {
  assert.equal(typeof EventSource, "function", "run with --experimental-eventsource");
  const mock = await mockRtdb.start({ port: await freePort(), redirectPort: await freePort(), keepAliveMs: 200 });
  const config = resolveCloudConfig({ databaseURL: mock.url, deviceId: "pulsox-itest" });
  const base = `${mock.url}/devices/pulsox-itest`;

  const login = await fetch(`${mock.url}/identitytoolkit/v1/accounts:signInWithPassword?key=k`, {
    method: "POST",
    body: JSON.stringify({ email: mock.email, password: mock.password }),
  }).then((r) => r.json());
  const auth = `auth=${encodeURIComponent(login.idToken)}`;
  const write = async (method, path, body) => {
    const res = await fetch(`${base}/${path}.json?${auth}`, { method, body: body === undefined ? undefined : JSON.stringify(body) });
    assert.equal(res.status, 200, `${method} ${path} -> ${res.status}`);
    return res.json();
  };

  // anonymous writes are refused, reads are public
  assert.equal((await fetch(`${base}/live.json`, { method: "PUT", body: "{}" })).status, 401);
  assert.equal((await fetch(`${base}/info.json`)).status, 200);

  await write("PUT", "info", { device_id: "pulsox-itest", fw_version: "t", sensor: "MAX30102", battery_pct: 55, updated: { ".sv": "timestamp" } });

  const frames = [];
  const states = [];
  const source = new CloudSource({ config, onFrame: (f) => frames.push(f), onState: (s) => states.push(s) });
  source.start();
  await waitFor("stream live", () => states.includes("live"));
  assert.ok(mock.stats.redirects >= 1, "the stream request was redirected (307) and followed");
  await waitFor("hello", () => frames.some((f) => f.type === "hello"));
  assert.equal(frames.find((f) => f.type === "hello").battery_pct, 55);

  // a measurement: PUT, PATCH (changed leaves only), PUT with a new field, then DELETE
  const live = (seq, extra = {}) => ({
    session_id: "-Nsess1", seq, ts: { ".sv": "timestamp" }, elapsed_ms: seq * 1000,
    finger_detected: true, spo2: 97, spo2_valid: true, bpm: 70, bpm_valid: true, signal_quality: 0.8, fs: 50, ppg: "1,2,3", ...extra,
  });
  await write("PUT", "live", live(1));
  await waitFor("first telemetry", () => frames.some((f) => f.type === "telemetry" && f.seq === 1));
  await write("PATCH", "live", { seq: 2, spo2: 96.5, ppg: "4,5" });
  await waitFor("patched telemetry", () => frames.some((f) => f.type === "telemetry" && f.seq === 2));
  await write("PUT", "live", live(3, { battery_pct: 54 }));
  await waitFor("third telemetry", () => frames.some((f) => f.type === "telemetry" && f.seq === 3));
  await write("DELETE", "live");
  await waitFor("measurement end", () => frames.some((f) => f.type === "measurement_end"));

  const types = frames.filter((f) => f.type !== "hello").map((f) => f.type);
  assert.deepEqual(types, [
    "measurement_start", "telemetry", "ppg",
    "telemetry", "ppg",
    "telemetry", "ppg",
    "measurement_end",
  ]);
  const t2 = frames.find((f) => f.type === "telemetry" && f.seq === 2);
  assert.equal(t2.spo2, 96.5); // from the patch
  assert.equal(t2.bpm, 70); // carried over
  assert.deepEqual(frames.filter((f) => f.type === "ppg")[1].samples, [4, 5]);
  assert.equal(frames.find((f) => f.type === "telemetry" && f.seq === 3).battery_pct, 54);

  // a dropped stream: reported, then the browser-side retry reconnects and re-syncs
  const before = states.length;
  mock.dropStreams();
  await waitFor("reconnecting", () => states.slice(before).includes("reconnecting"));
  await waitFor("live again", () => states.slice(before).includes("live"), 12000);
  await write("PUT", "live", live(10, { session_id: "-Nsess2" }));
  await waitFor("measurement 2", () => frames.some((f) => f.type === "measurement_start" && f.session_id === "-Nsess2"));

  // history over the same server
  const sid = (await write("POST", "sessions", { startedAt: { ".sv": "timestamp" } })).name;
  await write("POST", `sessions/${sid}/readings`, { ts: { ".sv": "timestamp" }, spo2: 97, bpm: 72, quality: 0.8 });
  await write("PATCH", `sessions/${sid}`, { endedAt: { ".sv": "timestamp" } });
  const history = new CloudHistory({ config, getThresholds: () => DEFAULT_THRESHOLDS });
  await history.refresh();
  assert.equal(history.status, "ready");
  const groups = history.groups();
  assert.equal(groups.length, 1);
  assert.equal(groups[0].session.id, sid);
  assert.equal(groups[0].rows[0].overallStatus, "good");

  source.stop();
  await mock.close();
  console.log("STREAM-INTEGRATION-OK");
}

main().then(
  () => process.exit(0),
  (err) => {
    console.error(err);
    process.exit(1);
  }
);
