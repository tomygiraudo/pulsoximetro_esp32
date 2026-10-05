// Demo-mode generator. There is no ESP32 firmware yet (see PROTOCOL.md), so
// this produces frames in the shape the dashboard expects from a real device
// over WebSocket — it's the executable reference for the protocol and lets
// the app be built/demoed today without hardware.
//
// Lifecycle mirrors the real device: after the connection opens the device is
// idle (only `hello`). Pressing the measurement button (here: the demo button
// in the dashboard) makes it send `measurement_start`, then `telemetry` (1 Hz)
// and `ppg` sample batches until the measurement ends.
//
// The field layout of `measurement_start` / `measurement_end` / `ppg` is
// PROVISIONAL (packet assembly is pending in PROTOCOL.md).

const SIM_PPG_FS = 50; // Hz — provisional
const SIM_PPG_BATCH_MS = 200; // 10 samples per frame at 50 Hz
const SIM_TELEMETRY_MS = 1000;

class TelemetrySimulator {
  constructor() {
    this._onFrame = null;
    this._telemetryTimer = null;
    this._ppgTimer = null;
    this._seq = 0;
    this._t0 = performance.now();
    this._measuring = false;
    this._resetState();
  }

  get isMeasuring() {
    return this._measuring;
  }

  /** Opens the (simulated) connection: the device announces itself and waits
   * idle for the measurement button. */
  start(onFrame) {
    this.stop();
    this._onFrame = onFrame;
    onFrame({
      type: "hello",
      device_id: "demo-simulator",
      fw_version: "sim-1.0.0",
      sensor: "MAX30102 (simulado)",
      sample_rate_hz: Math.round(1000 / SIM_TELEMETRY_MS),
    });
  }

  /** Simulates pressing the measurement button on the device. */
  startMeasurement() {
    if (!this._onFrame || this._measuring) return;
    this._measuring = true;
    this._resetState();
    this._onFrame({
      type: "measurement_start",
      session_id: `sim-${Date.now()}`,
      uptime_ms: Math.round(performance.now() - this._t0),
    });
    this._telemetryTimer = setInterval(() => this._onFrame && this._onFrame(this._tick()), SIM_TELEMETRY_MS);
    this._ppgTimer = setInterval(() => {
      const frame = this._ppgBatch();
      if (frame && this._onFrame) this._onFrame(frame);
    }, SIM_PPG_BATCH_MS);
  }

  /** Simulates the end of a measurement. */
  stopMeasurement() {
    if (!this._measuring) return;
    this._clearTimers();
    this._measuring = false;
    if (this._onFrame) this._onFrame({ type: "measurement_end", uptime_ms: Math.round(performance.now() - this._t0) });
  }

  /** Tears the simulated connection down (no `measurement_end` is sent, just
   * like a dropped socket). */
  stop() {
    this._clearTimers();
    this._measuring = false;
    this._onFrame = null;
  }

  _clearTimers() {
    if (this._telemetryTimer) clearInterval(this._telemetryTimer);
    if (this._ppgTimer) clearInterval(this._ppgTimer);
    this._telemetryTimer = null;
    this._ppgTimer = null;
  }

  _resetState() {
    this._spo2 = 97.5;
    this._bpm = 74;
    this._quality = 0.8;
    this._fingerDetected = false;
    this._fingerTimer = 2; // finger takes ~2 s to settle after the button press
    this._episode = null; // { field: 'spo2'|'bpm', ticksLeft, target }
    this._ppgPhase = 0;
    this._beatAmp = 1;
  }

  _tick() {
    this._seq += 1;
    this._advanceFinger();

    if (!this._fingerDetected) {
      this._quality = 0;
      return {
        type: "telemetry",
        seq: this._seq,
        uptime_ms: Math.round(performance.now() - this._t0),
        finger_detected: false,
        spo2: null,
        spo2_valid: false,
        bpm: null,
        bpm_valid: false,
        signal_quality: 0,
        battery_pct: 78,
      };
    }

    this._advanceEpisode();
    this._spo2 = clamp(this._spo2 + gaussian(0, 0.25), 82, 100);
    this._bpm = clamp(this._bpm + gaussian(0, 1.1), 38, 165);

    if (this._episode) {
      const pull = (this._episode.target - this[`_${this._episode.field}`]) * 0.18;
      this[`_${this._episode.field}`] += pull;
    }

    this._quality = clamp(0.72 + gaussian(0, 0.08), 0.5, 0.98);

    return {
      type: "telemetry",
      seq: this._seq,
      uptime_ms: Math.round(performance.now() - this._t0),
      finger_detected: true,
      spo2: round1(this._spo2),
      spo2_valid: this._quality > 0.45,
      bpm: Math.round(this._bpm),
      bpm_valid: this._quality > 0.45,
      signal_quality: round2(this._quality),
      battery_pct: 78,
    };
  }

  /** A batch of already-filtered PPG samples (zero-mean pulse waveform whose
   * beat rate follows the current simulated heart rate). No batch while the
   * finger is off the sensor, like the real device. */
  _ppgBatch() {
    if (!this._fingerDetected) return null;
    const n = Math.round((SIM_PPG_BATCH_MS / 1000) * SIM_PPG_FS);
    const step = this._bpm / 60 / SIM_PPG_FS;
    const noise = (1 - this._quality) * 0.06;
    const samples = [];
    for (let i = 0; i < n; i++) {
      this._ppgPhase += step;
      if (this._ppgPhase >= 1) {
        this._ppgPhase -= 1;
        this._beatAmp = 1 + gaussian(0, 0.04);
      }
      const v = (ppgPulse(this._ppgPhase) - 0.28) * this._beatAmp * (0.55 + 0.45 * this._quality);
      samples.push(Math.round((v + gaussian(0, noise)) * 1000));
    }
    return { type: "ppg", fs: SIM_PPG_FS, samples };
  }

  _advanceFinger() {
    if (this._fingerTimer > 0) {
      this._fingerTimer -= 1;
      return;
    }
    this._fingerDetected = !this._fingerDetected;
    this._fingerTimer = this._fingerDetected
      ? randInt(40, 90) // stays "on finger" for a while
      : randInt(3, 6); // brief gap before it's back on
  }

  _advanceEpisode() {
    if (this._episode) {
      this._episode.ticksLeft -= 1;
      if (this._episode.ticksLeft <= 0) this._episode = null;
      return;
    }
    // Small chance per tick of a transient excursion, so the dashboard's
    // caution/danger states are visible during a demo without waiting
    // for real anomalies.
    if (Math.random() < 0.02) {
      const field = Math.random() < 0.5 ? "spo2" : "bpm";
      const severity = Math.random() < 0.35 ? "critical" : "warning";
      const target =
        field === "spo2"
          ? severity === "critical"
            ? randInt(84, 89)
            : randInt(90, 94)
          : Math.random() < 0.5
          ? severity === "critical"
            ? randInt(38, 48)
            : randInt(50, 58)
          : severity === "critical"
          ? randInt(130, 155)
          : randInt(102, 120);
      this._episode = { field, target, ticksLeft: randInt(12, 25) };
    }
  }
}

/** One normalized cardiac cycle (phase 0..1): systolic peak + dicrotic wave. */
function ppgPulse(t) {
  return Math.exp(-Math.pow((t - 0.22) / 0.07, 2)) + 0.36 * Math.exp(-Math.pow((t - 0.52) / 0.1, 2));
}

function clamp(v, min, max) {
  return Math.min(max, Math.max(min, v));
}

function round1(v) {
  return Math.round(v * 10) / 10;
}

function round2(v) {
  return Math.round(v * 100) / 100;
}

function randInt(min, max) {
  return Math.floor(min + Math.random() * (max - min + 1));
}

function gaussian(mean, stdev) {
  const u = 1 - Math.random();
  const v = Math.random();
  const z = Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
  return mean + z * stdev;
}
