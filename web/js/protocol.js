// Parsing + validation for the telemetry protocol defined in PROTOCOL.md,
// and the pure classification logic that turns a reading into a
// good/warning/critical/unknown status given the current thresholds.

const DEFAULT_THRESHOLDS = Object.freeze({
  spo2Good: 95, // >= this is normal
  spo2Warning: 90, // >= this (and < spo2Good) is caution; below is danger
  bpmLowGood: 60,
  bpmHighGood: 100,
  bpmLowWarning: 50, // [bpmLowWarning, bpmLowGood) and (bpmHighGood, bpmHighWarning] are caution
  bpmHighWarning: 120,
});

/** Parses one WebSocket text frame. Returns null (and logs) on malformed JSON
 * or an unrecognized/missing `type` — the caller should simply drop it,
 * a live telemetry stream isn't the place to throw on one bad frame. */
function parseTelemetryFrame(raw) {
  let msg;
  try {
    msg = JSON.parse(raw);
  } catch (err) {
    console.warn("[protocol] frame not valid JSON, dropping", err);
    return null;
  }
  if (!msg || typeof msg !== "object" || typeof msg.type !== "string") {
    console.warn("[protocol] frame missing type, dropping", msg);
    return null;
  }
  if (msg.type !== "hello" && msg.type !== "telemetry" && msg.type !== "status") {
    console.warn("[protocol] unknown frame type, dropping", msg.type);
    return null;
  }
  return msg;
}

function classifySpo2(spo2, thresholds = DEFAULT_THRESHOLDS) {
  if (spo2 === null || spo2 === undefined || Number.isNaN(spo2)) return "unknown";
  if (spo2 >= thresholds.spo2Good) return "good";
  if (spo2 >= thresholds.spo2Warning) return "warning";
  return "critical";
}

function classifyBpm(bpm, thresholds = DEFAULT_THRESHOLDS) {
  if (bpm === null || bpm === undefined || Number.isNaN(bpm)) return "unknown";
  if (bpm >= thresholds.bpmLowGood && bpm <= thresholds.bpmHighGood) return "good";
  if (
    (bpm >= thresholds.bpmLowWarning && bpm < thresholds.bpmLowGood) ||
    (bpm > thresholds.bpmHighGood && bpm <= thresholds.bpmHighWarning)
  ) {
    return "warning";
  }
  return "critical";
}
