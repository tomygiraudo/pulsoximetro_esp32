// Owns exactly one active data source at a time: either a live WebSocket to
// the ESP32, or the local TelemetrySimulator (demo mode). Reconnects the
// WebSocket with capped exponential backoff and reports connection state
// changes so the UI badge always reflects what's actually happening.

const RECONNECT_BASE_MS = 1000;
const RECONNECT_MAX_MS = 15000;

class ConnectionManager {
  /** @param {{onFrame: (msg: object) => void, onState: (state: "connecting"|"live"|"reconnecting"|"offline"|"demo") => void}} handlers */
  constructor(handlers) {
    this._onFrame = handlers.onFrame;
    this._onState = handlers.onState;
    this._ws = null;
    this._url = null;
    this._reconnectAttempt = 0;
    this._reconnectTimer = null;
    this._mode = "offline"; // "live" | "demo" | "offline"
    this._simulator = new TelemetrySimulator();
    this._manuallyStopped = true;
  }

  get mode() {
    return this._mode;
  }

  connectTo(url) {
    this.stopDemo();
    this._manuallyStopped = false;
    this._url = url;
    this._mode = "live";
    this._openSocket();
  }

  disconnect() {
    this._manuallyStopped = true;
    this._clearReconnectTimer();
    this.stopDemo(); // no-op if a WebSocket (not the simulator) is the active source
    if (this._ws) {
      this._ws.close();
      this._ws = null;
    }
    this._mode = "offline";
    this._onState("offline");
  }

  startDemo() {
    this.disconnect();
    this._manuallyStopped = true; // demo mode should never trigger WS reconnect
    this._mode = "demo";
    this._simulator.start((frame) => this._onFrame(frame));
    this._onState("demo");
  }

  stopDemo() {
    this._simulator.stop();
  }

  _openSocket() {
    this._clearReconnectTimer();
    this._onState(this._reconnectAttempt > 0 ? "reconnecting" : "connecting");

    let socket;
    try {
      socket = new WebSocket(this._url);
    } catch (err) {
      console.warn("[connection] invalid WebSocket URL", this._url, err);
      this._onState("offline");
      return;
    }
    this._ws = socket;

    socket.addEventListener("open", () => {
      this._reconnectAttempt = 0;
      this._onState("live");
    });

    socket.addEventListener("message", (event) => {
      const msg = parseTelemetryFrame(event.data);
      if (msg) this._onFrame(msg);
    });

    socket.addEventListener("close", () => {
      if (this._ws !== socket) return; // stale socket, already replaced
      this._ws = null;
      if (this._manuallyStopped || this._mode !== "live") return;
      this._scheduleReconnect();
    });

    socket.addEventListener("error", () => {
      // "close" always follows "error" on WebSocket, so reconnection is
      // handled there — this just avoids an unhandled-error console spam.
    });
  }

  _scheduleReconnect() {
    this._onState("reconnecting");
    const delay = Math.min(RECONNECT_MAX_MS, RECONNECT_BASE_MS * 2 ** this._reconnectAttempt);
    this._reconnectAttempt += 1;
    this._reconnectTimer = setTimeout(() => {
      if (this._manuallyStopped || this._mode !== "live") return;
      this._openSocket();
    }, delay);
  }

  _clearReconnectTimer() {
    if (this._reconnectTimer) {
      clearTimeout(this._reconnectTimer);
      this._reconnectTimer = null;
    }
  }
}
