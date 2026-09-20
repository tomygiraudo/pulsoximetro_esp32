// Demo-mode telemetry generator. There is no ESP32 firmware yet (see
// PROTOCOL.md), so this produces frames in the exact same shape a real
// device would send over WebSocket — it's the executable reference for the
// protocol and lets the dashboard be built/demoed today without hardware.

class TelemetrySimulator {
  constructor() {
    this._timer = null;
    this._seq = 0;
    this._t0 = performance.now();
    this._spo2 = 97.5;
    this._bpm = 74;
    this._fingerDetected = false;
    this._fingerTimer = 0;
    this._episode = null; // { field: 'spo2'|'bpm', ticksLeft, target }
  }

  start(onFrame, { intervalMs = 1000 } = {}) {
    this.stop();
    onFrame({
      type: "hello",
      device_id: "demo-simulator",
      fw_version: "sim-1.0.0",
      sensor: "MAX30102 (simulado)",
      sample_rate_hz: Math.round(1000 / intervalMs),
    });
    this._timer = setInterval(() => onFrame(this._tick()), intervalMs);
  }

  stop() {
    if (this._timer) {
      clearInterval(this._timer);
      this._timer = null;
    }
  }

  _tick() {
    this._seq += 1;
    this._advanceFinger();

    if (!this._fingerDetected) {
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
      };
    }

    this._advanceEpisode();
    this._spo2 = clamp(this._spo2 + gaussian(0, 0.25), 82, 100);
    this._bpm = clamp(this._bpm + gaussian(0, 1.1), 38, 165);

    if (this._episode) {
      const pull = (this._episode.target - this[`_${this._episode.field}`]) * 0.18;
      this[`_${this._episode.field}`] += pull;
    }

    const signalQuality = clamp(0.65 + gaussian(0, 0.12), 0.35, 0.98);

    return {
      type: "telemetry",
      seq: this._seq,
      uptime_ms: Math.round(performance.now() - this._t0),
      finger_detected: true,
      spo2: round1(this._spo2),
      spo2_valid: signalQuality > 0.45,
      bpm: Math.round(this._bpm),
      bpm_valid: signalQuality > 0.45,
      signal_quality: round2(signalQuality),
    };
  }

  _advanceFinger() {
    if (this._fingerTimer > 0) {
      this._fingerTimer -= 1;
      return;
    }
    this._fingerDetected = !this._fingerDetected;
    this._fingerTimer = this._fingerDetected
      ? randInt(25, 70) // stays "on finger" for a while
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
