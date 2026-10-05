// Persisted reading log (the "Historial" view), grouped by measurement. A
// measurement ("session") opens when the device sends its start packet and
// closes when it ends (or the connection drops). Readings are separate from
// the raw per-second buffer that feeds the trend charts (see charts.js):
// logging every 1Hz tick here would make the list useless noise, so within a
// session an entry is logged for the first valid reading, for every status
// change, and otherwise every SESSION_SAMPLE_INTERVAL_MS.

const HISTORY_STORAGE_KEY = "pulsox.history.v2";
const HISTORY_LEGACY_KEY = "pulsox.history.v1"; // flat list, pre-sessions
const HISTORY_MAX_ENTRIES = 400;
const HISTORY_MAX_SESSIONS = 60;
const SESSION_SAMPLE_INTERVAL_MS = 20 * 1000;
const LEGACY_SESSION_ID = "legacy";

const STATUS_RANK = { unknown: 0, good: 1, warning: 2, critical: 3 };

function worseOf(a, b) {
  return STATUS_RANK[a] >= STATUS_RANK[b] ? a : b;
}

class HistoryStore {
  constructor(storageKey = HISTORY_STORAGE_KEY) {
    this._storageKey = storageKey;
    // sessions: [{ id, startedAt, endedAt|null, implicit?, legacy? }]  (any order)
    // entries:  [{ ts, sessionId, spo2, spo2Status, bpm, bpmStatus, overallStatus }]  (newest first)
    const loaded = this._load();
    this._sessions = loaded.sessions;
    this._entries = loaded.entries;
    this._activeId = null;
  }

  get activeSessionId() {
    return this._activeId;
  }

  /** Number of measurements that have at least one logged reading. */
  get sessionCount() {
    const withEntries = new Set(this._entries.map((e) => e.sessionId));
    return this._sessions.filter((s) => withEntries.has(s.id)).length;
  }

  startSession({ id, startedAt, implicit = false }) {
    if (this._activeId) this.endSession(Date.now());
    this._sessions.push({ id, startedAt, endedAt: null, implicit });
    this._activeId = id;
    this._save();
  }

  endSession(endedAt) {
    const session = this._sessions.find((s) => s.id === this._activeId);
    if (session) session.endedAt = endedAt;
    this._activeId = null;
    this._save();
  }

  /** @returns {object|null} the newly logged entry, or null if this tick was throttled/skipped */
  addReading({ ts, sessionId, spo2, spo2Status, bpm, bpmStatus }) {
    if (spo2Status === "unknown" && bpmStatus === "unknown") return null;
    if (!this._sessions.some((s) => s.id === sessionId)) return null;

    const overallStatus = worseOf(spo2Status, bpmStatus);
    const last = this._entries.find((e) => e.sessionId === sessionId);
    const shouldLog = !last || last.overallStatus !== overallStatus || ts - last.ts >= SESSION_SAMPLE_INTERVAL_MS;
    if (!shouldLog) return null;

    const entry = { ts, sessionId, spo2, spo2Status, bpm, bpmStatus, overallStatus };
    this._entries.unshift(entry);
    if (this._entries.length > HISTORY_MAX_ENTRIES) this._entries.length = HISTORY_MAX_ENTRIES;
    this._save();
    return entry;
  }

  /** Wipes the log. A measurement in progress is kept (empty) so its next
   * readings still have somewhere to go. */
  clear() {
    const active = this._sessions.find((s) => s.id === this._activeId);
    this._sessions = active ? [active] : [];
    this._entries = [];
    this._save();
  }

  /**
   * Measurements newest first, each with its readings newest first. Groups
   * whose readings are all filtered out (or that have none) are omitted.
   * @param {"all"|"warning"|"critical"} filter
   */
  groups(filter = "all") {
    const sessions = [...this._sessions].sort((a, b) => b.startedAt - a.startedAt);
    const groups = [];
    for (const session of sessions) {
      const rows = this._entries.filter(
        (e) => e.sessionId === session.id && (filter === "all" || e.overallStatus === filter)
      );
      if (rows.length === 0) continue;
      groups.push({ session, active: session.id === this._activeId, rows });
    }
    return groups;
  }

  _load() {
    try {
      const raw = localStorage.getItem(this._storageKey);
      if (raw) {
        const parsed = JSON.parse(raw);
        if (parsed && Array.isArray(parsed.sessions) && Array.isArray(parsed.entries)) return parsed;
      }
      return this._migrateLegacy();
    } catch (err) {
      console.warn("[history] failed to load from localStorage", err);
      return { sessions: [], entries: [] };
    }
  }

  /** v1 stored a flat list with no measurement info: keep it under a single
   * "previous readings" group instead of dropping it. */
  _migrateLegacy() {
    try {
      const raw = localStorage.getItem(HISTORY_LEGACY_KEY);
      if (!raw) return { sessions: [], entries: [] };
      const old = JSON.parse(raw);
      if (!Array.isArray(old) || old.length === 0) return { sessions: [], entries: [] };
      const entries = old.map((e) => ({ ...e, sessionId: LEGACY_SESSION_ID }));
      const times = entries.map((e) => e.ts);
      return {
        sessions: [{ id: LEGACY_SESSION_ID, startedAt: Math.min(...times), endedAt: Math.max(...times), legacy: true }],
        entries,
      };
    } catch (err) {
      console.warn("[history] failed to migrate legacy history", err);
      return { sessions: [], entries: [] };
    }
  }

  _save() {
    // Drop finished measurements that never logged a reading, and cap the count.
    const withEntries = new Set(this._entries.map((e) => e.sessionId));
    this._sessions = this._sessions.filter((s) => s.id === this._activeId || withEntries.has(s.id));
    if (this._sessions.length > HISTORY_MAX_SESSIONS) {
      this._sessions.sort((a, b) => b.startedAt - a.startedAt);
      const keep = new Set(this._sessions.slice(0, HISTORY_MAX_SESSIONS).map((s) => s.id));
      this._sessions = this._sessions.filter((s) => keep.has(s.id) || s.id === this._activeId);
      this._entries = this._entries.filter((e) => keep.has(e.sessionId) || e.sessionId === this._activeId);
    }
    try {
      localStorage.setItem(this._storageKey, JSON.stringify({ sessions: this._sessions, entries: this._entries }));
    } catch (err) {
      console.warn("[history] failed to persist to localStorage", err);
    }
  }
}
