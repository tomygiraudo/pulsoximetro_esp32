// Wires DOM <-> ConnectionManager <-> HistoryStore <-> charts/PPG together.
// No framework/build step by design — this app may later be copied as-is
// into the ESP32's LittleFS/SPIFFS image and served directly by the
// firmware, so it stays plain HTML/CSS/JS with zero runtime deps besides
// Chart.js (loaded from cdnjs) and Google Fonts.
//
// Three views switched by the bottom nav (hash routes):
//   #/history    #/dashboard (default)    #/settings
//
// Measurement lifecycle (see PROTOCOL.md): the dashboard idles until the
// device sends its start packet (measurement button pressed); from then on
// `telemetry` + `ppg` frames feed the readings, the PPG trace and the history.
// When a measurement ends with a reading, the dashboard keeps showing its final
// SpO2 / BPM and the last PPG window for RESULT_HOLD_MS (state.result) before
// going back to "—"; starting another measurement replaces it right away.
//
// Data source (Ajustes > Conexion), persisted in `pulsox.source`:
//   cloud  Firebase Realtime Database written by the ESP32 (cloud.js); shared
//          history, works from any network. Only offered if cloud-config.js is filled in.
//   local  WebSocket straight to the ESP32 on the same WiFi.
//   demo   TelemetrySimulator.
// All of them feed handleFrame() with the same frames.

const STORAGE_KEYS = {
  source: "pulsox.source", // "cloud" | "local" | "demo"
  wsUrl: "pulsox.wsUrl",
  demoMode: "pulsox.demoMode", // pre-`source` setting, only read to migrate
  theme: "pulsox.theme", // "light" (default) | "dark" | "auto"
  thresholds: "pulsox.thresholds",
};

const ROUTES = ["history", "dashboard", "settings"];
const ROUTE_TITLES = { history: "Historial", dashboard: "Dashboard", settings: "Ajustes" };
const THEMES = ["light", "dark", "auto"];
const SOURCES = ["cloud", "local", "demo"];
// ws:// from a page served over https (GitHub Pages) is blocked as mixed content, so
// "Red local" is only offered when the app is opened over plain http.
const LOCAL_AVAILABLE = location.protocol !== "https:";
const SOURCE_HINTS = {
  cloud: "Muestra la medición en vivo y el historial compartido desde cualquier red: el ESP32 sube los datos a la nube (Firebase).",
  local:
    "La app y el ESP32 deben estar en la misma red WiFi. Solo está disponible si la app se abre por http: desde https (GitHub Pages) el navegador bloquea ws://.",
  demo: "Simula mediciones para probar la app sin hardware.",
};

const PALETTES = {
  light: {
    spo2Line: "#2b58e0",
    bpmLine: "#6a44d8",
    surface: "#ffffff",
    track: "#e6e3de",
    axisText: "#5b5a6b",
    grid: "rgba(22, 21, 29, 0.08)",
    zoneGood: "rgba(12, 163, 12, 0.08)",
    zoneWarning: "rgba(250, 178, 25, 0.16)",
    zoneCritical: "rgba(208, 59, 59, 0.08)",
    themeColor: "#f4f2ef",
  },
  dark: {
    spo2Line: "#6b97ff",
    bpmLine: "#b39bff",
    surface: "#16181f",
    track: "#2a2d3a",
    axisText: "#9ea0b4",
    grid: "rgba(255, 255, 255, 0.08)",
    zoneGood: "rgba(12, 163, 12, 0.1)",
    zoneWarning: "rgba(250, 178, 25, 0.12)",
    zoneCritical: "rgba(227, 91, 91, 0.12)",
    themeColor: "#0c0d12",
  },
};

const STATUS_LABEL = { good: "Normal", warning: "Precaución", critical: "Peligro", unknown: "Sin lectura" };
const STATUS_ICON = { good: "good", warning: "warning", critical: "critical", unknown: "help" };
const SPO2_Y_RANGE = [80, 100];
const BPM_Y_RANGE = [35, 165];
const RING_RADIUS = 86;
const RING_CIRCUMFERENCE = 2 * Math.PI * RING_RADIUS;
const RING_ARC = 0.75 * RING_CIRCUMFERENCE; // 270° open gauge
const LOW_BATTERY_PCT = 20;
const DEFAULT_PPG_FS = 50;
const HOLD_LAST_VALID_MS = 5000;
const RESULT_HOLD_MS = 2 * 60 * 1000; // how long a finished measurement's result stays on the dashboard

const cloudConfig = resolveCloudConfig(window.PULSOX_CLOUD); // null -> the "Nube" source is not offered

const state = {
  thresholds: loadThresholds(),
  theme: loadTheme(),
  source: loadSource(),
  route: "dashboard",
  conn: "offline",
  hello: null,
  battery: null, // last battery_pct seen (null = unknown)
  session: null, // { id, startedAt, implicit } while a measurement is open
  result: null, // { spo2, bpm, startedAt, endedAt, expiresAt } of the measurement that just ended
  telemetrySeen: false, // first telemetry of the open measurement received
  finger: false,
  spo2: null,
  bpm: null,
  spo2Status: "unknown",
  bpmStatus: "unknown",
  spo2At: 0, // when the last valid SpO2 / BPM arrived (ms epoch)
  bpmAt: 0,
  quality: 0,
  ppgFs: DEFAULT_PPG_FS,
  lastSeq: null, // `seq` of the last telemetry: with the cloud source, the second the next PPG batch belongs to
  trendTab: "spo2",
  activeFilter: "all",
};

// Local (localStorage) history for "Red local" / "Demo"; the cloud one is read-only
// and shared. `history` points at whichever matches the current source.
const localHistory = new HistoryStore();
const cloudHistory = cloudConfig
  ? new CloudHistory({
      config: cloudConfig,
      getThresholds: () => state.thresholds,
      onChange: () => {
        if (history === cloudHistory) renderHistory();
      },
    })
  : null;
let history = localHistory;

// The PPG signal of each measurement (signal.js): this page records it while it watches the
// measurement and saves it complete when the measurement ends, so the Historial can show it.
// Same split as the history: the cloud one is shared, the local one is this browser's.
const recorder = new SignalRecorder();
const localSignals = new LocalSignalStore();
const cloudSignals = cloudConfig ? new CloudSignalStore({ config: cloudConfig }) : null;
let signals = localSignals;
const signalViews = new Map(); // sid -> { status: "loading"|"ok"|"none"|"error", signal?, scrollLeft? } of the Historial panels
const openSignals = new Set(); // sids whose panel is open

// ---------------------------------------------------------------- DOM refs
const $ = (id) => document.getElementById(id);
const el = {
  views: document.querySelectorAll(".view"),
  navItems: document.querySelectorAll(".nav-item"),
  connectionBadge: $("connection-badge"),
  connectionLabel: $("connection-label"),
  batteryChip: $("battery-chip"),
  batteryText: $("battery-text"),
  batteryFill: $("battery-fill"),
  summary: $("summary"),
  summaryIcon: $("summary-icon"),
  summaryTitle: $("dash-title"),
  summarySub: $("summary-sub"),
  demoMeasureBtn: $("demo-measure-btn"),
  spo2Card: $("spo2-card"),
  spo2Chip: $("spo2-chip"),
  spo2Ring: $("spo2-ring"),
  spo2RingFill: $("spo2-ring-fill"),
  ringTickWarning: $("ring-tick-warning"),
  ringTickGood: $("ring-tick-good"),
  spo2Value: $("spo2-value"),
  spo2Caption: $("spo2-caption"),
  spo2Pill: $("spo2-pill"),
  bpmCard: $("bpm-card"),
  bpmChip: $("bpm-chip"),
  bpmValue: $("bpm-value"),
  bpmPill: $("bpm-pill"),
  signalBars: $("signal-bars"),
  ppgCanvas: $("ppg-canvas"),
  ppgEmpty: $("ppg-empty"),
  historyCount: $("history-count"),
  historyGroups: $("history-groups"),
  trendTabs: $("trend-tabs"),
  rangePills: $("range-pills"),
  filterChips: $("filter-chips"),
  sourceSegmented: $("source-segmented"),
  sourceHint: $("source-hint"),
  localFields: $("local-fields"),
  wsUrlInput: $("ws-url-input"),
  connectBtn: $("ws-connect-btn"),
  themeSegmented: $("theme-segmented"),
  clearHistoryBtn: $("clear-history-btn"),
  clearHistoryHint: $("clear-history-hint"),
  deviceInfo: $("device-info"),
  toast: $("toast"),
  thresholdInputs: Object.fromEntries(
    ["spo2Good", "spo2Warning", "bpmLowWarning", "bpmLowGood", "bpmHighGood", "bpmHighWarning"].map((key) => [
      key,
      $(`th-${key}`),
    ])
  ),
};

// ------------------------------------------------------- small utilities

function clamp(v, min, max) {
  return Math.min(max, Math.max(min, v));
}

function palette() {
  return PALETTES[resolvedTheme()];
}

/** Keeps only tick values inside [min, max], unique and sorted. */
function ticksWithin(range, values) {
  return [...new Set(values)].filter((v) => v >= range[0] && v <= range[1]).sort((a, b) => a - b);
}

/** Clamps zone bands to the visible y range (thresholds are user-editable). */
function zonesWithin(range, zones) {
  return zones
    .map((z) => ({ ...z, min: Math.max(z.min, range[0]), max: Math.min(z.max, range[1]) }))
    .filter((z) => z.max > z.min);
}

function formatElapsed(ms) {
  const total = Math.max(0, Math.floor(ms / 1000));
  const m = Math.floor(total / 60);
  const s = total % 60;
  return `${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}`;
}

function formatDuration(ms) {
  const total = Math.max(0, Math.round(ms / 1000));
  if (total < 60) return `${total} s`;
  const m = Math.floor(total / 60);
  const s = total % 60;
  if (m < 60) return s ? `${m} min ${String(s).padStart(2, "0")} s` : `${m} min`;
  const h = Math.floor(m / 60);
  return `${h} h ${String(m % 60).padStart(2, "0")} min`;
}

function startOfDay(d) {
  return new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
}

function formatClock(ts, withSeconds = true) {
  return new Date(ts).toLocaleTimeString("es-AR", {
    hour: "2-digit",
    minute: "2-digit",
    ...(withSeconds ? { second: "2-digit" } : {}),
    hour12: false,
  });
}

function formatGroupTitle(startedAt) {
  const d = new Date(startedAt);
  const dayDiff = Math.round((startOfDay(new Date()) - startOfDay(d)) / 86400000);
  const day =
    dayDiff === 0 ? "Hoy" : dayDiff === 1 ? "Ayer" : d.toLocaleDateString("es-AR", { day: "numeric", month: "short" });
  return `${day} · ${formatClock(startedAt, false)}`;
}

// ------------------------------------------------------------------ router

function currentRoute() {
  const match = location.hash.match(/^#\/(\w+)/);
  return match && ROUTES.includes(match[1]) ? match[1] : "dashboard";
}

function applyRoute({ focusHeading = false } = {}) {
  const route = currentRoute();
  state.route = route;
  el.views.forEach((view) => {
    view.hidden = view.dataset.route !== route;
  });
  el.navItems.forEach((item) => {
    if (item.dataset.route === route) item.setAttribute("aria-current", "page");
    else item.removeAttribute("aria-current");
  });
  document.title = `PulsOx — ${ROUTE_TITLES[route]}`;
  ppg.setLive(route === "dashboard");
  if (route === "history") {
    history.refresh?.(); // cloud: re-read the shared history
    renderHistory();
    activeTrendChart().resize();
  }
  window.scrollTo(0, 0);
  if (focusHeading) document.querySelector(`#view-${route} h1`)?.focus({ preventScroll: true });
}

window.addEventListener("hashchange", () => applyRoute({ focusHeading: true }));

// --------------------------------------------------------------- charts

const spo2Chart = new VitalChart({
  canvas: $("spo2-chart"),
  emptyStateEl: $("spo2-chart-empty"),
  lineColor: palette().spo2Line,
  surfaceColor: palette().surface,
  yRange: SPO2_Y_RANGE,
  unit: "%",
  theme: { axisText: palette().axisText, grid: palette().grid },
  getTickValues: () =>
    ticksWithin(SPO2_Y_RANGE, [SPO2_Y_RANGE[0], state.thresholds.spo2Warning, state.thresholds.spo2Good, SPO2_Y_RANGE[1]]),
  getZones: () => {
    const p = palette();
    const t = state.thresholds;
    return zonesWithin(SPO2_Y_RANGE, [
      { min: SPO2_Y_RANGE[0], max: t.spo2Warning, color: p.zoneCritical },
      { min: t.spo2Warning, max: t.spo2Good, color: p.zoneWarning },
      { min: t.spo2Good, max: SPO2_Y_RANGE[1], color: p.zoneGood },
    ]);
  },
});

const bpmChart = new VitalChart({
  canvas: $("bpm-chart"),
  emptyStateEl: $("bpm-chart-empty"),
  lineColor: palette().bpmLine,
  surfaceColor: palette().surface,
  yRange: BPM_Y_RANGE,
  unit: "lpm",
  theme: { axisText: palette().axisText, grid: palette().grid },
  getTickValues: () => {
    const t = state.thresholds;
    return ticksWithin(BPM_Y_RANGE, [40, t.bpmLowGood, t.bpmHighGood, t.bpmHighWarning, 160]);
  },
  getZones: () => {
    const p = palette();
    const t = state.thresholds;
    return zonesWithin(BPM_Y_RANGE, [
      { min: BPM_Y_RANGE[0], max: t.bpmLowWarning, color: p.zoneCritical },
      { min: t.bpmLowWarning, max: t.bpmLowGood, color: p.zoneWarning },
      { min: t.bpmLowGood, max: t.bpmHighGood, color: p.zoneGood },
      { min: t.bpmHighGood, max: t.bpmHighWarning, color: p.zoneWarning },
      { min: t.bpmHighWarning, max: BPM_Y_RANGE[1], color: p.zoneCritical },
    ]);
  },
});

function activeTrendChart() {
  return state.trendTab === "bpm" ? bpmChart : spo2Chart;
}

el.trendTabs.addEventListener("click", (e) => {
  const btn = e.target.closest("button[data-tab]");
  if (!btn) return;
  state.trendTab = btn.dataset.tab;
  el.trendTabs.querySelectorAll("button").forEach((b) => {
    const on = b === btn;
    b.classList.toggle("is-active", on);
    b.setAttribute("aria-pressed", String(on));
  });
  $("spo2-chart-wrap").hidden = state.trendTab !== "spo2";
  $("bpm-chart-wrap").hidden = state.trendTab !== "bpm";
  activeTrendChart().resize();
});

el.rangePills.addEventListener("click", (e) => {
  const btn = e.target.closest("button[data-range]");
  if (!btn) return;
  el.rangePills.querySelectorAll("button").forEach((b) => {
    const on = b === btn;
    b.classList.toggle("is-active", on);
    b.setAttribute("aria-pressed", String(on));
  });
  const seconds = Number(btn.dataset.range);
  spo2Chart.setRangeSeconds(seconds);
  bpmChart.setRangeSeconds(seconds);
});

// -------------------------------------------------------------- PPG trace

function ppgColors() {
  const p = palette();
  return { line: p.bpmLine, surface: p.surface, grid: p.grid, label: p.axisText, track: p.track };
}

const ppg = new PpgTrace({ canvas: el.ppgCanvas, getColors: ppgColors });

// ----------------------------------------------------------- connection

const connection = new ConnectionManager({
  onFrame: handleFrame,
  onState: handleConnectionState,
});

function handleFrame(msg) {
  switch (msg.type) {
    case "hello":
      state.hello = msg;
      // the cloud source also reports the battery while the device is idle
      if (typeof msg.battery_pct === "number") {
        state.battery = clamp(Math.round(msg.battery_pct), 0, 100);
        renderBattery();
      }
      renderDeviceInfo();
      break;
    case "measurement_start":
      // The cloud source knows the database's session id (to match the shared
      // history) and when the measurement began (it may have started before we
      // connected); the WebSocket packet carries neither, so the app assigns them.
      beginSession(
        state.source === "cloud"
          ? { id: typeof msg.session_id === "string" ? msg.session_id : null, startedAt: msg.started_at }
          : {}
      );
      break;
    case "measurement_end":
      // the cloud source also closes a measurement when the device goes quiet ("timeout"): no result then
      endSession({ timedOut: msg.reason === "timeout" });
      break;
    case "telemetry":
      handleTelemetry(msg);
      break;
    case "ppg":
      handlePpg(msg);
      break;
    case "status":
      showToast(msg.message || msg.code || "Evento del dispositivo");
      break;
  }
}

function handleConnectionState(newState) {
  state.conn = newState;
  if (newState !== "live" && newState !== "demo") {
    clearResult(); // a result from before the drop would be stale
    if (state.session) {
      endSession({ silent: true });
      showToast("Conexión perdida: se cerró la medición");
    }
  }
  renderConnection();
  renderDashboard();
  renderDemoButton();
}

// --------------------------------------------------------- measurement

function resetLiveValues() {
  state.telemetrySeen = false;
  state.finger = false;
  state.spo2 = null;
  state.bpm = null;
  state.spo2Status = "unknown";
  state.bpmStatus = "unknown";
  state.spo2At = 0;
  state.bpmAt = 0;
  state.quality = 0;
}

/** Drops the held result of the last measurement (and its frozen PPG window). */
function clearResult() {
  if (!state.result) return;
  state.result = null;
  ppg.clear();
}

/** Opens a measurement (the device's start packet, or — if telemetry shows up
 * without one, e.g. after a reconnect while the device is still measuring —
 * an implicit one). `id` / `startedAt` (ms epoch) override the defaults. */
function beginSession({ implicit = false, id = null, startedAt = null } = {}) {
  if (state.session) endSession({ silent: true });
  const now = Date.now();
  if (typeof startedAt !== "number" || !Number.isFinite(startedAt)) startedAt = now;
  startedAt = clamp(startedAt, now - 24 * 3600 * 1000, now);
  state.session = { id: id || `m-${startedAt}`, startedAt, implicit };
  state.result = null; // the previous result gives way to the new measurement
  resetLiveValues();
  ppg.clear();
  recorder.begin(state.session.id);
  history.startSession({ id: state.session.id, startedAt, implicit });
  renderAll();
  if (!implicit) showToast("Medición iniciada");
}

/** Closes the open measurement. If it ended normally with the finger on and a
 * reading, its final values and PPG window stay on the dashboard (state.result)
 * for RESULT_HOLD_MS. A silent close (connection lost) or a `timedOut` one
 * (the device went quiet) has no result, and neither has a measurement the
 * device discarded: those lose the finger first (PROTOCOL.md). */
function endSession({ silent = false, timedOut = false } = {}) {
  if (!state.session) return;
  const now = Date.now();
  const hasResult = !silent && !timedOut && state.finger && (state.spo2 !== null || state.bpm !== null);
  state.result = hasResult
    ? { spo2: state.spo2, bpm: state.bpm, startedAt: state.session.startedAt, endedAt: now, expiresAt: now + RESULT_HOLD_MS }
    : null;
  history.endSession(now);
  // Only a measurement that ended normally keeps its signal: a lost connection or a silent device
  // leaves it incomplete, and the cloud store skips what the device discarded.
  const signal = silent || timedOut ? null : recorder.snapshot();
  if (signal) saveSignal(state.session.id, signal);
  recorder.begin(null);
  state.session = null;
  resetLiveValues();
  if (hasResult) ppg.freeze();
  else ppg.clear();
  // break the trend lines at the end of a measurement
  spo2Chart.push(Date.now(), null);
  bpmChart.push(Date.now(), null);
  renderAll();
  if (!silent) showToast("Medición finalizada");
}

function handleTelemetry(msg) {
  if (!state.session) beginSession({ implicit: true });
  const now = Date.now();
  const finger = !!msg.finger_detected;
  const spo2Known = finger && msg.spo2_valid && typeof msg.spo2 === "number";
  const bpmKnown = finger && msg.bpm_valid && typeof msg.bpm === "number";

  state.telemetrySeen = true;
  state.lastSeq = typeof msg.seq === "number" ? msg.seq : null;
  state.finger = finger;
  // A single invalid tick must not blank the readout: keep the last valid
  // value for a few seconds. (Charts and history still record the gap.)
  if (spo2Known) {
    state.spo2 = msg.spo2;
    state.spo2At = now;
  } else if (!finger || now - state.spo2At > HOLD_LAST_VALID_MS) {
    state.spo2 = null;
  }
  if (bpmKnown) {
    state.bpm = msg.bpm;
    state.bpmAt = now;
  } else if (!finger || now - state.bpmAt > HOLD_LAST_VALID_MS) {
    state.bpm = null;
  }
  state.spo2Status = state.spo2 !== null ? classifySpo2(state.spo2, state.thresholds) : "unknown";
  state.bpmStatus = state.bpm !== null ? classifyBpm(state.bpm, state.thresholds) : "unknown";
  state.quality = finger ? clamp(msg.signal_quality || 0, 0, 1) : 0;
  if (typeof msg.battery_pct === "number") state.battery = clamp(Math.round(msg.battery_pct), 0, 100);

  if (!finger) ppg.clear(); // no finger -> no waveform

  const spo2Value = spo2Known ? msg.spo2 : null;
  const bpmValue = bpmKnown ? msg.bpm : null;
  spo2Chart.push(now, spo2Value);
  bpmChart.push(now, bpmValue);

  const logged = history.addReading({
    ts: now,
    sessionId: state.session.id,
    spo2: spo2Value,
    spo2Status: spo2Known ? classifySpo2(spo2Value, state.thresholds) : "unknown",
    bpm: bpmValue,
    bpmStatus: bpmKnown ? classifyBpm(bpmValue, state.thresholds) : "unknown",
  });
  if (logged) renderHistory();

  renderBattery();
  renderDashboard();
}

function handlePpg(msg) {
  const data = extractPpgSamples(msg, state.ppgFs);
  if (!data) return;
  if (!state.session) beginSession({ implicit: true });
  // Until the first telemetry says otherwise, accept the signal; afterwards
  // only while the finger is on the sensor.
  if (state.telemetrySeen && !state.finger) return;
  state.ppgFs = data.fs;
  ppg.push(data.samples, data.fs);
  // The cloud source sends the telemetry of a second right before its PPG, so `lastSeq` says which second
  // this batch is (the WebSocket's `seq` counts something else: its batches carry no number).
  recorder.push(data.samples, data.fs, state.source === "cloud" ? state.lastSeq : null);
  renderPpgEmpty();
}

/** Keeps the signal of the measurement that just ended in the store of the current source. */
function saveSignal(sid, signal) {
  const store = signals;
  store.save(sid, signal).then((outcome) => {
    if (outcome === "failed") showToast("No se pudo guardar la señal de la medición");
    if (outcome === "denied") console.warn("[signal] la base rechazó la señal: ¿están publicadas las reglas de tools/cloud/database.rules.json?");
    if (outcome === "saved" && store === signals) {
      signalViews.delete(sid); // whatever the panel said ("sin señal…") is out of date
      hydrateSignalPanels();
    }
  });
}

// ---------------------------------------------------------- dashboard UI

function criticalAdvice(spo2Status, bpmStatus, bpm) {
  const t = state.thresholds;
  const reasons = [];
  if (spo2Status === "critical") reasons.push(`SpO₂ por debajo de ${t.spo2Warning} %.`);
  if (bpmStatus === "critical") {
    reasons.push(bpm < t.bpmLowWarning ? "Frecuencia cardíaca muy baja." : "Frecuencia cardíaca muy alta.");
  }
  return `${reasons.join(" ")} Si te sentís mal, buscá atención médica.`;
}

/** The finished measurement's final values (state.result), classified with the
 * current thresholds so editing them in Ajustes also updates the held result. */
function resultModel() {
  const r = state.result;
  const t = state.thresholds;
  const spo2Known = r.spo2 !== null;
  const bpmKnown = r.bpm !== null;
  const spo2Status = spo2Known ? classifySpo2(r.spo2, t) : "unknown";
  const bpmStatus = bpmKnown ? classifyBpm(r.bpm, t) : "unknown";
  const overall = worseOf(spo2Status, bpmStatus);
  const duration = formatElapsed(r.endedAt - r.startedAt);
  const sub =
    overall === "critical"
      ? criticalAdvice(spo2Status, bpmStatus, r.bpm)
      : overall === "warning"
      ? `Resultado final · ${duration} · revisá las lecturas resaltadas`
      : `Resultado final · ${duration} · todo en rango`;

  return {
    title: "Medición finalizada",
    sub,
    summaryStatus: overall,
    summaryIcon: STATUS_ICON[overall],
    spo2: {
      ringValue: spo2Known ? r.spo2 : null,
      value: spo2Known ? String(Math.round(r.spo2)) : "—",
      status: spo2Status,
      label: spo2Known ? STATUS_LABEL[spo2Status] : "Sin lectura",
      caption: spo2Known ? `Normal desde ${t.spo2Good} %` : "Sin lectura",
    },
    bpm: {
      value: bpmKnown ? String(Math.round(r.bpm)) : "—",
      status: bpmStatus,
      label: bpmKnown ? STATUS_LABEL[bpmStatus] : "Sin lectura",
    },
    signal: false,
    ppgEmpty: ppg.hasSamples ? null : "Sin señal registrada",
  };
}

function dashboardModel() {
  const t = state.thresholds;
  const unknownVital = { ringValue: null, value: "—", status: "unknown" };

  if (!state.session && state.result) return resultModel();

  if (!state.session) {
    if (state.conn === "live" || state.conn === "demo") {
      return {
        title: "Listo para medir",
        sub: "Presioná el botón de medición del dispositivo para empezar.",
        summaryStatus: "unknown",
        summaryIcon: "play-circle",
        spo2: { ...unknownVital, label: "En espera", caption: "Sin medición" },
        bpm: { ...unknownVital, label: "En espera" },
        signal: false,
        ppgEmpty: "La señal aparece al iniciar la medición",
      };
    }
    const connecting = state.conn === "connecting" || state.conn === "reconnecting";
    const cloud = state.source === "cloud";
    return {
      title: connecting ? "Conectando…" : "Sin conexión",
      sub: connecting
        ? cloud
          ? "Conectando con la nube."
          : "Buscando el dispositivo en la red."
        : cloud
        ? "No se pudo leer la nube. Revisá tu conexión a internet."
        : "Configurá la dirección del ESP32 en Ajustes, o elegí otra fuente de datos.",
      summaryStatus: "unknown",
      summaryIcon: connecting ? "refresh-cw" : "wifi-off",
      spo2: { ...unknownVital, label: "Sin lectura", caption: "Sin conexión" },
      bpm: { ...unknownVital, label: "Sin lectura" },
      signal: false,
      ppgEmpty: cloud ? "Sin conexión con la nube" : "Sin conexión con el dispositivo",
    };
  }

  const elapsed = formatElapsed(Date.now() - state.session.startedAt);

  if (!state.finger) {
    return {
      title: "Esperando lectura",
      sub: `Midiendo · ${elapsed} · colocá el dedo en el sensor.`,
      summaryStatus: "unknown",
      summaryIcon: "help",
      spo2: { ...unknownVital, label: "Sin lectura", caption: "Colocá el dedo\nen el sensor" },
      bpm: { ...unknownVital, label: "Sin lectura" },
      signal: false,
      ppgEmpty: "Sin señal — colocá el dedo en el sensor",
    };
  }

  const spo2Known = state.spo2 !== null;
  const bpmKnown = state.bpm !== null;
  const spo2 = {
    ringValue: spo2Known ? state.spo2 : null,
    value: spo2Known ? String(Math.round(state.spo2)) : "—",
    status: state.spo2Status,
    label: spo2Known ? STATUS_LABEL[state.spo2Status] : "Calibrando…",
    caption: spo2Known ? `Normal desde ${t.spo2Good} %` : "Calibrando…",
  };
  const bpm = {
    value: bpmKnown ? String(Math.round(state.bpm)) : "—",
    status: state.bpmStatus,
    label: bpmKnown ? STATUS_LABEL[state.bpmStatus] : "Calibrando…",
  };
  const ppgEmpty = ppg.hasSamples ? null : "Esperando señal PPG…";

  if (!spo2Known && !bpmKnown) {
    return {
      title: "Calibrando…",
      sub: `Midiendo · ${elapsed} · estabilizando la señal.`,
      summaryStatus: "unknown",
      summaryIcon: "help",
      spo2,
      bpm,
      signal: true,
      ppgEmpty,
    };
  }

  const overall = worseOf(state.spo2Status, state.bpmStatus);
  if (overall === "critical") {
    return {
      title: "Valores críticos",
      sub: criticalAdvice(state.spo2Status, state.bpmStatus, state.bpm),
      summaryStatus: "critical",
      summaryIcon: "critical",
      spo2,
      bpm,
      signal: true,
      ppgEmpty,
    };
  }
  if (overall === "warning") {
    return {
      title: "Fuera del rango habitual",
      sub: `Midiendo · ${elapsed} · revisá las lecturas resaltadas`,
      summaryStatus: "warning",
      summaryIcon: "warning",
      spo2,
      bpm,
      signal: true,
      ppgEmpty,
    };
  }
  return {
    title: "Todo en rango",
    sub: `Midiendo · ${elapsed} · lectura estable`,
    summaryStatus: "good",
    summaryIcon: "good",
    spo2,
    bpm,
    signal: true,
    ppgEmpty,
  };
}

function renderPill(pillEl, status, label) {
  pillEl.innerHTML = `${iconSvg(STATUS_ICON[status], { size: 16 })}<span>${label}</span>`;
}

function setRing(value) {
  const has = typeof value === "number";
  el.spo2Ring.dataset.hasValue = String(has);
  const fraction = has ? clamp((value - SPO2_Y_RANGE[0]) / (SPO2_Y_RANGE[1] - SPO2_Y_RANGE[0]), 0, 1) : 0;
  el.spo2RingFill.style.strokeDasharray = `${Math.max(0.001, fraction * RING_ARC).toFixed(2)} ${RING_CIRCUMFERENCE.toFixed(2)}`;
}

/** Notches on the gauge at the "Precaución" and "Normal" thresholds. */
function renderRingTicks() {
  const place = (line, value) => {
    const fraction = clamp((value - SPO2_Y_RANGE[0]) / (SPO2_Y_RANGE[1] - SPO2_Y_RANGE[0]), 0, 1);
    const theta = ((135 + fraction * 270) * Math.PI) / 180;
    const cos = Math.cos(theta);
    const sin = Math.sin(theta);
    line.setAttribute("x1", (100 + (RING_RADIUS - 9) * cos).toFixed(2));
    line.setAttribute("y1", (100 + (RING_RADIUS - 9) * sin).toFixed(2));
    line.setAttribute("x2", (100 + (RING_RADIUS + 9) * cos).toFixed(2));
    line.setAttribute("y2", (100 + (RING_RADIUS + 9) * sin).toFixed(2));
  };
  place(el.ringTickWarning, state.thresholds.spo2Warning);
  place(el.ringTickGood, state.thresholds.spo2Good);
}

function renderPpgEmpty() {
  const message = dashboardModel().ppgEmpty;
  el.ppgEmpty.textContent = message || "";
  el.ppgEmpty.hidden = !message;
}

function renderDashboard() {
  const m = dashboardModel();

  el.summary.dataset.status = m.summaryStatus;
  el.summaryIcon.innerHTML = iconSvg(m.summaryIcon, { size: 26 });
  el.summaryTitle.textContent = m.title;
  el.summarySub.textContent = m.sub;

  el.spo2Card.dataset.status = m.spo2.status;
  el.spo2Value.textContent = m.spo2.value;
  el.spo2Value.dataset.empty = String(m.spo2.value === "—");
  el.spo2Caption.textContent = m.spo2.caption;
  renderPill(el.spo2Pill, m.spo2.status, m.spo2.label);
  setRing(m.spo2.ringValue);

  el.bpmCard.dataset.status = m.bpm.status;
  el.bpmValue.textContent = m.bpm.value;
  el.bpmValue.dataset.empty = String(m.bpm.value === "—");
  renderPill(el.bpmPill, m.bpm.status, m.bpm.label);

  el.signalBars.hidden = !m.signal;
  el.signalBars.setAttribute("aria-label", `Calidad de señal ${Math.round(state.quality * 100)} %`);
  const lit = Math.round(state.quality * 5);
  el.signalBars.querySelectorAll("span").forEach((bar, i) => bar.classList.toggle("is-on", i < lit));

  el.ppgEmpty.textContent = m.ppgEmpty || "";
  el.ppgEmpty.hidden = !m.ppgEmpty;
}

function renderConnection() {
  const labels = {
    connecting: "Conectando…",
    live: state.session ? "En vivo" : "Conectado",
    reconnecting: "Reconectando…",
    offline: "Sin conexión",
    demo: "Demo",
  };
  el.connectionBadge.dataset.state = state.conn === "connecting" ? "reconnecting" : state.conn;
  el.connectionLabel.textContent = labels[state.conn] || "Sin conexión";
}

function renderBattery() {
  if (state.battery === null) {
    el.batteryChip.hidden = true;
    return;
  }
  const low = state.battery < LOW_BATTERY_PCT;
  el.batteryChip.hidden = false;
  el.batteryChip.dataset.low = String(low);
  el.batteryText.textContent = `${state.battery} %${low ? " · baja" : ""}`;
  el.batteryFill.setAttribute("width", String(Math.max(1, Math.round((13 * state.battery) / 100))));
}

function renderDemoButton() {
  const demo = state.conn === "demo";
  el.demoMeasureBtn.hidden = !demo;
  if (demo) el.demoMeasureBtn.textContent = state.session ? "Detener medición (demo)" : "Simular botón de medición";
}

function renderDeviceInfo() {
  const h = state.hello;
  el.deviceInfo.textContent = h ? `${h.device_id} · fw ${h.fw_version} · ${h.sensor}` : "Sin datos del dispositivo todavía.";
}

function renderAll() {
  renderConnection();
  renderBattery();
  renderDashboard();
  renderDemoButton();
  renderHistory();
}

el.demoMeasureBtn.addEventListener("click", () => {
  if (connection.isDemoMeasuring) connection.stopDemoMeasurement();
  else connection.startDemoMeasurement();
});

// keep the elapsed-time label fresh between the 1 Hz telemetry frames, and let
// a finished measurement's result expire (compared against the clock, not a
// timeout, so a throttled background tab still drops it on its next tick)
setInterval(() => {
  if (state.result && Date.now() >= state.result.expiresAt) {
    clearResult();
    renderDashboard();
  }
  if (state.session && state.route === "dashboard") renderDashboard();
}, 1000);

// -------------------------------------------------------------- history UI

function renderHistory() {
  const groups = history.groups(state.activeFilter);
  const count = history.sessionCount;
  el.historyCount.textContent = count ? `${count} ${count === 1 ? "medición" : "mediciones"}` : "";

  if (!groups.length) {
    const message =
      state.activeFilter !== "all"
        ? "No hay lecturas con este filtro."
        : history.status === "loading"
        ? "Cargando el historial de la nube…"
        : history.status === "error"
        ? "No se pudo leer el historial de la nube."
        : state.source === "cloud"
        ? "Todavía no hay mediciones en la nube. Presioná el botón de medición del dispositivo."
        : "Todavía no hay mediciones. Iniciá una desde el dashboard.";
    el.historyGroups.innerHTML = `<div class="history-empty">${iconSvg("history", { size: 28 })}<span>${message}</span></div>`;
    return;
  }

  el.historyGroups.innerHTML = groups.map(groupHtml).join("");
  hydrateSignalPanels();
}

function groupHtml({ session, active, rows }) {
  const start = session.startedAt;
  const end = session.endedAt ?? rows[0].ts;
  const title = session.legacy ? "Lecturas anteriores" : formatGroupTitle(start);
  const meta = session.legacy ? "" : active ? "En curso" : `Medición de ${formatDuration(end - start)}`;
  // The signal of a measurement can be opened once it is over (ids are push keys / "m-<time>": safe in an attribute).
  const hasSignal = !session.legacy && !active && SIGNAL_SID_PATTERN.test(session.id);
  const signalToggle = hasSignal
    ? `<button type="button" class="signal-toggle" data-signal-toggle="${session.id}" aria-expanded="false">${iconSvg("activity", { size: 16 })}<span>Ver señal</span></button>`
    : "";
  const signalPanel = hasSignal ? `<div class="signal-panel" data-signal-panel="${session.id}" hidden></div>` : "";
  const items = rows
    .map((entry) => {
      const spo2Text = entry.spo2 !== null ? `${Math.round(entry.spo2)} % SpO₂` : "SpO₂ —";
      const bpmText = entry.bpm !== null ? `${Math.round(entry.bpm)} lpm` : "lpm —";
      return `
        <li class="history-row" data-status="${entry.overallStatus}">
          <span class="history-row__icon">${iconSvg(STATUS_ICON[entry.overallStatus], { size: 20 })}</span>
          <span class="history-row__body">
            <span class="history-row__readings">${spo2Text} · ${bpmText}</span>
            <span class="history-row__label">${STATUS_LABEL[entry.overallStatus]}</span>
          </span>
          <time class="history-row__time" datetime="${new Date(entry.ts).toISOString()}">${formatClock(entry.ts)}</time>
        </li>`;
    })
    .join("");
  return `
    <div class="history-group">
      <div class="history-group__head">
        <div class="history-group__heading">
          <h3 class="history-group__title">${title}</h3>
          <span class="history-group__meta">${meta}</span>
        </div>
        ${signalToggle}
      </div>
      ${signalPanel}
      <ul class="history-list">${items}</ul>
    </div>`;
}

// ------------------------------------------------- recorded signal (Historial)

const SIGNAL_MESSAGES = {
  loading: "Cargando la señal…",
  none: "Esta medición no tiene señal guardada.",
  noneCloud: "Esta medición no tiene señal guardada: se guarda cuando la app está abierta mirando la medición.",
  error: "No se pudo leer la señal.",
};

function signalPanels() {
  return [...el.historyGroups.querySelectorAll("[data-signal-panel]")];
}

function hydrateSignalPanels() {
  signalPanels().forEach(renderSignalPanel);
}

/** Fills one panel from what is known of its signal, and starts loading it the first time it is opened. */
function renderSignalPanel(panel) {
  const sid = panel.dataset.signalPanel;
  const open = openSignals.has(sid);
  const toggle = panel.closest(".history-group").querySelector("[data-signal-toggle]");
  toggle.setAttribute("aria-expanded", String(open));
  toggle.querySelector("span").textContent = open ? "Ocultar señal" : "Ver señal";
  panel.hidden = !open;
  if (!open) {
    panel.innerHTML = "";
    return;
  }

  let view = signalViews.get(sid);
  if (!view) {
    view = { status: "loading" };
    signalViews.set(sid, view);
    loadSignalView(sid);
  }
  if (view.status !== "ok") {
    const text = view.status === "none" && state.source === "cloud" ? SIGNAL_MESSAGES.noneCloud : SIGNAL_MESSAGES[view.status];
    const retry = view.status === "error" ? `<button type="button" class="btn-soft" data-signal-retry>Reintentar</button>` : "";
    panel.innerHTML = `<p class="signal-panel__msg" role="status">${text}</p>${retry}`;
    return;
  }

  const { signal } = view;
  const duration = formatDuration((signal.values.length / signal.fs) * 1000);
  panel.innerHTML = `
    <div class="signal-panel__head">
      <span class="signal-panel__title">Señal PPG filtrada</span>
      <span class="signal-panel__meta">${duration} · ${signal.fs} Hz</span>
    </div>
    <div class="signal-scroll" tabindex="0" role="region" aria-label="Señal PPG de la medición: deslizá para recorrerla">
      <canvas class="signal-canvas" role="img" aria-label="Señal PPG de una medición de ${duration}"></canvas>
    </div>
    <p class="signal-panel__hint">Deslizá hacia los costados para recorrer toda la medición.</p>`;
  const scroll = panel.querySelector(".signal-scroll");
  drawPpgStrip(panel.querySelector("canvas"), signal, ppgColors());
  scroll.scrollLeft = view.scrollLeft || 0; // the list is rebuilt now and then: stay where the user was
  scroll.addEventListener("scroll", () => (view.scrollLeft = scroll.scrollLeft), { passive: true });
}

function loadSignalView(sid) {
  const store = signals;
  store.load(sid).then((res) => {
    if (store !== signals) return; // the source changed while it loaded
    signalViews.set(sid, res.status === "ok" ? { status: "ok", signal: res.signal, scrollLeft: 0 } : { status: res.status });
    const panel = signalPanels().find((p) => p.dataset.signalPanel === sid);
    if (panel) renderSignalPanel(panel);
  });
}

el.historyGroups.addEventListener("click", (e) => {
  const toggle = e.target.closest("[data-signal-toggle]");
  const retry = e.target.closest("[data-signal-retry]");
  const sid = toggle ? toggle.dataset.signalToggle : retry?.closest("[data-signal-panel]")?.dataset.signalPanel;
  if (!sid) return;
  if (toggle) {
    if (openSignals.has(sid)) openSignals.delete(sid);
    else openSignals.add(sid);
  }
  if (signalViews.get(sid)?.status !== "ok") signalViews.delete(sid); // opening again (or retrying) asks again
  const panel = signalPanels().find((p) => p.dataset.signalPanel === sid);
  if (panel) renderSignalPanel(panel);
});

el.filterChips.addEventListener("click", (e) => {
  const btn = e.target.closest("button[data-filter]");
  if (!btn) return;
  state.activeFilter = btn.dataset.filter;
  el.filterChips.querySelectorAll("button").forEach((b) => {
    const on = b === btn;
    b.classList.toggle("is-active", on);
    b.setAttribute("aria-pressed", String(on));
  });
  renderHistory();
});

let clearArmed = false;
el.clearHistoryBtn.addEventListener("click", () => {
  if (!clearArmed) {
    clearArmed = true;
    el.clearHistoryBtn.innerHTML = `${iconSvg("trash", { size: 18 })}<span>¿Confirmar borrado?</span>`;
    setTimeout(() => {
      if (!clearArmed) return;
      clearArmed = false;
      resetClearButton();
    }, 3000);
    return;
  }
  clearArmed = false;
  history.clear();
  signals.clear();
  signalViews.clear();
  openSignals.clear();
  renderHistory();
  resetClearButton();
  showToast("Historial borrado");
});

function resetClearButton() {
  el.clearHistoryBtn.innerHTML = `${iconSvg("trash", { size: 18 })}<span>Borrar historial</span>`;
}

// -------------------------------------------------------------- settings

el.wsUrlInput.value = localStorage.getItem(STORAGE_KEYS.wsUrl) || "";

el.connectBtn.addEventListener("click", () => {
  const url = el.wsUrlInput.value.trim();
  if (!/^wss?:\/\/.+/.test(url)) {
    showToast("Ingresá una URL válida (ws://IP/ws)");
    return;
  }
  localStorage.setItem(STORAGE_KEYS.wsUrl, url);
  connection.connectTo(url);
});

/** Saved source; if there is none yet, derived from the pre-`source` settings
 * (wsUrl / demoMode), preferring the cloud when it is configured. */
function loadSource() {
  const saved = localStorage.getItem(STORAGE_KEYS.source);
  if (SOURCES.includes(saved) && (saved !== "cloud" || cloudConfig) && (saved !== "local" || LOCAL_AVAILABLE)) return saved;
  const url = localStorage.getItem(STORAGE_KEYS.wsUrl);
  const demo = localStorage.getItem(STORAGE_KEYS.demoMode);
  if (demo === "true") return "demo";
  if (url && LOCAL_AVAILABLE) return "local";
  if (cloudConfig) return "cloud";
  // "local" without an address = disconnected
  return demo === null || !LOCAL_AVAILABLE ? "demo" : "local";
}

/** Switches the data source: stops the current one (closing any open
 * measurement in the history it was writing to), swaps the history and the PPG
 * delay, and starts the new one. */
function setSource(source, { persist = true } = {}) {
  if (persist) localStorage.setItem(STORAGE_KEYS.source, source);
  connection.disconnect();
  clearResult(); // never carry one source's result over to another
  state.source = source;
  state.hello = null;
  state.battery = null;
  history = source === "cloud" ? cloudHistory : localHistory;
  signals = source === "cloud" ? cloudSignals : localSignals;
  signalViews.clear();
  openSignals.clear();
  ppg.setRenderDelay(source === "cloud" ? PPG_RENDER_DELAY_CLOUD_MS : PPG_RENDER_DELAY_MS);
  renderSourceUi();
  renderBattery();
  renderDeviceInfo();
  renderHistory();

  if (source === "cloud") {
    connection.connectCloud(cloudConfig);
    cloudHistory.refresh();
  } else if (source === "demo") {
    connection.startDemo();
  } else {
    const url = localStorage.getItem(STORAGE_KEYS.wsUrl);
    if (url) connection.connectTo(url);
  }
}

function renderSourceUi() {
  el.sourceSegmented.querySelectorAll("button").forEach((b) => {
    const on = b.dataset.source === state.source;
    b.classList.toggle("is-active", on);
    b.setAttribute("aria-pressed", String(on));
  });
  el.sourceHint.textContent = SOURCE_HINTS[state.source];
  el.localFields.hidden = state.source !== "local";
  // the shared cloud history belongs to the device; the viewer cannot wipe it
  el.clearHistoryBtn.disabled = state.source === "cloud";
  el.clearHistoryHint.hidden = state.source !== "cloud";
}

el.sourceSegmented.querySelector('[data-source="cloud"]').hidden = !cloudConfig;
el.sourceSegmented.querySelector('[data-source="local"]').hidden = !LOCAL_AVAILABLE;
el.sourceSegmented.addEventListener("click", (e) => {
  const btn = e.target.closest("button[data-source]");
  if (!btn || btn.dataset.source === state.source) return;
  setSource(btn.dataset.source);
});

// --------------------------------------------------------------- theme

function loadTheme() {
  const saved = localStorage.getItem(STORAGE_KEYS.theme);
  return THEMES.includes(saved) ? saved : "light";
}

function resolvedTheme() {
  if (state.theme === "auto") {
    return window.matchMedia?.("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  }
  return state.theme;
}

function applyTheme() {
  document.documentElement.setAttribute("data-theme", state.theme);
  const p = palette();
  document.querySelector('meta[name="theme-color"]')?.setAttribute("content", p.themeColor);
  spo2Chart.setTheme({ lineColor: p.spo2Line, surfaceColor: p.surface, axisText: p.axisText, grid: p.grid });
  bpmChart.setTheme({ lineColor: p.bpmLine, surfaceColor: p.surface, axisText: p.axisText, grid: p.grid });
  ppg.redraw();
  hydrateSignalPanels();
  el.themeSegmented.querySelectorAll("button").forEach((b) => {
    const on = b.dataset.theme === state.theme;
    b.classList.toggle("is-active", on);
    b.setAttribute("aria-pressed", String(on));
  });
}

el.themeSegmented.addEventListener("click", (e) => {
  const btn = e.target.closest("button[data-theme]");
  if (!btn) return;
  state.theme = btn.dataset.theme;
  localStorage.setItem(STORAGE_KEYS.theme, state.theme);
  applyTheme();
});

window.matchMedia?.("(prefers-color-scheme: dark)").addEventListener("change", () => {
  if (state.theme === "auto") applyTheme();
});

// ------------------------------------------------------------ thresholds

function loadThresholds() {
  try {
    const raw = localStorage.getItem(STORAGE_KEYS.thresholds);
    if (!raw) return { ...DEFAULT_THRESHOLDS };
    return { ...DEFAULT_THRESHOLDS, ...JSON.parse(raw) };
  } catch {
    return { ...DEFAULT_THRESHOLDS };
  }
}

function populateThresholdInputs() {
  for (const [key, input] of Object.entries(el.thresholdInputs)) {
    if (input) input.value = state.thresholds[key];
  }
}

function reclassifyLastReading() {
  if (state.spo2 !== null) state.spo2Status = classifySpo2(state.spo2, state.thresholds);
  if (state.bpm !== null) state.bpmStatus = classifyBpm(state.bpm, state.thresholds);
  renderRingTicks();
  renderDashboard();
  spo2Chart.redraw();
  bpmChart.redraw();
}

Object.entries(el.thresholdInputs).forEach(([key, input]) => {
  if (!input) return;
  input.addEventListener("change", () => {
    const value = Number(input.value);
    if (input.value === "" || Number.isNaN(value)) return;
    state.thresholds = { ...state.thresholds, [key]: value };
    localStorage.setItem(STORAGE_KEYS.thresholds, JSON.stringify(state.thresholds));
    reclassifyLastReading();
  });
});

// --------------------------------------------------------------- toast

let toastTimer = null;
function showToast(message) {
  el.toast.textContent = message;
  el.toast.classList.add("is-visible");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.toast.classList.remove("is-visible"), 3200);
}

// ---------------------------------------------------------------- init

document.querySelectorAll("[data-icon]").forEach((node) => {
  node.innerHTML = iconSvg(node.dataset.icon, { size: 22 });
});
el.spo2Chip.innerHTML = iconSvg("droplet", { size: 16 });
el.bpmChip.innerHTML = iconSvg("heart", { size: 16 });
resetClearButton();

populateThresholdInputs();
renderRingTicks();
applyTheme();
applyRoute();
renderAll();
renderDeviceInfo();

setSource(state.source, { persist: false });

// While a measurement is running, keep the Historial (cloud) in step with the
// readings the device uploads every few seconds.
setInterval(() => {
  if (state.route === "history" && state.session) history.refresh?.();
}, 10000);

// Service workers require a secure context; this silently no-ops when the
// app is loaded from the ESP32 over plain http on the local network.
if ("serviceWorker" in navigator && (location.protocol === "https:" || location.hostname === "localhost")) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("sw.js").catch((err) => console.warn("[sw] registration failed", err));
  });
}
