// PPG signal of each measurement, so the Historial can show it ("Ver señal").
//
// The device only streams the signal while it measures (devices/<id>/live.ppg is
// replaced every second and deleted at the end), and it must not spend RAM or
// time on keeping it: the page that WATCHES the measurement records it instead,
// then saves it complete once the measurement is over.
//
//   SignalRecorder     collects the batches of the open measurement. Over the cloud
//                      transport every batch carries the `seq` of its second, so a
//                      second this page did not see stays a visible gap (null).
//   CloudSignalStore   devices/<id>/ppg/<sid> = { fs, n, data: "112,340,,,-5,…" }.
//                      Written by the page (tools/cloud/database.rules.json lets it,
//                      for a closed session, for a few minutes, and only with more
//                      samples than what is stored), read when a measurement is opened.
//                      Kept apart from `sessions` so the history list stays light.
//   LocalSignalStore   the same, in localStorage, for "Red local" / "Demo".
//
// A signal in memory is { fs, n, values }: `values` in time order, null where the
// recording has a gap, `n` the number of real samples. On the wire (and in
// localStorage) `values` is one CSV string with an empty field for each gap.
//
// The environment-dependent pieces (fetch, clock, storage) can be injected so the
// logic runs under `node --test` (tests/signal.test.js).

const SIGNAL_MAX_SAMPLES = 6000; // the cap of `n` in the rules: 120 s at 50 Hz
const SIGNAL_MIN_SECONDS = 5; // a shorter recording is not worth keeping
const SIGNAL_SID_PATTERN = /^[A-Za-z0-9_-]{1,40}$/;
const SIGNAL_NONE_TTL_MS = 15 * 1000; // "nothing stored yet" is asked again after this long
const SIGNAL_SAVE_RETRY_MS = [1500, 4000]; // waits between the (up to 3) attempts to save
const SIGNAL_LOCAL_PREFIX = "pulsox.ppg.v1.";
const SIGNAL_LOCAL_INDEX = "pulsox.ppg.v1.index";
const SIGNAL_LOCAL_MAX = 20; // signals kept in localStorage (~15 kB each)

// ----------------------------------------------------------------- encoding

/** [112, null, -5] -> "112,,-5" */
function signalEncode(values) {
  return values.map((v) => (typeof v === "number" && Number.isFinite(v) ? String(Math.round(v * 100) / 100) : "")).join(",");
}

/** "112,,-5" -> [112, null, -5] */
function signalDecode(csv) {
  if (typeof csv !== "string" || csv === "") return [];
  return csv.split(",").map((s) => {
    if (s.trim() === "") return null;
    const v = Number(s);
    return Number.isFinite(v) ? v : null;
  });
}

function signalCount(values) {
  let n = 0;
  for (const v of values) if (v !== null) n++;
  return n;
}

function signalToWire(signal) {
  return { fs: signal.fs, n: signal.n, data: signalEncode(signal.values) };
}

/** The stored { fs, n, data } -> a signal, or null if it is not usable. */
function signalFromWire(wire) {
  if (!wire || typeof wire !== "object" || typeof wire.data !== "string") return null;
  if (typeof wire.fs !== "number" || !(wire.fs > 0)) return null;
  const values = signalDecode(wire.data);
  const n = signalCount(values);
  return n >= 2 ? { fs: wire.fs, n, values } : null;
}

// ----------------------------------------------------------------- recorder

class SignalRecorder {
  constructor({ maxSamples = SIGNAL_MAX_SAMPLES } = {}) {
    this._max = maxSamples;
    this.begin(null);
  }

  /** Starts recording the measurement `sessionId` (drops whatever was recorded). */
  begin(sessionId) {
    this._sid = sessionId;
    this._values = [];
    this._count = 0;
    this._fs = 0;
    this._lastSeq = null;
  }

  get sessionId() {
    return this._sid;
  }

  /** Real samples recorded so far. */
  get count() {
    return this._count;
  }

  /**
   * @param {number[]} samples  one batch, in time order
   * @param {number} fs  sample rate in Hz
   * @param {number|null} seq  cloud transport: the 1-based second this batch belongs to. The
   *   seconds missing before it (a tab in the background, joining late) become a gap. Null
   *   when batches carry no number (WebSocket, demo): they are appended as they come.
   */
  push(samples, fs, seq = null) {
    if (this._sid === null || !Array.isArray(samples) || samples.length === 0 || !(fs > 0)) return;
    if (this._fs && fs !== this._fs) return; // a rate change in the middle of a measurement: keep the first
    if (typeof seq === "number" && Number.isFinite(seq)) {
      const expected = this._lastSeq === null ? 1 : this._lastSeq + 1;
      if (seq < expected) return; // the same second again
      this._lastSeq = seq;
      this._pad((seq - expected) * fs);
    }
    this._fs = fs;
    for (const v of samples) {
      if (this._values.length >= this._max) return;
      if (typeof v !== "number" || !Number.isFinite(v)) continue;
      this._values.push(v);
      this._count++;
    }
  }

  /** The recording as a signal, or null if it is too short to keep. */
  snapshot() {
    if (!this._fs || this._count < this._fs * SIGNAL_MIN_SECONDS) return null;
    return { fs: this._fs, n: this._count, values: this._values.slice() };
  }

  _pad(gap) {
    const room = this._max - this._values.length;
    for (let i = Math.min(gap, room); i > 0; i--) this._values.push(null);
  }
}

// -------------------------------------------------------------- local store

class LocalSignalStore {
  /** @param {{storage?: Storage, max?: number}} [opts]  `storage` is a test seam */
  constructor({ storage = null, max = SIGNAL_LOCAL_MAX } = {}) {
    this._storage = storage;
    this._max = max;
  }

  get _s() {
    if (this._storage) return this._storage;
    try {
      return typeof localStorage === "undefined" ? null : localStorage;
    } catch {
      return null;
    }
  }

  async save(sid, signal) {
    const s = this._s;
    if (!s || !signal) return "failed";
    const body = JSON.stringify(signalToWire(signal));
    const index = this._index(s).filter((id) => id !== sid);
    // The oldest signals make room when the browser's quota says no...
    for (;;) {
      try {
        s.setItem(SIGNAL_LOCAL_PREFIX + sid, body);
        break;
      } catch (err) {
        if (index.length === 0) {
          console.warn("[signal] could not store the signal locally", err);
          return "failed";
        }
        s.removeItem(SIGNAL_LOCAL_PREFIX + index.shift());
      }
    }
    // ...and when there are more than `max`.
    index.push(sid);
    while (index.length > this._max) s.removeItem(SIGNAL_LOCAL_PREFIX + index.shift());
    try {
      s.setItem(SIGNAL_LOCAL_INDEX, JSON.stringify(index));
    } catch (err) {
      console.warn("[signal] could not store the signal locally", err);
      s.removeItem(SIGNAL_LOCAL_PREFIX + sid);
      return "failed";
    }
    return "saved";
  }

  async load(sid) {
    const s = this._s;
    if (!s) return { status: "none" };
    try {
      const raw = s.getItem(SIGNAL_LOCAL_PREFIX + sid);
      const signal = raw ? signalFromWire(JSON.parse(raw)) : null;
      return signal ? { status: "ok", signal } : { status: "none" };
    } catch (err) {
      console.warn("[signal] could not read the stored signal", err);
      return { status: "none" };
    }
  }

  clear() {
    const s = this._s;
    if (!s) return;
    try {
      for (const id of this._index(s)) s.removeItem(SIGNAL_LOCAL_PREFIX + id);
      s.removeItem(SIGNAL_LOCAL_INDEX);
    } catch (err) {
      console.warn("[signal] could not clear the stored signals", err);
    }
  }

  _index(s) {
    try {
      const list = JSON.parse(s.getItem(SIGNAL_LOCAL_INDEX) || "[]");
      return Array.isArray(list) ? list.filter((id) => typeof id === "string") : [];
    } catch {
      return [];
    }
  }
}

// -------------------------------------------------------------- cloud store

class CloudSignalStore {
  /**
   * @param {object} opts
   * @param {{deviceUrl: string}} opts.config  from resolveCloudConfig()
   * @param {object} [opts.env]  test seams: fetch, now, sleep
   */
  constructor({ config, env = {} }) {
    this._cfg = config;
    this._env = env;
    this._cache = new Map(); // sid -> { result, at }
    this._inflight = new Map(); // sid -> Promise
  }

  /**
   * Saves the recording of a finished measurement. Resolves to:
   *   "saved"    stored (and kept in memory, so opening it needs no download)
   *   "skipped"  the device did not keep that measurement (discarded, or never closed)
   *   "exists"   someone watching the whole measurement already saved a signal at least as long
   *   "denied"   the rules said no (the few minutes to save are over, or a longer one arrived)
   *   "failed"   the network did not cooperate after 3 attempts
   */
  async save(sid, signal) {
    if (!SIGNAL_SID_PATTERN.test(sid) || !signal || !(signal.n >= 1)) return "skipped";
    try {
      // Only a measurement the device closed and kept has an `endedAt` (a discarded one is deleted).
      const endedAt = await this._get(`sessions/${sid}/endedAt`);
      if (typeof endedAt !== "number") return "skipped";
      const stored = await this._get(`ppg/${sid}/n`);
      if (typeof stored === "number" && stored >= signal.n) return "exists";
    } catch (err) {
      console.warn("[signal] could not check the measurement before saving", err); // the write is tried anyway
    }

    const body = JSON.stringify(signalToWire(signal));
    for (let attempt = 0; attempt <= SIGNAL_SAVE_RETRY_MS.length; attempt++) {
      try {
        const res = await this._fetch(this._url(`ppg/${sid}`, "?print=silent"), { method: "PUT", body });
        if (res.ok) {
          this._cache.set(sid, { result: { status: "ok", signal }, at: this._now() });
          return "saved";
        }
        if (res.status === 401 || res.status === 403) return "denied"; // a rule, not the network: retrying will not help
      } catch (err) {
        console.warn("[signal] could not save the signal", err);
      }
      if (attempt < SIGNAL_SAVE_RETRY_MS.length) await this._sleep(SIGNAL_SAVE_RETRY_MS[attempt]);
    }
    return "failed";
  }

  /** @returns {Promise<{status: "ok", signal: object} | {status: "none"} | {status: "error"}>} */
  load(sid) {
    if (!SIGNAL_SID_PATTERN.test(sid)) return Promise.resolve({ status: "none" });
    const hit = this._cache.get(sid);
    if (hit && (hit.result.status === "ok" || this._now() - hit.at < SIGNAL_NONE_TTL_MS)) return Promise.resolve(hit.result);
    let pending = this._inflight.get(sid);
    if (!pending) {
      pending = this._load(sid).finally(() => this._inflight.delete(sid));
      this._inflight.set(sid, pending);
    }
    return pending;
  }

  clear() {
    this._cache.clear();
  }

  async _load(sid) {
    try {
      const signal = signalFromWire(await this._get(`ppg/${sid}`));
      const result = signal ? { status: "ok", signal } : { status: "none" };
      this._cache.set(sid, { result, at: this._now() });
      return result;
    } catch (err) {
      console.warn("[signal] could not read the signal", err);
      return { status: "error" };
    }
  }

  async _get(path) {
    const res = await this._fetch(this._url(path), { cache: "no-store" });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return res.json();
  }

  _url(path, query = "") {
    return `${this._cfg.deviceUrl}/${path}.json${query}`;
  }

  _fetch(url, opts) {
    return this._env.fetch ? this._env.fetch(url, opts) : fetch(url, opts);
  }

  _now() {
    return this._env.now ? this._env.now() : Date.now();
  }

  _sleep(ms) {
    return this._env.sleep ? this._env.sleep(ms) : new Promise((resolve) => setTimeout(resolve, ms));
  }
}
