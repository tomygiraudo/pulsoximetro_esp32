// Wires DOM <-> ConnectionManager <-> HistoryStore <-> VitalChart together.
// No framework/build step by design — this app may later be copied as-is
// into the ESP32's LittleFS/SPIFFS image and served directly by the
// firmware, so it stays plain HTML/CSS/JS with zero runtime deps besides
// Chart.js (loaded from cdnjs) and Google Fonts.

const STORAGE_KEYS = {
  wsUrl: "pulsox.wsUrl",
  demoMode: "pulsox.demoMode",
  theme: "pulsox.theme", // "auto" | "light" | "dark"
  thresholds: "pulsox.thresholds",
};

const CHART_PALETTES = {
  light: {
    spo2Line: "#0891b2",
    bpmLine: "#4a3aa7",
    axisText: "#5b7a78",
    grid: "rgba(19, 78, 74, 0.10)",
    zoneGood: "rgba(12, 163, 12, 0.08)",
    zoneWarning: "rgba(250, 178, 25, 0.14)",
    zoneCritical: "rgba(208, 59, 59, 0.08)",
  },
  dark: {
    spo2Line: "#4dd4f0",
    bpmLine: "#9b8cf2",
    axisText: "#7d939a",
    grid: "rgba(238, 246, 246, 0.14)",
    zoneGood: "rgba(12, 163, 12, 0.18)",
    zoneWarning: "rgba(250, 178, 25, 0.22)",
    zoneCritical: "rgba(227, 91, 91, 0.18)",
  },
};

const STATUS_LABEL = { good: "Normal", warning: "Precaución", critical: "Peligro", unknown: "Sin lectura" };
const SPO2_Y_RANGE = [80, 100];
const BPM_Y_RANGE = [35, 165];

const state = {
  thresholds: loadThresholds(),
  theme: localStorage.getItem(STORAGE_KEYS.theme) || "auto",
  lastSpo2: null,
  lastBpm: null,
  activeFilter: "all",
  connectionState: "offline",
  lastHello: null,
};

const history = new HistoryStore();

// ---------------------------------------------------------------- DOM refs
const el = {
  connectionBadge: document.getElementById("connection-badge"),
  connectionLabel: document.getElementById("connection-label"),
  settingsBtn: document.getElementById("settings-btn"),
  spo2Card: document.getElementById("spo2-card"),
  spo2Value: document.getElementById("spo2-value"),
  spo2StatusIcon: document.getElementById("spo2-status-icon"),
  spo2Pill: document.getElementById("spo2-status-pill"),
  bpmCard: document.getElementById("bpm-card"),
  bpmValue: document.getElementById("bpm-value"),
  bpmStatusIcon: document.getElementById("bpm-status-icon"),
  bpmPill: document.getElementById("bpm-status-pill"),
  signalSection: document.getElementById("signal-quality-section"),
  signalFill: document.getElementById("signal-bar-fill"),
  historyList: document.getElementById("history-list"),
  filterChips: document.querySelectorAll(".filter-chips button"),
  settingsDrawer: document.getElementById("settings-drawer"),
  settingsClose: document.getElementById("settings-close"),
  settingsBackdrop: document.querySelector(".settings-backdrop"),
  wsUrlInput: document.getElementById("ws-url-input"),
  connectBtn: document.getElementById("ws-connect-btn"),
  demoToggle: document.getElementById("demo-toggle"),
  themeSegmented: document.getElementById("theme-segmented"),
  clearHistoryBtn: document.getElementById("clear-history-btn"),
  deviceInfo: document.getElementById("device-info"),
  toast: document.getElementById("toast"),
  thresholdInputs: {
    spo2Good: document.getElementById("th-spo2-good"),
    spo2Warning: document.getElementById("th-spo2-warning"),
    bpmLowWarning: document.getElementById("th-bpm-low-warning"),
    bpmLowGood: document.getElementById("th-bpm-low-good"),
    bpmHighGood: document.getElementById("th-bpm-high-good"),
    bpmHighWarning: document.getElementById("th-bpm-high-warning"),
  },
};

// -------------------------------------------------------------- charts

function currentPalette() {
  return CHART_PALETTES[resolvedTheme() === "dark" ? "dark" : "light"];
}

const spo2Chart = new VitalChart({
  canvas: document.getElementById("spo2-chart"),
  emptyStateEl: document.getElementById("spo2-chart-empty"),
  lineColor: currentPalette().spo2Line,
  yRange: SPO2_Y_RANGE,
  unit: "%",
  theme: { axisText: currentPalette().axisText, grid: currentPalette().grid },
  getZones: () => {
    const p = currentPalette();
    const t = state.thresholds;
    return [
      { min: SPO2_Y_RANGE[0], max: t.spo2Warning, color: p.zoneCritical },
      { min: t.spo2Warning, max: t.spo2Good, color: p.zoneWarning },
      { min: t.spo2Good, max: SPO2_Y_RANGE[1], color: p.zoneGood },
    ];
  },
});

const bpmChart = new VitalChart({
  canvas: document.getElementById("bpm-chart"),
  emptyStateEl: document.getElementById("bpm-chart-empty"),
  lineColor: currentPalette().bpmLine,
  yRange: BPM_Y_RANGE,
  unit: "lpm",
  theme: { axisText: currentPalette().axisText, grid: currentPalette().grid },
  getZones: () => {
    const p = currentPalette();
    const t = state.thresholds;
    return [
      { min: BPM_Y_RANGE[0], max: t.bpmLowWarning, color: p.zoneCritical },
      { min: t.bpmLowWarning, max: t.bpmLowGood, color: p.zoneWarning },
      { min: t.bpmLowGood, max: t.bpmHighGood, color: p.zoneGood },
      { min: t.bpmHighGood, max: t.bpmHighWarning, color: p.zoneWarning },
      { min: t.bpmHighWarning, max: BPM_Y_RANGE[1], color: p.zoneCritical },
    ];
  },
});

setupRangePills("spo2-range", spo2Chart);
setupRangePills("bpm-range", bpmChart);
setupPauseButton("spo2-pause-btn", spo2Chart);
setupPauseButton("bpm-pause-btn", bpmChart);

function setupRangePills(containerId, chart) {
  const container = document.getElementById(containerId);
  if (!container) return;
  container.addEventListener("click", (e) => {
    const btn = e.target.closest("button[data-range]");
    if (!btn) return;
    container.querySelectorAll("button").forEach((b) => b.classList.toggle("is-active", b === btn));
    chart.setRangeSeconds(Number(btn.dataset.range));
  });
}

function setupPauseButton(btnId, chart) {
  const btn = document.getElementById(btnId);
  if (!btn) return;
  btn.innerHTML = iconSvg("pause", { size: 16 });
  btn.setAttribute("aria-label", "Pausar gráfico");
  btn.addEventListener("click", () => {
    const paused = !chart.isPaused;
    chart.setPaused(paused);
    btn.innerHTML = iconSvg(paused ? "play" : "pause", { size: 16 });
    btn.setAttribute("aria-label", paused ? "Reanudar gráfico" : "Pausar gráfico");
  });
}

// ----------------------------------------------------------- connection

const connection = new ConnectionManager({
  onFrame: handleFrame,
  onState: handleConnectionState,
});

function handleFrame(msg) {
  if (msg.type === "hello") {
    state.lastHello = msg;
    renderDeviceInfo();
    return;
  }
  if (msg.type === "telemetry") {
    handleTelemetry(msg);
    return;
  }
  if (msg.type === "status") {
    showToast(msg.message || msg.code || "Evento del dispositivo");
  }
}

function handleTelemetry(msg) {
  const now = Date.now();
  const finger = !!msg.finger_detected;
  const spo2Known = finger && msg.spo2_valid && typeof msg.spo2 === "number";
  const bpmKnown = finger && msg.bpm_valid && typeof msg.bpm === "number";

  const spo2Status = spo2Known ? classifySpo2(msg.spo2, state.thresholds) : "unknown";
  const bpmStatus = bpmKnown ? classifyBpm(msg.bpm, state.thresholds) : "unknown";

  state.lastSpo2 = spo2Known ? msg.spo2 : null;
  state.lastBpm = bpmKnown ? msg.bpm : null;

  renderKpiCard(el.spo2Card, el.spo2Value, el.spo2StatusIcon, el.spo2Pill, {
    finger,
    known: spo2Known,
    value: spo2Known ? Math.round(msg.spo2) : null,
    status: spo2Status,
  });
  renderKpiCard(el.bpmCard, el.bpmValue, el.bpmStatusIcon, el.bpmPill, {
    finger,
    known: bpmKnown,
    value: bpmKnown ? Math.round(msg.bpm) : null,
    status: bpmStatus,
  });

  if (finger) {
    el.signalSection.hidden = false;
    el.signalFill.style.width = `${Math.round((msg.signal_quality || 0) * 100)}%`;
  } else {
    el.signalSection.hidden = true;
  }

  spo2Chart.push(now, spo2Known ? msg.spo2 : null);
  bpmChart.push(now, bpmKnown ? msg.bpm : null);

  const logged = history.addReading({
    ts: now,
    spo2: spo2Known ? msg.spo2 : null,
    spo2Status,
    bpm: bpmKnown ? msg.bpm : null,
    bpmStatus,
  });
  if (logged) renderHistoryList();
}

function renderKpiCard(cardEl, valueEl, iconEl, pillEl, { finger, known, value, status }) {
  cardEl.dataset.status = status === "unknown" ? "unknown" : status;
  valueEl.textContent = known ? String(value) : "—";
  iconEl.innerHTML = iconSvg(status, { size: 22 });

  let label;
  if (!finger) label = "Colocá el dedo en el sensor";
  else if (!known) label = "Calibrando…";
  else label = STATUS_LABEL[status];

  pillEl.innerHTML = `${iconSvg(status, { size: 16 })}<span>${label}</span>`;
}

function renderDeviceInfo() {
  if (!el.deviceInfo) return;
  const h = state.lastHello;
  if (!h) {
    el.deviceInfo.textContent = "Sin datos del dispositivo todavía.";
    return;
  }
  el.deviceInfo.textContent = `${h.device_id} · fw ${h.fw_version} · ${h.sensor}`;
}

function handleConnectionState(newState) {
  state.connectionState = newState;
  const label = {
    connecting: "Conectando…",
    live: "En vivo",
    reconnecting: "Reconectando…",
    offline: "Sin conexión",
    demo: "Demo",
  }[newState];
  const badgeState = newState === "connecting" ? "reconnecting" : newState;
  el.connectionBadge.dataset.state = badgeState;
  el.connectionLabel.textContent = label;
  const iconName = { live: "wifi", demo: "wifi", reconnecting: "refresh-cw", connecting: "refresh-cw", offline: "wifi-off" }[newState];
  const iconHolder = el.connectionBadge.querySelector(".badge__icon");
  if (iconHolder) iconHolder.innerHTML = iconSvg(iconName, { size: 14 });
}

// -------------------------------------------------------------- history UI

function renderHistoryList() {
  const entries = history.filtered(state.activeFilter);
  if (!entries.length) {
    el.historyList.innerHTML = `
      <li class="history-list__empty">
        ${iconSvg("unknown", { size: 28 })}
        <span>Todavía no hay lecturas${state.activeFilter !== "all" ? " para este filtro" : ""}.</span>
      </li>`;
    return;
  }

  el.historyList.innerHTML = entries
    .map((entry) => {
      const spo2Text = entry.spo2 !== null ? `${Math.round(entry.spo2)}% SpO2` : "SpO2 —";
      const bpmText = entry.bpm !== null ? `${Math.round(entry.bpm)} lpm` : "lpm —";
      return `
      <li class="history-row" data-status="${entry.overallStatus}">
        <span class="history-row__icon">${iconSvg(entry.overallStatus, { size: 20 })}</span>
        <span class="history-row__body">
          <span class="history-row__readings">${spo2Text} · ${bpmText}</span>
          <span class="history-row__label">${STATUS_LABEL[entry.overallStatus]}</span>
        </span>
        <time class="history-row__time" datetime="${new Date(entry.ts).toISOString()}" title="${new Date(entry.ts).toLocaleString()}">
          ${formatRelativeTime(entry.ts)}
        </time>
      </li>`;
    })
    .join("");
}

function formatRelativeTime(ts) {
  const diffSec = Math.round((Date.now() - ts) / 1000);
  if (diffSec < 10) return "ahora";
  if (diffSec < 60) return `hace ${diffSec}s`;
  const diffMin = Math.round(diffSec / 60);
  if (diffMin < 60) return `hace ${diffMin} min`;
  const diffH = Math.round(diffMin / 60);
  if (diffH < 24) return `hace ${diffH} h`;
  return new Date(ts).toLocaleDateString();
}

// keep relative timestamps fresh without rebuilding the whole list on a fast tick
setInterval(() => {
  el.historyList.querySelectorAll("time[datetime]").forEach((t) => {
    t.textContent = formatRelativeTime(new Date(t.getAttribute("datetime")).getTime());
  });
}, 30000);

el.filterChips.forEach((btn) => {
  btn.addEventListener("click", () => {
    state.activeFilter = btn.dataset.filter;
    el.filterChips.forEach((b) => b.classList.toggle("is-active", b === btn));
    renderHistoryList();
  });
});

let clearArmed = false;
el.clearHistoryBtn.addEventListener("click", () => {
  if (!clearArmed) {
    clearArmed = true;
    el.clearHistoryBtn.textContent = "";
    el.clearHistoryBtn.append(iconSvgNode("trash", { size: 16 }), document.createTextNode(" ¿Confirmar borrado?"));
    setTimeout(() => {
      if (!clearArmed) return;
      clearArmed = false;
      resetClearButton();
    }, 3000);
    return;
  }
  clearArmed = false;
  history.clear();
  renderHistoryList();
  resetClearButton();
  showToast("Historial borrado");
});

function resetClearButton() {
  el.clearHistoryBtn.innerHTML = `${iconSvg("trash", { size: 16 })}<span>Borrar historial</span>`;
}
resetClearButton();

function iconSvgNode(name, opts) {
  const wrap = document.createElement("span");
  wrap.innerHTML = iconSvg(name, opts);
  return wrap.firstChild;
}

// -------------------------------------------------------------- settings

function openSettings() {
  el.settingsDrawer.hidden = false;
  el.settingsDrawer.setAttribute("aria-hidden", "false");
  requestAnimationFrame(() => {
    el.settingsDrawer.dataset.open = "true";
    el.wsUrlInput.focus({ preventScroll: true });
  });
  document.addEventListener("keydown", onSettingsKeydown);
}

function closeSettings() {
  el.settingsDrawer.dataset.open = "false";
  el.settingsDrawer.setAttribute("aria-hidden", "true");
  document.removeEventListener("keydown", onSettingsKeydown);
  setTimeout(() => {
    if (el.settingsDrawer.dataset.open === "false") el.settingsDrawer.hidden = true;
  }, 320);
  el.settingsBtn.focus();
}

function onSettingsKeydown(e) {
  if (e.key === "Escape") closeSettings();
}

el.settingsBtn.addEventListener("click", openSettings);
el.settingsClose.addEventListener("click", closeSettings);
el.settingsBackdrop.addEventListener("click", closeSettings);

el.wsUrlInput.value = localStorage.getItem(STORAGE_KEYS.wsUrl) || "";

el.connectBtn.addEventListener("click", () => {
  const url = el.wsUrlInput.value.trim();
  if (!/^wss?:\/\/.+/.test(url)) {
    showToast("Ingresá una URL válida (ws://IP/ws)");
    return;
  }
  localStorage.setItem(STORAGE_KEYS.wsUrl, url);
  localStorage.setItem(STORAGE_KEYS.demoMode, "false");
  el.demoToggle.checked = false;
  connection.connectTo(url);
});

el.demoToggle.addEventListener("change", () => {
  const on = el.demoToggle.checked;
  localStorage.setItem(STORAGE_KEYS.demoMode, String(on));
  if (on) {
    connection.startDemo();
  } else {
    const url = localStorage.getItem(STORAGE_KEYS.wsUrl);
    if (url) connection.connectTo(url);
    else connection.disconnect();
  }
});

// --------------------------------------------------------------- theme

function resolvedTheme() {
  if (state.theme === "auto") {
    return window.matchMedia?.("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  }
  return state.theme;
}

function applyTheme() {
  const root = document.documentElement;
  if (state.theme === "auto") root.removeAttribute("data-theme");
  else root.setAttribute("data-theme", state.theme);

  const p = currentPalette();
  spo2Chart.setTheme({ lineColor: p.spo2Line, axisText: p.axisText, grid: p.grid });
  bpmChart.setTheme({ lineColor: p.bpmLine, axisText: p.axisText, grid: p.grid });

  el.themeSegmented.querySelectorAll("button").forEach((b) => b.classList.toggle("is-active", b.dataset.theme === state.theme));
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
  if (state.lastSpo2 !== null) {
    const status = classifySpo2(state.lastSpo2, state.thresholds);
    el.spo2Card.dataset.status = status;
    el.spo2StatusIcon.innerHTML = iconSvg(status, { size: 22 });
    el.spo2Pill.innerHTML = `${iconSvg(status, { size: 16 })}<span>${STATUS_LABEL[status]}</span>`;
  }
  if (state.lastBpm !== null) {
    const status = classifyBpm(state.lastBpm, state.thresholds);
    el.bpmCard.dataset.status = status;
    el.bpmStatusIcon.innerHTML = iconSvg(status, { size: 22 });
    el.bpmPill.innerHTML = `${iconSvg(status, { size: 16 })}<span>${STATUS_LABEL[status]}</span>`;
  }
  spo2Chart.redraw();
  bpmChart.redraw();
}

Object.entries(el.thresholdInputs).forEach(([key, input]) => {
  if (!input) return;
  input.addEventListener("change", () => {
    const value = Number(input.value);
    if (Number.isNaN(value)) return;
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

el.settingsBtn.innerHTML = iconSvg("settings", { size: 20 });
el.settingsClose.innerHTML = iconSvg("x", { size: 20 });

populateThresholdInputs();
applyTheme();
renderHistoryList();
renderDeviceInfo();
renderKpiCard(el.spo2Card, el.spo2Value, el.spo2StatusIcon, el.spo2Pill, { finger: false, known: false, value: null, status: "unknown" });
renderKpiCard(el.bpmCard, el.bpmValue, el.bpmStatusIcon, el.bpmPill, { finger: false, known: false, value: null, status: "unknown" });

const savedUrl = localStorage.getItem(STORAGE_KEYS.wsUrl);
const savedDemo = localStorage.getItem(STORAGE_KEYS.demoMode);
const shouldDemo = savedDemo === "true" || (!savedUrl && savedDemo === null);

if (shouldDemo) {
  el.demoToggle.checked = true;
  connection.startDemo();
} else if (savedUrl) {
  connection.connectTo(savedUrl);
} else {
  handleConnectionState("offline");
}

// Service workers require a secure context; this silently no-ops when the
// app is loaded from the ESP32 over plain http on the local network.
if ("serviceWorker" in navigator && (location.protocol === "https:" || location.hostname === "localhost")) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("sw.js").catch((err) => console.warn("[sw] registration failed", err));
  });
}
