// Persisted reading log (the "Historial" list), separate from the raw
// per-second buffer that feeds the charts (see charts.js). Logging every
// 1Hz telemetry tick here would make the list useless noise, so entries are
// throttled: always log a status transition, log every ~60s while a
// caution/danger episode is sustained (so the log shows its extent, not just
// its start), and otherwise log a "still normal" snapshot every ~5 minutes.

const HISTORY_STORAGE_KEY = "pulsox.history.v1";
const HISTORY_MAX_ENTRIES = 300;
const SUSTAINED_EVENT_INTERVAL_MS = 60 * 1000;
const HEARTBEAT_INTERVAL_MS = 5 * 60 * 1000;

const STATUS_RANK = { unknown: 0, good: 1, warning: 2, critical: 3 };

function worseOf(a, b) {
  return STATUS_RANK[a] >= STATUS_RANK[b] ? a : b;
}

class HistoryStore {
  constructor(storageKey = HISTORY_STORAGE_KEY) {
    this._storageKey = storageKey;
    this._entries = this._load();
  }

  get entries() {
    return this._entries;
  }

  /** @returns {object|null} the newly logged entry, or null if this tick was throttled/skipped */
  addReading({ ts, spo2, spo2Status, bpm, bpmStatus }) {
    if (spo2Status === "unknown" && bpmStatus === "unknown") return null;

    const overallStatus = worseOf(spo2Status, bpmStatus);
    const last = this._entries[0];

    let shouldLog = false;
    if (!last) {
      shouldLog = true;
    } else if (last.overallStatus !== overallStatus) {
      shouldLog = true;
    } else if (overallStatus !== "good" && ts - last.ts >= SUSTAINED_EVENT_INTERVAL_MS) {
      shouldLog = true;
    } else if (ts - last.ts >= HEARTBEAT_INTERVAL_MS) {
      shouldLog = true;
    }

    if (!shouldLog) return null;

    const entry = { ts, spo2, spo2Status, bpm, bpmStatus, overallStatus };
    this._entries.unshift(entry);
    if (this._entries.length > HISTORY_MAX_ENTRIES) {
      this._entries.length = HISTORY_MAX_ENTRIES;
    }
    this._save();
    return entry;
  }

  clear() {
    this._entries = [];
    this._save();
  }

  filtered(filter) {
    if (filter === "warning") return this._entries.filter((e) => e.overallStatus === "warning");
    if (filter === "critical") return this._entries.filter((e) => e.overallStatus === "critical");
    return this._entries;
  }

  _load() {
    try {
      const raw = localStorage.getItem(this._storageKey);
      if (!raw) return [];
      const parsed = JSON.parse(raw);
      return Array.isArray(parsed) ? parsed : [];
    } catch (err) {
      console.warn("[history] failed to load from localStorage", err);
      return [];
    }
  }

  _save() {
    try {
      localStorage.setItem(this._storageKey, JSON.stringify(this._entries));
    } catch (err) {
      console.warn("[history] failed to persist to localStorage", err);
    }
  }
}
