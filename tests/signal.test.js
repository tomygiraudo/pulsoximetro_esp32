// Tests for web/js/signal.js (the recorded PPG signal of each measurement) and for
// the `ppg` rules of tools/cloud/database.rules.json, which are run by the local
// mock of Firebase (tools/cloud/mock_rtdb.js) over real HTTP.
//   node --test

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const net = require("node:net");
const { loadWeb } = require("./helpers/load-web");
const mockRtdb = require("../tools/cloud/mock_rtdb");

const {
  resolveCloudConfig,
  signalEncode,
  signalDecode,
  signalCount,
  signalToWire,
  signalFromWire,
  SignalRecorder,
  LocalSignalStore,
  CloudSignalStore,
  SIGNAL_MAX_SAMPLES,
  SIGNAL_NONE_TTL_MS,
  SIGNAL_SAVE_RETRY_MS,
  SIGNAL_LOCAL_PREFIX,
} = loadWeb(
  ["protocol.js", "history.js", "cloud.js", "signal.js"],
  [
    "resolveCloudConfig",
    "signalEncode",
    "signalDecode",
    "signalCount",
    "signalToWire",
    "signalFromWire",
    "SignalRecorder",
    "LocalSignalStore",
    "CloudSignalStore",
    "SIGNAL_MAX_SAMPLES",
    "SIGNAL_NONE_TTL_MS",
    "SIGNAL_SAVE_RETRY_MS",
    "SIGNAL_LOCAL_PREFIX",
  ],
  { ...console, warn() {} } // the failure paths below log on purpose
);

const ramp = (n, from = 0) => Array.from({ length: n }, (_, i) => from + i);

// ---------------------------------------------------------------- encoding

test("a signal survives the wire format, gaps included", () => {
  const values = [112, null, null, -5, 0.5, 340, null];
  assert.equal(signalEncode(values), "112,,,-5,0.5,340,");
  assert.deepEqual(signalDecode("112,,,-5,0.5,340,"), values);
  assert.equal(signalCount(values), 4);
  const signal = { fs: 50, n: 4, values };
  assert.deepEqual(signalFromWire(signalToWire(signal)), signal);
});

test("rounds to two decimals and writes a gap for anything that is not a finite number", () => {
  assert.equal(signalEncode([1.23456, NaN, Infinity, undefined, "7"]), "1.23,,,,");
});

test("signalFromWire rejects what cannot be drawn", () => {
  assert.equal(signalFromWire(null), null);
  assert.equal(signalFromWire({ fs: 50, n: 3 }), null); // no data
  assert.equal(signalFromWire({ fs: 0, n: 3, data: "1,2,3" }), null); // no rate
  assert.equal(signalFromWire({ fs: "50", n: 3, data: "1,2,3" }), null);
  assert.equal(signalFromWire({ fs: 50, n: 1, data: "5,,," }), null); // one sample is not a trace
  assert.equal(signalFromWire({ fs: 50, n: 2, data: "1,abc,3" }).n, 2); // garbage is a gap
});

// ---------------------------------------------------------------- recorder

test("batches without a seq are appended as they come", () => {
  const rec = new SignalRecorder();
  rec.begin("m-1");
  rec.push([1, 2, 3], 50);
  rec.push([4, 5], 50);
  assert.equal(rec.sessionId, "m-1");
  assert.equal(rec.count, 5);
  assert.equal(rec.snapshot(), null); // far from SIGNAL_MIN_SECONDS
});

test("a second the page did not see becomes a gap of fs samples", () => {
  const rec = new SignalRecorder();
  rec.begin("-Nabc");
  rec.push(ramp(50), 50, 1);
  rec.push(ramp(50, 50), 50, 2);
  rec.push(ramp(50, 200), 50, 5); // seconds 3 and 4 were lost
  for (let seq = 6; seq <= 10; seq++) rec.push(ramp(50), 50, seq);
  const { values, n } = rec.snapshot();
  assert.equal(values.length, 10 * 50);
  assert.equal(values.slice(100, 200).every((v) => v === null), true);
  assert.equal(values[199], null);
  assert.equal(values[200], 200);
  assert.equal(n, 8 * 50);
});

test("joining late leaves the seconds before as a leading gap", () => {
  const rec = new SignalRecorder();
  rec.begin("-Nabc");
  rec.push(ramp(50), 50, 21); // seq 1..20 never seen
  for (let seq = 22; seq <= 30; seq++) rec.push(ramp(50), 50, seq);
  const snap = rec.snapshot();
  assert.equal(snap.values.length, 30 * 50);
  assert.equal(snap.values.slice(0, 1000).every((v) => v === null), true);
  assert.equal(snap.n, 10 * 50);
});

test("a repeated or older second is ignored", () => {
  const rec = new SignalRecorder();
  rec.begin("-Nabc");
  rec.push(ramp(50), 50, 1);
  rec.push(ramp(50), 50, 2);
  rec.push(ramp(50), 50, 2);
  rec.push(ramp(50), 50, 1);
  assert.equal(rec.count, 100);
});

test("a change of sample rate mid-measurement is ignored, not mixed in", () => {
  const rec = new SignalRecorder();
  rec.begin("m-1");
  rec.push(ramp(50), 50);
  rec.push(ramp(100), 100);
  assert.equal(rec.count, 50);
});

test("it stops at the cap that the rules enforce", () => {
  const rec = new SignalRecorder();
  rec.begin("m-1");
  for (let i = 0; i < 200; i++) rec.push(ramp(50), 50); // 10 000 samples offered
  assert.equal(rec.count, SIGNAL_MAX_SAMPLES);
  assert.equal(rec.snapshot().values.length, SIGNAL_MAX_SAMPLES);
  rec.begin("m-2"); // a gap can not push it past the cap either
  for (let seq = 1; seq <= 6; seq++) rec.push(ramp(50), 50, seq);
  rec.push(ramp(50), 50, 500);
  assert.equal(rec.snapshot().values.length, SIGNAL_MAX_SAMPLES);
  assert.equal(rec.count, 300);
});

test("begin() starts clean and nothing is recorded before it", () => {
  const rec = new SignalRecorder();
  rec.push(ramp(500), 50);
  assert.equal(rec.count, 0);
  rec.begin("m-1");
  rec.push(ramp(500), 50);
  assert.equal(rec.snapshot().n, 500);
  rec.begin("m-2");
  assert.equal(rec.count, 0);
  assert.equal(rec.snapshot(), null);
});

test("the snapshot is a copy and ignores samples that are not numbers", () => {
  const rec = new SignalRecorder();
  rec.begin("m-1");
  rec.push([...ramp(250), "x", NaN, null], 50);
  const snap = rec.snapshot();
  assert.equal(snap.n, 250);
  rec.push(ramp(50), 50);
  assert.equal(snap.values.length, 250);
});

// ------------------------------------------------------------ local store

class FakeStorage {
  constructor({ quotaChars = Infinity } = {}) {
    this.map = new Map();
    this.quotaChars = quotaChars;
  }
  getItem(k) {
    return this.map.has(k) ? this.map.get(k) : null;
  }
  setItem(k, v) {
    const others = [...this.map].filter(([key]) => key !== k).reduce((sum, [, val]) => sum + val.length, 0);
    if (others + v.length > this.quotaChars) throw new Error("QuotaExceededError");
    this.map.set(k, v);
  }
  removeItem(k) {
    this.map.delete(k);
  }
  get keys() {
    return [...this.map.keys()].filter((k) => k.startsWith(SIGNAL_LOCAL_PREFIX) && !k.endsWith("index"));
  }
}

const sig = (n = 300, from = 0) => ({ fs: 50, n, values: ramp(n, from) });

test("LocalSignalStore saves and loads a signal; unknown or corrupt ones read as none", async () => {
  const storage = new FakeStorage();
  const store = new LocalSignalStore({ storage });
  assert.equal(await store.save("m-1", sig()), "saved");
  const res = await store.load("m-1");
  assert.equal(res.status, "ok");
  assert.deepEqual(res.signal, sig());
  assert.deepEqual(await store.load("m-2"), { status: "none" });
  storage.map.set(SIGNAL_LOCAL_PREFIX + "m-3", "{not json");
  assert.deepEqual(await store.load("m-3"), { status: "none" });
});

test("LocalSignalStore keeps only the newest `max` signals", async () => {
  const storage = new FakeStorage();
  const store = new LocalSignalStore({ storage, max: 2 });
  for (const id of ["a", "b", "c"]) await store.save(id, sig());
  assert.deepEqual(storage.keys.sort(), [SIGNAL_LOCAL_PREFIX + "b", SIGNAL_LOCAL_PREFIX + "c"]);
  assert.equal((await store.load("a")).status, "none");
  await store.save("b", sig(300, 1)); // saving again moves it to the newest
  await store.save("d", sig());
  assert.deepEqual(storage.keys.sort(), [SIGNAL_LOCAL_PREFIX + "b", SIGNAL_LOCAL_PREFIX + "d"]);
});

test("LocalSignalStore frees the oldest signals when the browser's quota is full", async () => {
  const one = JSON.stringify(signalToWire(sig())).length;
  const storage = new FakeStorage({ quotaChars: one * 2.5 + 200 });
  const store = new LocalSignalStore({ storage });
  for (const id of ["a", "b", "c"]) assert.equal(await store.save(id, sig()), "saved");
  assert.equal((await store.load("a")).status, "none");
  assert.equal((await store.load("c")).status, "ok");
});

test("LocalSignalStore gives up when even an empty store can not hold the signal", async () => {
  const store = new LocalSignalStore({ storage: new FakeStorage({ quotaChars: 50 }) });
  assert.equal(await store.save("a", sig()), "failed");
});

test("LocalSignalStore.clear removes every signal", async () => {
  const storage = new FakeStorage();
  const store = new LocalSignalStore({ storage });
  await store.save("a", sig());
  await store.save("b", sig());
  store.clear();
  assert.equal(storage.map.size, 0);
  assert.equal((await store.load("a")).status, "none");
});

test("LocalSignalStore without a storage does nothing instead of throwing", async () => {
  const store = new LocalSignalStore(); // `localStorage` does not exist under node
  assert.equal(await store.save("a", sig()), "failed");
  assert.deepEqual(await store.load("a"), { status: "none" });
  store.clear();
});

// ------------------------------------------------------------ cloud store

const DB = "https://pulsox-test-default-rtdb.firebaseio.com";
const CONFIG = resolveCloudConfig({ databaseURL: DB, deviceId: "pulsox-abc123" });
const DEV = `${DB}/devices/pulsox-abc123`;

const jsonRes = (body, status = 200) => ({ ok: status >= 200 && status < 300, status, json: async () => body });

/** A fake fetch that answers from `db` ({ "sessions/x/endedAt": 123, ... }) and records the calls. */
function makeFake({ db = {}, putStatus = 204, putFailures = 0 } = {}) {
  const calls = [];
  const sleeps = [];
  let failures = putFailures;
  let t = 1_700_000_000_000;
  const env = {
    now: () => t,
    sleep: async (ms) => {
      sleeps.push(ms);
      t += ms;
    },
    fetch: async (url, opts = {}) => {
      calls.push({ url, method: opts.method || "GET", body: opts.body });
      if (opts.method === "PUT") {
        if (failures > 0) {
          failures--;
          throw new TypeError("network down");
        }
        return jsonRes(null, putStatus);
      }
      const key = url.slice(DEV.length + 1).replace(/\.json.*$/, "");
      if (key in db && db[key] instanceof Error) throw db[key];
      return jsonRes(key in db ? db[key] : null, db.__status ?? 200);
    },
  };
  return { env, calls, sleeps, advance: (ms) => (t += ms), puts: () => calls.filter((c) => c.method === "PUT") };
}

const wire = (signal) => signalToWire(signal);

test("save: writes ppg/<sid> once the device closed the measurement", async () => {
  const f = makeFake({ db: { "sessions/-Nabc/endedAt": 1_700_000_000_000 } });
  const store = new CloudSignalStore({ config: CONFIG, env: f.env });
  const signal = { fs: 50, n: 4, values: [5, null, 7, 8, 9] };
  assert.equal(await store.save("-Nabc", signal), "saved");
  const [put] = f.puts();
  assert.equal(put.url, `${DEV}/ppg/-Nabc.json?print=silent`);
  assert.deepEqual(JSON.parse(put.body), { fs: 50, n: 4, data: "5,,7,8,9" });
  // opening it afterwards needs no download
  const before = f.calls.length;
  const res = await store.load("-Nabc");
  assert.equal(res.status, "ok");
  assert.deepEqual(res.signal, signal);
  assert.equal(f.calls.length, before);
});

test("save: a measurement the device discarded or never closed is not saved", async () => {
  const f = makeFake({ db: {} }); // no endedAt
  const store = new CloudSignalStore({ config: CONFIG, env: f.env });
  assert.equal(await store.save("-Ngone", sig()), "skipped");
  assert.equal(f.puts().length, 0);
});

test("save: does not upload what someone who saw all of it already stored", async () => {
  const f = makeFake({ db: { "sessions/-Nabc/endedAt": 1, "ppg/-Nabc/n": 2500 } });
  const store = new CloudSignalStore({ config: CONFIG, env: f.env });
  assert.equal(await store.save("-Nabc", sig(2000)), "exists");
  assert.equal(f.puts().length, 0);
  assert.equal(await store.save("-Nabc", sig(2600)), "saved"); // a longer one does replace it
  assert.equal(f.puts().length, 1);
});

test("save: retries a dropped connection with the configured waits", async () => {
  const f = makeFake({ db: { "sessions/-Nabc/endedAt": 1 }, putFailures: 2 });
  const store = new CloudSignalStore({ config: CONFIG, env: f.env });
  assert.equal(await store.save("-Nabc", sig()), "saved");
  assert.equal(f.puts().length, 3);
  assert.deepEqual(f.sleeps, SIGNAL_SAVE_RETRY_MS);
});

test("save: gives up after three attempts", async () => {
  const f = makeFake({ db: { "sessions/-Nabc/endedAt": 1 }, putFailures: 99 });
  const store = new CloudSignalStore({ config: CONFIG, env: f.env });
  assert.equal(await store.save("-Nabc", sig()), "failed");
  assert.equal(f.puts().length, 3);
  assert.equal((await store.load("-Nabc")).status, "none"); // nothing was kept in memory
});

test("save: a refusal by the rules is final (no retries)", async () => {
  const f = makeFake({ db: { "sessions/-Nabc/endedAt": 1 }, putStatus: 401 });
  const store = new CloudSignalStore({ config: CONFIG, env: f.env });
  assert.equal(await store.save("-Nabc", sig()), "denied");
  assert.equal(f.puts().length, 1);
  assert.deepEqual(f.sleeps, []);
});

test("save: still tries the write when the pre-checks can not be read", async () => {
  const f = makeFake({ db: { "sessions/-Nabc/endedAt": new TypeError("offline") } });
  const store = new CloudSignalStore({ config: CONFIG, env: f.env });
  assert.equal(await store.save("-Nabc", sig()), "saved");
});

test("save and load refuse ids that are not push keys", async () => {
  const f = makeFake();
  const store = new CloudSignalStore({ config: CONFIG, env: f.env });
  for (const bad of ["", "a/b", "../x", "x.json", "a b", "x".repeat(41)]) {
    assert.equal(await store.save(bad, sig()), "skipped");
    assert.deepEqual(await store.load(bad), { status: "none" });
  }
  assert.equal(f.calls.length, 0);
});

test("load: decodes the stored signal and remembers it", async () => {
  const stored = { fs: 50, n: 3, data: "1,,2,3" };
  const f = makeFake({ db: { "ppg/-Nabc": stored } });
  const store = new CloudSignalStore({ config: CONFIG, env: f.env });
  const res = await store.load("-Nabc");
  assert.deepEqual(res, { status: "ok", signal: { fs: 50, n: 3, values: [1, null, 2, 3] } });
  await store.load("-Nabc");
  assert.equal(f.calls.length, 1);
});

test("load: 'nothing stored' is asked again after a while, a stored signal never", async () => {
  const f = makeFake({ db: {} });
  const store = new CloudSignalStore({ config: CONFIG, env: f.env });
  assert.deepEqual(await store.load("-Nabc"), { status: "none" });
  assert.deepEqual(await store.load("-Nabc"), { status: "none" });
  assert.equal(f.calls.length, 1);
  f.advance(SIGNAL_NONE_TTL_MS + 1);
  await store.load("-Nabc");
  assert.equal(f.calls.length, 2);
});

test("load: a read error is reported and not remembered", async () => {
  const f = makeFake({ db: { "ppg/-Nabc": new TypeError("offline") } });
  const store = new CloudSignalStore({ config: CONFIG, env: f.env });
  assert.deepEqual(await store.load("-Nabc"), { status: "error" });
  assert.deepEqual(await store.load("-Nabc"), { status: "error" });
  assert.equal(f.calls.length, 2);
  const f2 = makeFake({ db: { __status: 500 } });
  assert.deepEqual(await new CloudSignalStore({ config: CONFIG, env: f2.env }).load("-Nabc"), { status: "error" });
});

test("load: two callers at once share one download", async () => {
  const f = makeFake({ db: { "ppg/-Nabc": wire(sig()) } });
  const store = new CloudSignalStore({ config: CONFIG, env: f.env });
  const [a, b] = await Promise.all([store.load("-Nabc"), store.load("-Nabc")]);
  assert.equal(a.status, "ok");
  assert.equal(b.status, "ok");
  assert.equal(f.calls.length, 1);
});

// ------------------------------------------- the rules, on the mock (real HTTP)

const freePort = () =>
  new Promise((resolve) => {
    const s = net.createServer().listen(0, "127.0.0.1", () => {
      const { port } = s.address();
      s.close(() => resolve(port));
    });
  });

/** A mock whose device session is closed (`endedAt`), and helpers to talk to it. */
async function rulesHarness({ rulesFile } = {}) {
  const mock = await mockRtdb.start({ port: await freePort(), rulesFile });
  const base = `${mock.url}/devices/pulsox-rules`;
  const login = await fetch(`${mock.url}/identitytoolkit/v1/accounts:signInWithPassword?key=k`, {
    method: "POST",
    body: JSON.stringify({ email: mock.email, password: mock.password }),
  }).then((r) => r.json());
  const auth = `auth=${encodeURIComponent(login.idToken)}`;
  const device = async (method, p, body) => {
    const res = await fetch(`${base}/${p}.json?${auth}`, { method, body: body === undefined ? undefined : JSON.stringify(body) });
    assert.equal(res.status, 200, `device ${method} ${p} -> ${res.status}`);
    return res.json();
  };
  const anon = (method, p, body) =>
    fetch(`${base}/${p}.json`, { method, body: body === undefined ? undefined : JSON.stringify(body) }).then((r) => r.status);
  const openSession = async ({ close = true } = {}) => {
    const { name } = await device("POST", "sessions", { startedAt: { ".sv": "timestamp" } });
    if (close) await device("PATCH", `sessions/${name}`, { endedAt: { ".sv": "timestamp" } });
    return name;
  };
  return { mock, base, device, anon, openSession };
}

const stored = (n, extra = {}) => ({ fs: 50, n, data: ramp(n).join(","), ...extra });

test("rules: anyone can save the signal of a closed measurement, and read it back", async () => {
  const h = await rulesHarness();
  try {
    const sid = await h.openSession();
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(300)), 200);
    const back = await fetch(`${h.base}/ppg/${sid}.json`).then((r) => r.json());
    assert.deepEqual(back, stored(300));
  } finally {
    await h.mock.close();
  }
});

test("rules: only for a session that exists and was closed", async () => {
  const h = await rulesHarness();
  try {
    assert.equal(await h.anon("PUT", "ppg/-Nnope", stored(300)), 401); // no such session
    const open = await h.openSession({ close: false });
    assert.equal(await h.anon("PUT", `ppg/${open}`, stored(300)), 401); // still open: no endedAt
    assert.equal(await h.anon("PUT", "ppg/-Nnope2", stored(300)), 401);
    assert.equal(h.mock.getTree()?.devices?.["pulsox-rules"]?.ppg ?? null, null); // nothing got in
  } finally {
    await h.mock.close();
  }
});

test("rules: a longer recording replaces a shorter one, never the other way", async () => {
  const h = await rulesHarness();
  try {
    const sid = await h.openSession();
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(300)), 200);
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(300)), 401); // same length
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(200)), 401); // shorter
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(400)), 200);
    const back = await fetch(`${h.base}/ppg/${sid}/n.json`).then((r) => r.json());
    assert.equal(back, 400);
  } finally {
    await h.mock.close();
  }
});

test("rules: size, shape and unknown fields are enforced", async () => {
  const h = await rulesHarness();
  try {
    const sid = await h.openSession();
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(300, { data: "1,".repeat(20001) })), 401); // > 40 000 chars
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(0)), 401);
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(6001)), 401);
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(300, { fs: 0 })), 401);
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(300, { fs: 5000 })), 401);
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(300, { data: 12345 })), 401);
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(300, { hack: "x" })), 401);
    assert.equal(await h.anon("PUT", `ppg/${sid}`, { fs: 50, n: 300 }), 401); // no data
    assert.equal(await h.anon("PUT", `ppg/${sid}`, "just a string"), 401);
  } finally {
    await h.mock.close();
  }
});

test("rules: a full-size recording (the cap of n and of the text) fits", async () => {
  const h = await rulesHarness();
  try {
    const sid = await h.openSession();
    // 6000 samples of up to 5 characters each stay inside the 40 000-character cap
    const data = Array.from({ length: 6000 }, (_, i) => (i % 2 ? -1234 : 1234)).join(",");
    assert.ok(data.length < 40000, `test data is ${data.length} chars`);
    assert.equal(await h.anon("PUT", `ppg/${sid}`, { fs: 50, n: 6000, data }), 200);
  } finally {
    await h.mock.close();
  }
});

test("rules: nobody but the device can delete it, and the rest of the database stays closed", async () => {
  const h = await rulesHarness();
  try {
    const sid = await h.openSession();
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(300)), 200);
    assert.equal(await h.anon("DELETE", `ppg/${sid}`), 401);
    assert.equal(await h.anon("PUT", `ppg/${sid}`, null), 401);
    assert.equal(await h.anon("PUT", `ppg/${sid}/data`, "0"), 401); // not in pieces either
    assert.equal(await h.anon("PUT", "live", { session_id: sid, seq: 1, ts: { ".sv": "timestamp" } }), 401);
    assert.equal(await h.anon("PUT", "info", { device_id: "x", updated: { ".sv": "timestamp" } }), 401);
    assert.equal(await h.anon("PATCH", `sessions/${sid}`, { endedAt: 1 }), 401);
    assert.equal(await h.anon("DELETE", `sessions/${sid}`), 401);
    await h.device("DELETE", `ppg/${sid}`); // the device (its uid) can
    assert.equal((await fetch(`${h.base}/ppg/${sid}.json`).then((r) => r.json())) ?? null, null);
  } finally {
    await h.mock.close();
  }
});

test("rules: the signal can only be saved for a few minutes after the measurement ends", async () => {
  const rulesFile = path.join(os.tmpdir(), `pulsox-rules-${process.pid}-${Date.now()}.json`);
  const original = fs.readFileSync(path.join(__dirname, "..", "tools", "cloud", "database.rules.json"), "utf8");
  assert.ok(original.includes("+ 600000"), "the window should be 10 minutes in the real rules");
  fs.writeFileSync(rulesFile, original.replace("+ 600000", "+ 700")); // same rules, a 0.7 s window
  const h = await rulesHarness({ rulesFile });
  try {
    const sid = await h.openSession();
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(300)), 200); // inside the window
    await new Promise((r) => setTimeout(r, 900));
    assert.equal(await h.anon("PUT", `ppg/${sid}`, stored(400)), 401); // after it: frozen
    assert.equal((await fetch(`${h.base}/ppg/${sid}/n.json`).then((r) => r.json())), 300);
  } finally {
    await h.mock.close();
    fs.rmSync(rulesFile, { force: true });
  }
});

test("rules: the real CloudSignalStore saves and loads through them", async () => {
  const h = await rulesHarness();
  try {
    const config = resolveCloudConfig({ databaseURL: h.mock.url, deviceId: "pulsox-rules" });
    const store = new CloudSignalStore({ config });
    const signal = { fs: 50, n: 300, values: [...ramp(100), ...Array(50).fill(null), ...ramp(200)].slice(0, 350) };
    signal.n = signalCount(signal.values);

    const open = await h.openSession({ close: false });
    assert.equal(await store.save(open, signal), "skipped"); // not closed yet
    const sid = await h.openSession();
    assert.equal(await store.save(sid, signal), "saved");
    assert.equal(await store.save(sid, signal), "exists"); // a second viewer has nothing to add

    const fresh = new CloudSignalStore({ config }); // another browser
    const res = await fresh.load(sid);
    assert.equal(res.status, "ok");
    assert.deepEqual(res.signal, signal);
    assert.deepEqual(await fresh.load("-Nnothing"), { status: "none" });
  } finally {
    await h.mock.close();
  }
});
