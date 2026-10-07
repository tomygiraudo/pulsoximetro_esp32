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
const SOURCE_HINTS = {
  cloud: "Muestra la medición en vivo y el historial compartido desde cualquier red: el ESP32 sube los datos a la nube (Firebase).",
  local:
    "La app y el ESP32 deben estar en la misma red WiFi. Si la app se abre por https (GitHub Pages) el navegador bloquea ws://: usá la Nube o abrila por http.",
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

const ppg = new PpgTrace({
  canvas: el.ppgCanvas,
  getColors: () => {
    const p = palette();
    return { line: p.bpmLine, surface: p.surface, grid: p.grid, label: p.axisText, track: p.track };
  },
});

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
      endSession();
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
  if (newState !== "live" && newState !== "demo" && state.session) {
    endSession({ silent: true });
    showToast("Conexión perdida: se cerró la medición");
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

/** Opens a measurement (the device's start packet, or — if telemetry shows up
 * without one, e.g. after a reconnect while the device is still measuring —
 * an implicit one). `id` / `startedAt` (ms epoch) override the defaults. */
function beginSession({ implicit = false, id = null, startedAt = null } = {}) {
  if (state.session) endSession({ silent: true });
  const now = Date.now();
  if (typeof startedAt !== "number" || !Number.isFinite(startedAt)) startedAt = now;
  startedAt = clamp(startedAt, now - 24 * 3600 * 1000, now);
  state.session = { id: id || `m-${startedAt}`, startedAt, implicit };
  resetLiveValues();
  ppg.clear();
  history.startSession({ id: state.session.id, startedAt, implicit });
  renderAll();
  if (!implicit) showToast("Medición iniciada");
}

function endSession({ silent = false } = {}) {
  if (!state.session) return;
  history.endSession(Date.now());
  state.session = null;
  resetLiveValues();
  ppg.clear();
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
  renderPpgEmpty();
}

// ---------------------------------------------------------- dashboard UI

function dashboardModel() {
  const t = state.thresholds;
  const unknownVital = { ringValue: null, value: "—", status: "unknown" };

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
    const reasons = [];
    if (state.spo2Status === "critical") reasons.push(`SpO₂ por debajo de ${t.spo2Warning} %.`);
    if (state.bpmStatus === "critical") {
      reasons.push(state.bpm < t.bpmLowWarning ? "Frecuencia cardíaca muy baja." : "Frecuencia cardíaca muy alta.");
    }
    return {
      title: "Valores críticos",
      sub: `${reasons.join(" ")} Si te sentís mal, buscá atención médica.`,
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

// keep the elapsed-time label fresh between the 1 Hz telemetry frames
setInterval(() => {
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
}

function groupHtml({ session, active, rows }) {
  const start = session.startedAt;
  const end = session.endedAt ?? rows[0].ts;
  const title = session.legacy ? "Lecturas anteriores" : formatGroupTitle(start);
  const meta = session.legacy ? "" : active ? "En curso" : `Medición de ${formatDuration(end - start)}`;
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
        <h3 class="history-group__title">${title}</h3>
        <span class="history-group__meta">${meta}</span>
      </div>
      <ul class="history-list">${items}</ul>
    </div>`;
}

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
  if (SOURCES.includes(saved) && (saved !== "cloud" || cloudConfig)) return saved;
  const url = localStorage.getItem(STORAGE_KEYS.wsUrl);
  const demo = localStorage.getItem(STORAGE_KEYS.demoMode);
  if (demo === "true") return "demo";
  if (url) return "local";
  if (cloudConfig) return "cloud";
  return demo === null ? "demo" : "local"; // "local" without an address = disconnected
}

/** Switches the data source: stops the current one (closing any open
 * measurement in the history it was writing to), swaps the history and the PPG
 * delay, and starts the new one. */
function setSource(source, { persist = true } = {}) {
  if (persist) localStorage.setItem(STORAGE_KEYS.source, source);
  connection.disconnect();
  state.source = source;
  state.hello = null;
  state.battery = null;
  history = source === "cloud" ? cloudHistory : localHistory;
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
