// Tests for web/js/cloud.js with synthetic Firebase streaming events (no
// network, no browser): stream event application, the frames CloudSource
// synthesizes for app.js, its failure handling, and CloudHistory.
//   node --test

const test = require("node:test");
const assert = require("node:assert/strict");
const { loadWeb } = require("./helpers/load-web");

const {
  resolveCloudConfig,
  cloudSetPath,
  cloudApplyEvent,
  parsePpgCsv,
  cloudBuildEntries,
  CloudSource,
  CloudHistory,
  CLOUD_STALE_MS,
  CLOUD_SILENCE_MS,
  CLOUD_RECONNECT_BASE_MS,
  CLOUD_HISTORY_SESSIONS,
  SESSION_SAMPLE_INTERVAL_MS,
  DEFAULT_THRESHOLDS,
} = loadWeb(
  ["protocol.js", "history.js", "cloud.js"],
  [
    "resolveCloudConfig",
    "cloudSetPath",
    "cloudApplyEvent",
    "parsePpgCsv",
    "cloudBuildEntries",
    "CloudSource",
    "CloudHistory",
    "CLOUD_STALE_MS",
    "CLOUD_SILENCE_MS",
    "CLOUD_RECONNECT_BASE_MS",
    "CLOUD_HISTORY_SESSIONS",
    "SESSION_SAMPLE_INTERVAL_MS",
    "DEFAULT_THRESHOLDS",
  ],
  { ...console, warn() {} } // the error paths below log on purpose
);

const DB = "https://pulsox-test-default-rtdb.firebaseio.com";
const CONFIG = resolveCloudConfig({ databaseURL: DB, deviceId: "pulsox-abc123" });

// ------------------------------------------------------------------ fakes

class FakeClock {
  constructor(start = 1_700_000_000_000) {
    this.t = start;
    this._timers = new Map();
    this._next = 1;
  }
  now = () => this.t;
  setTimeout = (fn, ms) => {
    const id = this._next++;
    this._timers.set(id, { at: this.t + ms, fn });
    return id;
  };
  clearTimeout = (id) => {
    this._timers.delete(id);
  };
  /** Moves time forward, running due timers in order. */
  advance(ms) {
    const end = this.t + ms;
    for (;;) {
      let due = null;
      for (const [id, timer] of this._timers) if (timer.at <= end && (!due || timer.at < due[1].at)) due = [id, timer];
      if (!due) break;
      this._timers.delete(due[0]);
      this.t = Math.max(this.t, due[1].at);
      due[1].fn();
    }
    this.t = end;
  }
}

class FakeEventSource {
  static CLOSED = 2;
  static instances = [];
  constructor(url) {
    this.url = url;
    this.readyState = 0;
    this.closed = false;
    this._handlers = {};
    FakeEventSource.instances.push(this);
  }
  addEventListener(type, fn) {
    (this._handlers[type] ||= []).push(fn);
  }
  close() {
    this.closed = true;
    this.readyState = 2;
  }
  // test drivers
  emit(type, payload) {
    const event = { data: payload === undefined ? "null" : typeof payload === "string" ? payload : JSON.stringify(payload) };
    for (const fn of this._handlers[type] || []) fn(event);
  }
  open() {
    this.readyState = 1;
    this.emit("open");
  }
  put(path, data) {
    this.emit("put", { path, data });
  }
  patch(path, data) {
    this.emit("patch", { path, data });
  }
  /** Network-level error: the browser keeps retrying (CONNECTING) unless `closed`. */
  fail({ closed = false } = {}) {
    this.readyState = closed ? 2 : 0;
    this.emit("error");
  }
}

class FakeDocument {
  constructor() {
    this.visibilityState = "visible";
    this._listeners = new Set();
  }
  addEventListener(type, fn) {
    if (type === "visibilitychange") this._listeners.add(fn);
  }
  removeEventListener(type, fn) {
    if (type === "visibilitychange") this._listeners.delete(fn);
  }
  setVisibility(state) {
    this.visibilityState = state;
    for (const fn of [...this._listeners]) fn();
  }
  get listenerCount() {
    return this._listeners.size;
  }
}

const jsonResponse = (body, status = 200) => ({ ok: status >= 200 && status < 300, status, json: async () => body });
const flush = () => new Promise((resolve) => setImmediate(resolve));

const sampleLive = (overrides = {}) => ({
  session_id: "-Nabc",
  seq: 1,
  ts: 1_700_000_000_000,
  elapsed_ms: 1000,
  finger_detected: true,
  spo2: 97.4,
  spo2_valid: true,
  bpm: 72,
  bpm_valid: true,
  signal_quality: 0.86,
  battery_pct: 78,
  fs: 50,
  ppg: "112,340,-5",
  ...overrides,
});

function makeSource({ info = null, infoStatus = 200 } = {}) {
  FakeEventSource.instances = [];
  const clock = new FakeClock();
  const document = new FakeDocument();
  const frames = [];
  const states = [];
  const fetched = [];
  const fetch = async (url) => {
    fetched.push(url);
    return jsonResponse(info, infoStatus);
  };
  const source = new CloudSource({
    config: CONFIG,
    onFrame: (f) => frames.push(f),
    onState: (s) => states.push(s),
    env: { EventSource: FakeEventSource, fetch, document, now: clock.now, setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout },
  });
  return {
    source,
    clock,
    document,
    frames,
    states,
    fetched,
    get es() {
      return FakeEventSource.instances[FakeEventSource.instances.length - 1];
    },
    types: () => frames.map((f) => f.type),
  };
}

/** Source started, stream open, empty snapshot delivered. */
function startedSource(opts) {
  const h = makeSource(opts);
  h.source.start();
  h.es.open();
  h.es.put("/", null);
  return h;
}

// ------------------------------------------------------------ configuration

test("resolveCloudConfig accepts a Firebase URL and builds the device URL", () => {
  const cfg = resolveCloudConfig({ databaseURL: `${DB}/`, deviceId: " pulsox-abc123 " });
  assert.deepEqual(cfg, { deviceUrl: `${DB}/devices/pulsox-abc123`, deviceId: "pulsox-abc123" });
  assert.ok(resolveCloudConfig({ databaseURL: "https://x-default-rtdb.europe-west1.firebasedatabase.app", deviceId: "d1" }));
  assert.ok(resolveCloudConfig({ databaseURL: "http://localhost:9000", deviceId: "d1" })); // local mock
});

test("resolveCloudConfig returns null when the cloud source is not configured or unsafe", () => {
  assert.equal(resolveCloudConfig(undefined), null);
  assert.equal(resolveCloudConfig({ databaseURL: "", deviceId: "d1" }), null);
  assert.equal(resolveCloudConfig({ databaseURL: DB, deviceId: "" }), null);
  assert.equal(resolveCloudConfig({ databaseURL: "http://evil.example.com", deviceId: "d1" }), null);
  assert.equal(resolveCloudConfig({ databaseURL: "https://evil.example.com/../x", deviceId: "d1" }), null);
  assert.equal(resolveCloudConfig({ databaseURL: DB, deviceId: "../secrets" }), null);
  assert.equal(resolveCloudConfig({ databaseURL: DB, deviceId: "a b" }), null);
});

// ----------------------------------------------------------- stream events

test("put at the root replaces everything, null clears it", () => {
  let tree = cloudApplyEvent(null, "put", "/", { a: 1, b: { c: 2 } });
  assert.deepEqual(tree, { a: 1, b: { c: 2 } });
  tree = cloudApplyEvent(tree, "put", "/", { z: 9 });
  assert.deepEqual(tree, { z: 9 });
  assert.equal(cloudApplyEvent(tree, "put", "/", null), null);
});

test("put at a path sets the child, creating parents", () => {
  let tree = cloudApplyEvent({ seq: 1 }, "put", "/seq", 2);
  assert.deepEqual(tree, { seq: 2 });
  tree = cloudApplyEvent(null, "put", "/a/b/c", 5);
  assert.deepEqual(tree, { a: { b: { c: 5 } } });
});

test("put null deletes the child and prunes parents that became empty", () => {
  let tree = { a: { b: { c: 5 } }, keep: 1 };
  tree = cloudApplyEvent(tree, "put", "/a/b/c", null);
  assert.deepEqual(tree, { keep: 1 });
  assert.equal(cloudApplyEvent({ a: { b: 1 } }, "put", "/a/b", null), null);
  assert.deepEqual(cloudApplyEvent({ a: 1 }, "put", "/x/y", null), { a: 1 }); // nothing to delete
});

test("patch merges the children it lists, keys may be multi-level paths, null deletes", () => {
  let tree = { seq: 1, spo2: 97, ppg: "1,2", nested: { x: 1, y: 2 } };
  tree = cloudApplyEvent(tree, "patch", "/", { seq: 2, spo2: 96, ppg: null, "nested/x": 10 });
  assert.deepEqual(tree, { seq: 2, spo2: 96, nested: { x: 10, y: 2 } });
  tree = cloudApplyEvent(tree, "patch", "/nested", { y: null });
  assert.deepEqual(tree, { seq: 2, spo2: 96, nested: { x: 10 } });
  tree = cloudApplyEvent(null, "patch", "/", { a: 1 });
  assert.deepEqual(tree, { a: 1 });
});

test("a path with __proto__ is ignored (no prototype pollution)", () => {
  const tree = cloudApplyEvent({}, "put", "/__proto__/polluted", true);
  assert.equal({}.polluted, undefined);
  assert.deepEqual(tree, {});
  assert.equal(cloudSetPath(null, "/a/__proto__", 1), null);
});

test("parsePpgCsv parses the ppg leaf and skips garbage", () => {
  assert.deepEqual(parsePpgCsv("112,340,-5,0"), [112, 340, -5, 0]);
  assert.deepEqual(parsePpgCsv("1,,x,2.5"), [1, 2.5]);
  assert.deepEqual(parsePpgCsv(""), []);
  assert.deepEqual(parsePpgCsv(undefined), []);
  assert.deepEqual(parsePpgCsv(42), []);
});

// ------------------------------------------------------- CloudSource: frames

test("start opens the stream on live.json and reports connecting then live", () => {
  const h = makeSource();
  h.source.start();
  assert.equal(h.es.url, `${CONFIG.deviceUrl}/live.json`);
  assert.deepEqual(h.states, ["connecting"]);
  h.es.open();
  assert.deepEqual(h.states, ["connecting", "live"]);
});

test("hello comes from info.json (with the idle battery)", async () => {
  const h = makeSource({ info: { device_id: "pulsox-esp32-01", fw_version: "0.2.0", sensor: "MAX30102", battery_pct: 64, updated: 1 } });
  h.source.start();
  await flush();
  assert.deepEqual(h.fetched, [`${CONFIG.deviceUrl}/info.json`]);
  assert.deepEqual(h.frames, [
    { type: "hello", device_id: "pulsox-esp32-01", fw_version: "0.2.0", sensor: "MAX30102", sample_rate_hz: 1, battery_pct: 64 },
  ]);
});

test("no hello when the device never wrote info (null) or the read fails", async () => {
  const empty = makeSource({ info: null });
  empty.source.start();
  await flush();
  assert.deepEqual(empty.frames, []);

  const failing = makeSource({ info: { error: "Permission denied" }, infoStatus: 401 });
  failing.source.start();
  await flush();
  assert.deepEqual(failing.frames, []);
});

test("an empty snapshot (no live node) emits nothing: the dashboard idles", () => {
  const h = startedSource();
  assert.deepEqual(h.frames, []);
});

test("a live node opens the measurement and delivers telemetry then ppg", () => {
  const h = startedSource();
  h.es.put("/", sampleLive({ elapsed_ms: 5000 }));
  assert.deepEqual(h.types(), ["measurement_start", "telemetry", "ppg"]);
  assert.deepEqual(h.frames[0], { type: "measurement_start", session_id: "-Nabc", started_at: h.clock.t - 5000 });
  assert.deepEqual(h.frames[1], {
    type: "telemetry",
    seq: 1,
    finger_detected: true,
    spo2: 97.4,
    spo2_valid: true,
    bpm: 72,
    bpm_valid: true,
    signal_quality: 0.86,
    battery_pct: 78,
  });
  assert.deepEqual(h.frames[2], { type: "ppg", fs: 50, samples: [112, 340, -5] });
});

test("started_at falls back to now without elapsed_ms; ppg fs falls back to 50", () => {
  const h = startedSource();
  const live = sampleLive();
  delete live.elapsed_ms;
  delete live.fs;
  h.es.put("/", live);
  assert.equal(h.frames[0].started_at, h.clock.t);
  assert.equal(h.frames[2].fs, 50);
});

test("absent spo2/bpm/battery (the database stores no nulls) map to the protocol's null / omitted", () => {
  const h = startedSource();
  h.es.put("/", { session_id: "s1", seq: 7, ts: 1, finger_detected: false, spo2_valid: false, bpm_valid: false, signal_quality: 0 });
  const telemetry = h.frames.find((f) => f.type === "telemetry");
  assert.equal(telemetry.spo2, null);
  assert.equal(telemetry.bpm, null);
  assert.equal(telemetry.finger_detected, false);
  assert.ok(!("battery_pct" in telemetry));
  assert.ok(!h.types().includes("ppg")); // no ppg leaf
});

test("each new seq produces one telemetry + ppg; a repeated seq produces nothing", () => {
  const h = startedSource();
  h.es.put("/", sampleLive({ seq: 1 }));
  h.frames.length = 0;

  h.es.put("/", sampleLive({ seq: 1 })); // duplicate snapshot
  h.es.put("/ts", 1_700_000_000_500); // partial update without a new seq
  assert.deepEqual(h.frames, []);

  h.es.put("/", sampleLive({ seq: 2, ppg: "7,8" }));
  assert.deepEqual(h.types(), ["telemetry", "ppg"]);
  assert.equal(h.frames[0].seq, 2);
  assert.deepEqual(h.frames[1].samples, [7, 8]);
});

test("patch events are applied on top of the local copy of live", () => {
  const h = startedSource();
  h.es.put("/", sampleLive({ seq: 1 }));
  h.frames.length = 0;

  // changed leaves only; ppg removed (no finger)
  h.es.patch("/", { seq: 2, ts: 1_700_000_001_000, spo2: 95.1, finger_detected: true, ppg: null });
  assert.deepEqual(h.types(), ["telemetry"]);
  assert.equal(h.frames[0].seq, 2);
  assert.equal(h.frames[0].spo2, 95.1);
  assert.equal(h.frames[0].bpm, 72); // carried over from the earlier snapshot
});

test("deleting live ends the measurement and re-reads info", async () => {
  const h = startedSource({ info: { device_id: "d", battery_pct: 70 } });
  await flush();
  h.es.put("/", sampleLive());
  h.frames.length = 0;
  h.fetched.length = 0;

  h.es.put("/", null);
  assert.deepEqual(h.frames, [{ type: "measurement_end", reason: "ended" }]);
  assert.deepEqual(h.fetched, [`${CONFIG.deviceUrl}/info.json`]);
  await flush();
  assert.deepEqual(h.types(), ["measurement_end", "hello"]);

  h.es.put("/", null); // a second delete changes nothing
  assert.equal(h.frames.length, 2);
});

test("a different session_id while one is open starts a new measurement", () => {
  const h = startedSource();
  h.es.put("/", sampleLive({ session_id: "s1", seq: 5 }));
  h.frames.length = 0;
  h.es.put("/", sampleLive({ session_id: "s2", seq: 1, elapsed_ms: 0 }));
  assert.deepEqual(h.types(), ["measurement_start", "telemetry", "ppg"]);
  assert.equal(h.frames[0].session_id, "s2");
});

test("a malformed live node (no session_id) is treated as no measurement", () => {
  const h = startedSource();
  h.es.put("/", { seq: 1, spo2: 97 });
  assert.deepEqual(h.frames, []);
});

// --------------------------------------------------- CloudSource: watchdogs

test("no new seq for CLOUD_STALE_MS closes the measurement (device died mid-measurement)", () => {
  const h = startedSource();
  h.es.put("/", sampleLive({ seq: 1 }));
  h.frames.length = 0;

  h.clock.advance(CLOUD_STALE_MS - 1);
  assert.deepEqual(h.frames, []);
  h.clock.advance(1);
  assert.deepEqual(h.frames, [{ type: "measurement_end", reason: "timeout" }]);
});

test("each new seq re-arms the stale timer", () => {
  const h = startedSource();
  h.es.put("/", sampleLive({ seq: 1 }));
  for (let seq = 2; seq <= 8; seq++) {
    h.clock.advance(1000);
    h.es.put("/", sampleLive({ seq }));
  }
  h.clock.advance(CLOUD_STALE_MS - 1);
  assert.ok(!h.types().includes("measurement_end"));
  h.clock.advance(1);
  assert.equal(h.types().at(-1), "measurement_end");
});

test("after a timeout the same stale snapshot is ignored; fresh data reopens the measurement", () => {
  const h = startedSource();
  h.es.put("/", sampleLive({ seq: 4 }));
  h.clock.advance(CLOUD_STALE_MS);
  h.frames.length = 0;

  h.es.put("/", sampleLive({ seq: 4 })); // e.g. a reconnect re-sends the leftover node
  assert.deepEqual(h.frames, []);

  h.es.put("/", sampleLive({ seq: 5, elapsed_ms: 15000 })); // the device is back
  assert.deepEqual(h.types(), ["measurement_start", "telemetry", "ppg"]);
  assert.equal(h.frames[0].started_at, h.clock.t - 15000);
});

test("deleting live after a timeout clears the stale marker", () => {
  const h = startedSource();
  h.es.put("/", sampleLive({ seq: 4 }));
  h.clock.advance(CLOUD_STALE_MS);
  h.es.put("/", null);
  h.frames.length = 0;
  h.es.put("/", sampleLive({ seq: 4 })); // a new measurement that happens to reuse the same numbers
  assert.deepEqual(h.types(), ["measurement_start", "telemetry", "ppg"]);
});

test("silence (no data, no keep-alive) for CLOUD_SILENCE_MS reconnects the stream", () => {
  const h = startedSource();
  const first = h.es;
  h.clock.advance(CLOUD_SILENCE_MS - 1);
  assert.equal(FakeEventSource.instances.length, 1);
  h.clock.advance(1);
  assert.ok(first.closed);
  assert.equal(h.states.at(-1), "reconnecting");
  h.clock.advance(CLOUD_RECONNECT_BASE_MS);
  assert.equal(FakeEventSource.instances.length, 2);
});

test("keep-alive events keep the connection alive", () => {
  const h = startedSource();
  for (let i = 0; i < 6; i++) {
    h.clock.advance(30_000);
    h.es.emit("keep-alive", null);
  }
  assert.equal(FakeEventSource.instances.length, 1);
  assert.equal(h.states.at(-1), "live");
});

// --------------------------------------------------- CloudSource: failures

test("a network error (browser retries by itself) reports reconnecting and forgets the measurement", () => {
  const h = startedSource();
  h.es.put("/", sampleLive({ seq: 3, elapsed_ms: 3000 }));
  const es = h.es;
  h.frames.length = 0;

  es.fail(); // readyState CONNECTING
  assert.equal(h.states.at(-1), "reconnecting");
  assert.equal(FakeEventSource.instances.length, 1); // no manual reopen
  assert.deepEqual(h.frames, []); // the app closes its measurement from the state change

  es.open(); // the browser reconnected: a fresh snapshot follows
  assert.equal(h.states.at(-1), "live");
  es.put("/", sampleLive({ seq: 3, elapsed_ms: 3000 }));
  assert.deepEqual(h.types(), ["measurement_start", "telemetry", "ppg"]); // re-opened with the right start
});

test("an HTTP error closes the stream: retry with capped exponential backoff, reset on open", () => {
  const h = startedSource();
  h.es.fail({ closed: true });
  assert.equal(h.states.at(-1), "reconnecting");
  assert.equal(FakeEventSource.instances.length, 1);

  h.clock.advance(CLOUD_RECONNECT_BASE_MS - 1);
  assert.equal(FakeEventSource.instances.length, 1);
  h.clock.advance(1);
  assert.equal(FakeEventSource.instances.length, 2);

  h.es.fail({ closed: true }); // second failure -> 2 s
  h.clock.advance(CLOUD_RECONNECT_BASE_MS * 2 - 1);
  assert.equal(FakeEventSource.instances.length, 2);
  h.clock.advance(1);
  assert.equal(FakeEventSource.instances.length, 3);

  h.es.open(); // success resets the backoff
  h.es.fail({ closed: true });
  h.clock.advance(CLOUD_RECONNECT_BASE_MS);
  assert.equal(FakeEventSource.instances.length, 4);
});

test("cancel and auth_revoked events restart the stream", () => {
  const h = startedSource();
  h.es.emit("cancel", null);
  assert.equal(h.states.at(-1), "reconnecting");
  h.clock.advance(CLOUD_RECONNECT_BASE_MS);
  assert.equal(FakeEventSource.instances.length, 2);
  h.es.open();
  h.es.emit("auth_revoked", null);
  h.clock.advance(CLOUD_RECONNECT_BASE_MS);
  assert.equal(FakeEventSource.instances.length, 3);
});

test("an event that is not valid JSON is dropped without breaking the stream", () => {
  const h = startedSource();
  h.es.emit("put", "{not json");
  h.es.emit("put", JSON.stringify({ nopath: true }));
  h.es.put("/", sampleLive());
  assert.deepEqual(h.types(), ["measurement_start", "telemetry", "ppg"]);
});

test("without EventSource the source reports offline", () => {
  const states = [];
  const source = new CloudSource({ config: CONFIG, onFrame() {}, onState: (s) => states.push(s), env: { EventSource: null } });
  source.start();
  assert.deepEqual(states, ["offline"]);
});

// -------------------------------------------------- CloudSource: lifecycle

test("coming back to the tab reopens the stream without flickering the badge", () => {
  const h = startedSource();
  h.es.put("/", sampleLive({ seq: 2 }));
  const first = h.es;
  h.frames.length = 0;
  const statesBefore = h.states.length;

  h.document.setVisibility("hidden"); // nothing happens in the background
  assert.equal(FakeEventSource.instances.length, 1);

  h.document.setVisibility("visible");
  assert.ok(first.closed);
  assert.equal(FakeEventSource.instances.length, 2);
  assert.equal(h.states.length, statesBefore); // silent reopen

  h.es.open();
  h.es.put("/", sampleLive({ seq: 2 })); // same measurement, nothing new
  assert.deepEqual(h.frames, []);
  h.es.put("/", sampleLive({ seq: 9 })); // progressed while hidden
  assert.deepEqual(h.types(), ["telemetry", "ppg"]);
});

test("if the measurement ended while the tab was hidden, the new snapshot closes it", () => {
  const h = startedSource();
  h.es.put("/", sampleLive());
  h.frames.length = 0;
  h.document.setVisibility("visible");
  h.es.open();
  h.es.put("/", null);
  assert.deepEqual(h.types(), ["measurement_end"]);
});

test("stop closes the stream, silences old events and removes the listener", () => {
  const h = startedSource();
  h.es.put("/", sampleLive());
  const es = h.es;
  h.frames.length = 0;
  const statesBefore = h.states.length;

  h.source.stop();
  assert.ok(es.closed);
  assert.equal(h.document.listenerCount, 0);
  es.put("/", sampleLive({ seq: 99 }));
  es.fail({ closed: true });
  h.clock.advance(CLOUD_SILENCE_MS * 2);
  assert.deepEqual(h.frames, []);
  assert.equal(h.states.length, statesBefore);
  assert.equal(FakeEventSource.instances.length, 1);
});

test("a source can be restarted after stop", () => {
  const h = startedSource();
  h.source.stop();
  h.source.start();
  assert.equal(FakeEventSource.instances.length, 2);
  h.es.open();
  h.es.put("/", sampleLive());
  assert.deepEqual(h.types(), ["measurement_start", "telemetry", "ppg"]);
});

// ----------------------------------------------------- CloudHistory entries

const reading = (ts, spo2, bpm) => ({ ts, spo2, bpm, quality: 0.8 });
const T0 = 1_700_000_000_000;

test("cloudBuildEntries logs the first reading, every status change and one every 20 s", () => {
  const readings = [
    reading(T0, 98, 70), // first -> logged
    reading(T0 + 5000, 98, 71), // same status, < 20 s -> skipped
    reading(T0 + 10000, 92, 71), // spo2 caution -> logged
    reading(T0 + 15000, 92, 72), // same -> skipped
    reading(T0 + 30000, 92, 72), // 20 s since last logged -> logged
  ];
  const entries = cloudBuildEntries("s1", readings, DEFAULT_THRESHOLDS);
  assert.deepEqual(
    entries.map((e) => [e.ts - T0, e.overallStatus]),
    [
      [30000, "warning"],
      [10000, "warning"],
      [0, "good"],
    ]
  );
  assert.equal(entries[2].sessionId, "s1");
  assert.equal(entries[2].spo2Status, "good");
});

test("cloudBuildEntries sorts its input, skips unknown readings and keeps partial ones", () => {
  const readings = [
    reading(T0 + 20000, 85, undefined), // critical SpO2, no bpm
    { ts: T0 + 5000 }, // nothing valid -> skipped
    reading(T0, undefined, 72), // only bpm
  ];
  const entries = cloudBuildEntries("s1", readings, DEFAULT_THRESHOLDS);
  assert.equal(entries.length, 2);
  assert.deepEqual([entries[0].ts - T0, entries[0].spo2, entries[0].bpm, entries[0].overallStatus], [20000, 85, null, "critical"]);
  assert.deepEqual([entries[1].ts - T0, entries[1].spo2, entries[1].bpm, entries[1].overallStatus], [0, null, 72, "good"]);
});

test("the status is recomputed with the viewer's thresholds", () => {
  const readings = [reading(T0, 93, 70)];
  assert.equal(cloudBuildEntries("s", readings, DEFAULT_THRESHOLDS)[0].overallStatus, "warning");
  assert.equal(cloudBuildEntries("s", readings, { ...DEFAULT_THRESHOLDS, spo2Good: 92 })[0].overallStatus, "good");
});

// ------------------------------------------------------------ CloudHistory

function makeHistory(payload, { status = 200, thresholds = DEFAULT_THRESHOLDS } = {}) {
  const calls = [];
  let current = payload;
  const history = new CloudHistory({
    config: CONFIG,
    getThresholds: () => thresholds,
    onChange: () => calls.push("change"),
    env: {
      fetch: async (url) => {
        calls.push(url);
        return jsonResponse(current, status);
      },
    },
  });
  return { history, calls, setPayload: (p) => (current = p) };
}

const readingsMap = (list) => Object.fromEntries(list.map((r, i) => [`-Nr${String(i).padStart(3, "0")}`, r]));

const SESSIONS = {
  "-Nold": {
    startedAt: T0,
    endedAt: T0 + 60000,
    readings: readingsMap([reading(T0 + 1000, 97, 70), reading(T0 + 30000, 88, 70)]),
  },
  "-Nnew": {
    startedAt: T0 + 600000,
    // the device died: no endedAt
    readings: readingsMap([reading(T0 + 601000, 96, 80), reading(T0 + 640000, 96, 81)]),
  },
  "-Nempty": { startedAt: T0 + 900000 }, // no readings -> not listed
};

test("refresh queries the last sessions ordered by startedAt and builds the groups newest first", async () => {
  const { history, calls } = makeHistory(SESSIONS);
  assert.equal(history.status, "idle");
  assert.deepEqual(history.groups(), []);

  const done = history.refresh();
  assert.equal(history.status, "loading");
  await done;

  assert.equal(calls[0], `${CONFIG.deviceUrl}/sessions.json?orderBy=%22startedAt%22&limitToLast=${CLOUD_HISTORY_SESSIONS}`);
  assert.equal(calls[1], "change");
  assert.equal(history.status, "ready");
  assert.equal(history.sessionCount, 2);

  const groups = history.groups();
  assert.deepEqual(groups.map((g) => g.session.id), ["-Nnew", "-Nold"]);
  assert.deepEqual(groups[1].rows.map((r) => r.overallStatus), ["critical", "good"]); // newest first
});

test("a measurement the device never closed reads as ended at its last reading; the open one stays 'En curso'", async () => {
  const { history } = makeHistory(SESSIONS);
  await history.refresh();

  const dead = history.groups().find((g) => g.session.id === "-Nnew");
  assert.equal(dead.session.endedAt, T0 + 640000);
  assert.equal(dead.active, false);
  assert.equal(history.groups().find((g) => g.session.id === "-Nold").session.endedAt, T0 + 60000);

  history.startSession({ id: "-Nnew" }); // the dashboard is watching this one live
  assert.equal(history.activeSessionId, "-Nnew");
  const live = history.groups().find((g) => g.session.id === "-Nnew");
  assert.equal(live.active, true);
  assert.equal(live.session.endedAt, null);
});

test("filters by severity and omits groups left empty", async () => {
  const { history } = makeHistory(SESSIONS);
  await history.refresh();
  const critical = history.groups("critical");
  assert.deepEqual(critical.map((g) => g.session.id), ["-Nold"]);
  assert.deepEqual(critical[0].rows.map((r) => r.spo2), [88]);
  assert.deepEqual(history.groups("warning"), []);
});

test("changing the thresholds recomputes the statuses", async () => {
  let thresholds = DEFAULT_THRESHOLDS;
  const history = new CloudHistory({
    config: CONFIG,
    getThresholds: () => thresholds,
    env: { fetch: async () => jsonResponse(SESSIONS) },
  });
  await history.refresh();
  assert.equal(history.groups("critical").length, 1);
  thresholds = { ...DEFAULT_THRESHOLDS, spo2Warning: 80 };
  assert.equal(history.groups("critical").length, 0);
  assert.equal(history.groups("warning").length, 1);
});

test("a failed read keeps the previous data; with none it reports an error and still notifies", async () => {
  const failing = makeHistory(null, { status: 401 });
  await failing.history.refresh();
  assert.equal(failing.history.status, "error");
  assert.ok(failing.calls.includes("change"));
  assert.deepEqual(failing.history.groups(), []);

  const { history, setPayload } = makeHistory(SESSIONS);
  await history.refresh();
  history._env.fetch = async () => {
    throw new Error("offline");
  };
  await history.refresh();
  assert.equal(history.status, "ready"); // old data still shown
  assert.equal(history.groups().length, 2);
  setPayload(null);
});

test("an empty database (null) is an empty history, not an error", async () => {
  const { history } = makeHistory(null);
  await history.refresh();
  assert.equal(history.status, "ready");
  assert.deepEqual(history.groups(), []);
  assert.equal(history.sessionCount, 0);
});

test("overlapping refreshes coalesce into one extra read", async () => {
  const { history, calls } = makeHistory(SESSIONS);
  const a = history.refresh();
  history.refresh();
  history.refresh();
  await a;
  await flush();
  await flush();
  assert.equal(calls.filter((c) => c !== "change").length, 2);
});

test("ending the measurement re-reads the history; the other mutators do nothing", async () => {
  const { history, calls } = makeHistory(SESSIONS);
  history.startSession({ id: "-Nnew" });
  assert.equal(history.addReading({ ts: 1 }), null);
  history.clear();
  history.endSession(Date.now());
  assert.equal(history.activeSessionId, null);
  await flush();
  assert.ok(calls.some((c) => typeof c === "string" && c.includes("/sessions.json")));
});

test("sessions without a numeric startedAt are skipped", async () => {
  const { history } = makeHistory({ bad: { readings: {} }, alsoBad: "x", good: SESSIONS["-Nold"] });
  await history.refresh();
  assert.deepEqual(history.groups().map((g) => g.session.id), ["good"]);
});

test("the thinning interval matches the local history", () => {
  assert.equal(SESSION_SAMPLE_INTERVAL_MS, 20000);
});
