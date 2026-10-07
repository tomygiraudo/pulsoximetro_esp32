// "Nube" data source. Instead of talking to the ESP32 over the local network,
// both sides are clients of a Firebase Realtime Database (see PROTOCOL.md,
// "Transporte en la nube"): the device WRITES (REST over HTTPS, with login),
// this page only READS (public read rules):
//
//   CloudSource   streams  devices/<id>/live.json  with EventSource and turns
//                 the database changes into the SAME frames the WebSocket
//                 transport delivers (hello / measurement_start / telemetry /
//                 ppg / measurement_end), so app.js does not know the
//                 difference. Also reads info.json once for `hello`.
//   CloudHistory  same interface as HistoryStore, filled from
//                 devices/<id>/sessions.json (a shared history instead of one
//                 browser's localStorage). Read-only: the device owns the data.
//
// No SDK, no build step: plain fetch() + EventSource. The streaming protocol
// (https://firebase.google.com/docs/reference/rest/database#section-streaming)
// sends `put` / `patch` events whose payload is {path, data}, plus `keep-alive`
// (every ~30 s), `cancel` and `auth_revoked`; a 307 redirect may precede the
// stream, which EventSource follows on its own.
//
// The environment-dependent pieces (EventSource, fetch, timers, document) can
// be injected through `env` so the logic runs under `node --test` (see
// tests/cloud.test.js). Depends on protocol.js and history.js (loaded first).

const CLOUD_STALE_MS = 10 * 1000; // no new `seq` this long -> the device is gone
const CLOUD_SNAPSHOT_GRACE_MS = 3000; // a `live` found on connect must change within this to be believed
const CLOUD_SILENCE_MS = 65 * 1000; // no event at all (not even keep-alive) -> reconnect
const CLOUD_RECONNECT_BASE_MS = 1000;
const CLOUD_RECONNECT_MAX_MS = 15000;
const CLOUD_HISTORY_SESSIONS = 30;
const CLOUD_DEFAULT_PPG_FS = 50;
const CLOUD_ES_CLOSED = 2; // EventSource.CLOSED

// ------------------------------------------------------------- configuration

/** Validates window.PULSOX_CLOUD. Returns { deviceUrl, deviceId } or null when
 * the cloud source is not configured (the "Nube" option is then not offered). */
function resolveCloudConfig(cfg) {
  if (!cfg || typeof cfg.databaseURL !== "string" || typeof cfg.deviceId !== "string") return null;
  const base = cfg.databaseURL.trim().replace(/\/+$/, "");
  const deviceId = cfg.deviceId.trim();
  const secure = /^https:\/\/[a-z0-9-]+(\.[a-z0-9-]+)+$/i.test(base);
  const localMock = /^http:\/\/(localhost|127\.0\.0\.1)(:\d{1,5})?$/i.test(base); // tools/cloud/mock_rtdb.js
  if (!(secure || localMock) || !/^[A-Za-z0-9_-]{1,64}$/.test(deviceId)) return null;
  return { deviceUrl: `${base}/devices/${deviceId}`, deviceId };
}

// ---------------------------------------------------- stream event handling

function cloudIsObject(v) {
  return v !== null && typeof v === "object" && !Array.isArray(v);
}

/** Replaces the value at `path` (null/undefined deletes it). Returns the new
 * root. Mirrors the database: it has no empty objects, so a delete that
 * empties a parent removes the parent too. Mutates `tree` in place. */
function cloudSetPath(tree, path, value) {
  const keys = String(path).split("/").filter(Boolean);
  if (keys.includes("__proto__")) return tree;
  if (keys.length === 0) return value === undefined ? null : value;

  const root = cloudIsObject(tree) ? tree : {};
  const trail = [root]; // nodes from the root down to the parent of the target
  let node = root;
  for (let i = 0; i < keys.length - 1; i++) {
    if (!cloudIsObject(node[keys[i]])) {
      if (value === null || value === undefined) return Object.keys(root).length ? root : null; // nothing to delete
      node[keys[i]] = {};
    }
    node = node[keys[i]];
    trail.push(node);
  }
  const last = keys[keys.length - 1];
  if (value === null || value === undefined) delete node[last];
  else node[last] = value;

  for (let i = trail.length - 1; i > 0 && Object.keys(trail[i]).length === 0; i--) delete trail[i - 1][keys[i - 1]];
  return Object.keys(root).length ? root : null;
}

/** Applies one streaming event. `put`: replace the data at `path`. `patch`:
 * for each key of `data`, replace the child `path/key` (the key may itself be
 * a multi-level path). */
function cloudApplyEvent(tree, kind, path, data) {
  if (kind === "put") return cloudSetPath(tree, path, data);
  let next = tree;
  if (cloudIsObject(data)) {
    for (const [key, value] of Object.entries(data)) next = cloudSetPath(next, `${path}/${key}`, value);
  }
  return next;
}

/** "112,340,-5" -> [112, 340, -5] (the `ppg` leaf is a CSV string). */
function parsePpgCsv(csv) {
  if (typeof csv !== "string" || csv === "") return [];
  return csv
    .split(",")
    .map((s) => (s.trim() === "" ? NaN : Number(s)))
    .filter((v) => Number.isFinite(v));
}

// ------------------------------------------------------------- CloudSource

class CloudSource {
  /**
   * @param {object} opts
   * @param {{deviceUrl: string, deviceId: string}} opts.config  from resolveCloudConfig()
   * @param {(msg: object) => void} opts.onFrame
   * @param {(state: "connecting"|"live"|"reconnecting"|"offline") => void} opts.onState
   * @param {object} [opts.env]  test seams: EventSource, fetch, now, setTimeout, clearTimeout, document
   */
  constructor({ config, onFrame, onState, env = {} }) {
    this._cfg = config;
    this._onFrame = onFrame;
    this._onState = onState;
    this._env = env;

    this._stopped = true;
    this._es = null;
    this._attempt = 0;
    this._reconnectTimer = null;
    this._silenceTimer = null;
    this._staleTimer = null;

    this._live = null; // local copy of devices/<id>/live, kept in sync by the stream events
    this._sid = null; // session_id of the open measurement (null = none)
    this._lastSeq = null;
    this._dead = null; // {sid, seq} of a measurement closed for silence: ignore that snapshot until it changes
    this._snapshot = false; // the next event is the initial value a stream delivers on connect
    this._candidate = null; // {sid, seq} of a `live` seen in the snapshot, not believed yet
    this._candidateTimer = null;

    this._onVisibility = () => {
      if (this._stopped || this._doc?.visibilityState !== "visible") return;
      // Mobile browsers (iOS above all) suspend or kill background streams:
      // resync on return from a clean connection, without flickering the badge.
      this._open({ silent: true });
      this._fetchInfo();
    };
  }

  start() {
    if (!this._stopped) return;
    this._stopped = false;
    this._attempt = 0;
    if (!this._EventSource) {
      console.warn("[cloud] EventSource is not available in this browser");
      this._onState("offline");
      return;
    }
    this._doc?.addEventListener("visibilitychange", this._onVisibility);
    this._open();
    this._fetchInfo();
  }

  /** Closes everything. Emits nothing: the caller reports the new state. */
  stop() {
    this._stopped = true;
    this._doc?.removeEventListener("visibilitychange", this._onVisibility);
    this._closeStream();
    this._clear("_reconnectTimer");
    this._clear("_silenceTimer");
    this._resetSession();
    this._live = null;
    this._dead = null;
  }

  // ---- environment (resolved lazily so tests can inject fakes)

  get _EventSource() {
    if (this._env.EventSource !== undefined) return this._env.EventSource;
    return typeof EventSource !== "undefined" ? EventSource : null;
  }

  get _doc() {
    if (this._env.document !== undefined) return this._env.document;
    return typeof document !== "undefined" ? document : null;
  }

  _now() {
    return this._env.now ? this._env.now() : Date.now();
  }

  _setTimeout(fn, ms) {
    return this._env.setTimeout ? this._env.setTimeout(fn, ms) : setTimeout(fn, ms);
  }

  _clear(timerField) {
    if (this[timerField] === null) return;
    if (this._env.clearTimeout) this._env.clearTimeout(this[timerField]);
    else clearTimeout(this[timerField]);
    this[timerField] = null;
  }

  // ---- stream lifecycle

  _open({ silent = false } = {}) {
    this._clear("_reconnectTimer");
    this._closeStream();
    this._dropCandidate();
    if (!silent) this._onState(this._attempt > 0 ? "reconnecting" : "connecting");

    let es;
    try {
      es = new this._EventSource(`${this._cfg.deviceUrl}/live.json`);
    } catch (err) {
      console.warn("[cloud] could not open the stream", err);
      this._onState("offline");
      return;
    }
    this._es = es;
    this._touch();

    const current = () => this._es === es && !this._stopped;
    es.addEventListener("open", () => {
      if (!current()) return;
      this._attempt = 0;
      this._snapshot = true; // every (re)connection starts with the current value
      this._dropCandidate();
      this._onState("live");
    });
    es.addEventListener("put", (e) => current() && this._onData(e, "put"));
    es.addEventListener("patch", (e) => current() && this._onData(e, "patch"));
    es.addEventListener("keep-alive", () => current() && this._touch());
    // The server dropped us (rules changed, token revoked): start over.
    es.addEventListener("cancel", () => current() && this._failStream());
    es.addEventListener("auth_revoked", () => current() && this._failStream());
    es.addEventListener("error", () => {
      if (!current()) return;
      this._resetSession(); // the app closes its measurement on the state change below
      if (es.readyState === (this._EventSource.CLOSED ?? CLOUD_ES_CLOSED)) this._failStream();
      else this._onState("reconnecting"); // still CONNECTING: the browser retries by itself
    });
  }

  _closeStream() {
    if (!this._es) return;
    const es = this._es;
    this._es = null;
    es.close();
  }

  /** The stream is gone for good (HTTP error, cancel): retry with capped backoff. */
  _failStream() {
    this._resetSession();
    this._closeStream();
    this._clear("_silenceTimer");
    this._onState("reconnecting");
    const delay = Math.min(CLOUD_RECONNECT_MAX_MS, CLOUD_RECONNECT_BASE_MS * 2 ** this._attempt);
    this._attempt += 1;
    this._clear("_reconnectTimer");
    this._reconnectTimer = this._setTimeout(() => {
      this._reconnectTimer = null;
      if (!this._stopped) this._open();
    }, delay);
  }

  /** Any traffic (data or keep-alive) proves the connection is alive. */
  _touch() {
    this._clear("_silenceTimer");
    this._silenceTimer = this._setTimeout(() => {
      this._silenceTimer = null;
      if (!this._stopped) this._failStream(); // half-open connection that never errored
    }, CLOUD_SILENCE_MS);
  }

  // ---- database -> frames

  _onData(event, kind) {
    this._touch();
    let msg;
    try {
      msg = JSON.parse(event.data);
    } catch (err) {
      console.warn("[cloud] stream event is not valid JSON, dropping", err);
      return;
    }
    if (!msg || typeof msg.path !== "string") return;
    this._live = cloudApplyEvent(this._live, kind, msg.path, msg.data);
    this._sync();
    this._snapshot = false;
  }

  /** Compares the local `live` copy with what was already announced and emits
   * the frames the change implies. */
  _sync() {
    const live = this._live;
    if (!cloudIsObject(live) || typeof live.session_id !== "string") {
      this._dead = null;
      this._dropCandidate();
      if (this._sid !== null) this._endMeasurement("ended");
      return;
    }
    if (this._dead && this._dead.sid === live.session_id && this._dead.seq === live.seq) return; // same stale snapshot
    this._dead = null;

    const isNewSession = live.session_id !== this._sid;
    if (!isNewSession && live.seq === this._lastSeq) return; // a partial update, nothing new

    if (isNewSession && !this._confirmed(live)) return;

    if (isNewSession) {
      this._dropCandidate();
      this._sid = live.session_id;
      // `elapsed_ms` is the device's own stopwatch, so a viewer that joins mid-
      // measurement gets the right start without trusting its clock against the server's.
      const elapsed = typeof live.elapsed_ms === "number" && live.elapsed_ms >= 0 ? live.elapsed_ms : 0;
      this._onFrame({ type: "measurement_start", session_id: live.session_id, started_at: this._now() - elapsed });
    }
    this._lastSeq = live.seq;
    this._armStale();

    this._onFrame(this._telemetryFrame(live));
    const samples = parsePpgCsv(live.ppg);
    if (samples.length) {
      const fs = typeof live.fs === "number" && live.fs > 0 ? live.fs : CLOUD_DEFAULT_PPG_FS;
      this._onFrame({ type: "ppg", fs, samples });
    }
  }

  /** A `live` node that is already there when the stream connects may be the
   * leftover of a device that lost power mid-measurement (nobody deletes it).
   * Believe it only once it changes: wait for one more write (~1 s apart on a
   * live device); if none comes, it is a ghost and stays ignored. A `live` that
   * appears on an already-open stream is a measurement starting: no wait. */
  _confirmed(live) {
    if (this._snapshot) {
      this._candidate = { sid: live.session_id, seq: live.seq };
      this._clear("_candidateTimer");
      this._candidateTimer = this._setTimeout(() => {
        this._candidateTimer = null;
        if (!this._candidate) return;
        this._dead = this._candidate;
        this._candidate = null;
      }, CLOUD_SNAPSHOT_GRACE_MS);
      return false;
    }
    if (this._candidate && this._candidate.sid === live.session_id) {
      if (live.seq === this._candidate.seq) return false; // nothing new yet
      this._dropCandidate();
    }
    return true;
  }

  _dropCandidate() {
    this._clear("_candidateTimer");
    this._candidate = null;
  }

  _telemetryFrame(live) {
    const num = (v) => (typeof v === "number" && Number.isFinite(v) ? v : null);
    const frame = {
      type: "telemetry",
      seq: live.seq,
      finger_detected: live.finger_detected === true,
      spo2: num(live.spo2),
      spo2_valid: live.spo2_valid === true,
      bpm: num(live.bpm),
      bpm_valid: live.bpm_valid === true,
      signal_quality: num(live.signal_quality) ?? 0,
    };
    if (num(live.battery_pct) !== null) frame.battery_pct = live.battery_pct; // omitted when unknown (PROTOCOL.md)
    return frame;
  }

  /** The measurement is open but no new `seq` shows up: the device died or
   * lost WiFi mid-measurement. Close it instead of leaving it "live". */
  _armStale() {
    this._clear("_staleTimer");
    this._staleTimer = this._setTimeout(() => {
      this._staleTimer = null;
      if (this._sid === null) return;
      this._dead = { sid: this._sid, seq: this._lastSeq };
      this._endMeasurement("timeout");
    }, CLOUD_STALE_MS);
  }

  _endMeasurement(reason) {
    this._resetSession();
    this._onFrame({ type: "measurement_end", reason });
    this._fetchInfo(); // the device updates `info` (battery) when it finishes
  }

  /** Forgets the open measurement without announcing it. */
  _resetSession() {
    this._clear("_staleTimer");
    this._dropCandidate();
    this._sid = null;
    this._lastSeq = null;
  }

  // ---- device info (-> `hello`)

  async _fetchInfo() {
    try {
      const res = await this._fetchFn(`${this._cfg.deviceUrl}/info.json`, { cache: "no-store" });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const info = await res.json();
      if (this._stopped || !cloudIsObject(info)) return;
      const frame = {
        type: "hello",
        device_id: typeof info.device_id === "string" ? info.device_id : this._cfg.deviceId,
        fw_version: typeof info.fw_version === "string" ? info.fw_version : "—",
        sensor: typeof info.sensor === "string" ? info.sensor : "—",
        sample_rate_hz: 1,
      };
      if (typeof info.battery_pct === "number") frame.battery_pct = info.battery_pct; // battery while idle
      this._onFrame(frame);
    } catch (err) {
      console.warn("[cloud] could not read the device info", err);
    }
  }

  _fetchFn(url, opts) {
    return this._env.fetch ? this._env.fetch(url, opts) : fetch(url, opts);
  }
}

// ------------------------------------------------------------ CloudHistory

/** Readings of ONE measurement (any order) -> the entries the Historial lists
 * (newest first), thinned exactly like HistoryStore does at write time: the
 * first valid reading, every status change, and otherwise every
 * SESSION_SAMPLE_INTERVAL_MS. The status is computed here with the viewer's
 * own thresholds; the database only holds raw values. */
function cloudBuildEntries(sessionId, readings, thresholds) {
  const sorted = readings.filter((r) => r && typeof r.ts === "number").sort((a, b) => a.ts - b.ts);
  const entries = [];
  let last = null;
  for (const r of sorted) {
    const spo2 = typeof r.spo2 === "number" ? r.spo2 : null;
    const bpm = typeof r.bpm === "number" ? r.bpm : null;
    const spo2Status = classifySpo2(spo2, thresholds);
    const bpmStatus = classifyBpm(bpm, thresholds);
    if (spo2Status === "unknown" && bpmStatus === "unknown") continue;
    const overallStatus = worseOf(spo2Status, bpmStatus);
    if (last && last.overallStatus === overallStatus && r.ts - last.ts < SESSION_SAMPLE_INTERVAL_MS) continue;
    last = { ts: r.ts, sessionId, spo2, spo2Status, bpm, bpmStatus, overallStatus };
    entries.push(last);
  }
  return entries.reverse();
}

class CloudHistory {
  /**
   * @param {object} opts
   * @param {{deviceUrl: string}} opts.config  from resolveCloudConfig()
   * @param {() => object} opts.getThresholds  the viewer's current thresholds
   * @param {() => void} [opts.onChange]  called whenever new data arrives (re-render)
   * @param {object} [opts.env]  test seams: fetch
   */
  constructor({ config, getThresholds, onChange = () => {}, env = {} }) {
    this._cfg = config;
    this._getThresholds = getThresholds;
    this._onChange = onChange;
    this._env = env;
    this._raw = null; // last sessions.json payload
    this._version = 0;
    this._derived = null; // cache: { key, sessions, bySession }
    this._activeId = null;
    this._status = "idle"; // "idle" | "loading" | "ready" | "error"
    this._inflight = null;
    this._again = false;
  }

  get activeSessionId() {
    return this._activeId;
  }

  get status() {
    return this._status;
  }

  get sessionCount() {
    const { sessions, bySession } = this._derive();
    return sessions.filter((s) => bySession.get(s.id).length > 0).length;
  }

  // Mutators: the device owns the data, so these only track the open
  // measurement (to flag it "En curso") and re-read when it ends.
  startSession({ id }) {
    this._activeId = id;
  }

  endSession() {
    this._activeId = null;
    this.refresh();
  }

  addReading() {
    return null;
  }

  clear() {}

  /** Same shape as HistoryStore.groups(): measurements newest first, each with
   * its readings newest first; groups left empty by the filter are omitted. */
  groups(filter = "all") {
    const { sessions, bySession } = this._derive();
    const groups = [];
    for (const s of sessions) {
      const all = bySession.get(s.id);
      const rows = all.filter((e) => filter === "all" || e.overallStatus === filter);
      if (rows.length === 0) continue;
      const active = s.id === this._activeId;
      // A measurement the device never closed (power cut) reads as ended at its last reading.
      const endedAt = s.endedAt ?? (active ? null : s.lastReadingTs);
      groups.push({ session: { id: s.id, startedAt: s.startedAt, endedAt }, active, rows });
    }
    return groups;
  }

  /** Re-reads the last measurements. Overlapping calls coalesce into one extra read. */
  refresh() {
    if (this._inflight) {
      this._again = true;
      return this._inflight;
    }
    if (!this._raw) this._status = "loading";
    this._inflight = this._load().finally(() => {
      this._inflight = null;
      if (this._again) {
        this._again = false;
        this.refresh();
      }
    });
    return this._inflight;
  }

  async _load() {
    // orderBy on a child key needs ".indexOn": ["startedAt"] in the rules (tools/cloud/database.rules.json)
    const url = `${this._cfg.deviceUrl}/sessions.json?orderBy=%22startedAt%22&limitToLast=${CLOUD_HISTORY_SESSIONS}`;
    try {
      const res = await (this._env.fetch ? this._env.fetch(url, { cache: "no-store" }) : fetch(url, { cache: "no-store" }));
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const data = await res.json();
      this._raw = cloudIsObject(data) ? data : {};
      this._version += 1;
      this._status = "ready";
    } catch (err) {
      console.warn("[cloud] could not read the history", err);
      if (!this._raw) this._status = "error";
    }
    this._onChange();
  }

  /** Sessions newest first + their thinned entries, recomputed when the data
   * or the thresholds change. */
  _derive() {
    const thresholds = this._getThresholds();
    const key = `${this._version}|${JSON.stringify(thresholds)}`;
    if (this._derived && this._derived.key === key) return this._derived;

    const sessions = [];
    const bySession = new Map();
    for (const [id, s] of Object.entries(this._raw || {})) {
      if (!cloudIsObject(s) || typeof s.startedAt !== "number") continue;
      const readings = cloudIsObject(s.readings) ? Object.values(s.readings) : [];
      const times = readings.map((r) => (r && typeof r.ts === "number" ? r.ts : -Infinity));
      const lastReadingTs = Math.max(s.startedAt, ...times);
      sessions.push({
        id,
        startedAt: s.startedAt,
        endedAt: typeof s.endedAt === "number" ? s.endedAt : null,
        lastReadingTs,
      });
      bySession.set(id, cloudBuildEntries(id, readings, thresholds));
    }
    sessions.sort((a, b) => b.startedAt - a.startedAt);
    this._derived = { key, sessions, bySession };
    return this._derived;
  }
}
