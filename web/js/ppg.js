// Live PPG trace for the dashboard. The firmware sends the signal already
// filtered and conditioned (see PROTOCOL.md), so this only deals with
// *rendering*: it keeps a rolling buffer of samples, places them on a time
// axis from the sample rate, and draws a scrolling window on a <canvas> with
// requestAnimationFrame. Chart.js is intentionally not used here — at 50-100
// samples/s a general-purpose chart lib is the wrong tool.
//
// Timing: samples arrive in bursts (one WebSocket frame = a batch), so each
// sample gets a time from the sample rate (t0 + k/fs) instead of its arrival
// time, and the trace is drawn RENDER_DELAY_MS behind "now" so batches fill
// in smoothly. The sample clock is nudged toward the arrival clock to follow
// slow drift between the ESP32 oscillator and the phone. Over the cloud
// transport a batch is a whole second of signal, so the delay has to cover it
// (PPG_RENDER_DELAY_CLOUD_MS, see setRenderDelay()).
//
// Colors are literal strings (canvas does not resolve CSS var()); the caller
// passes them via getColors() and calls redraw() after a theme change.
// Depends on withAlpha() from charts.js.

const PPG_WINDOW_SECONDS = 6;
const PPG_RENDER_DELAY_MS = 300;
const PPG_RENDER_DELAY_CLOUD_MS = 1500; // 1 s batches + network jitter
const PPG_MAX_RESYNC_MS = 500;
const PPG_PAD_RIGHT = 7; // keeps the live-head dot inside the canvas

class PpgTrace {
  /**
   * @param {object} opts
   * @param {HTMLCanvasElement} opts.canvas
   * @param {() => {line: string, surface: string, grid: string, label: string, track: string}} opts.getColors
   */
  constructor({ canvas, getColors }) {
    this._canvas = canvas;
    this._ctx = canvas.getContext("2d");
    this._getColors = getColors;
    this._samples = []; // { t: performance.now() ms, v: number }
    this._nextT = null; // sample-clock time of the next sample to arrive
    this._fs = 50;
    this._renderDelay = PPG_RENDER_DELAY_MS;
    this._lo = -1; // smoothed vertical range
    this._hi = 1;
    this._raf = null;
    this._cssW = 0;
    this._cssH = 0;

    this._resizeObserver = new ResizeObserver(() => this._fitCanvas());
    this._resizeObserver.observe(canvas);
    this._fitCanvas();
  }

  /** Appends one batch of samples (arrived "now"). */
  push(samples, fs, now = performance.now()) {
    if (fs !== this._fs) {
      this._fs = fs;
      this._nextT = null;
    }
    const dt = 1000 / fs;
    const n = samples.length;
    // (Re)anchor so the batch's last sample lands "now" on the first batch,
    // after a stall, or when the two clocks diverged too far.
    if (this._nextT === null || Math.abs(now - (this._nextT + (n - 1) * dt)) > PPG_MAX_RESYNC_MS) {
      this._nextT = now - (n - 1) * dt;
    }
    for (let i = 0; i < n; i++) this._samples.push({ t: this._nextT + i * dt, v: samples[i] });
    this._nextT += n * dt;
    // Slow drift correction (the last sample should arrive ~ at "now").
    this._nextT += (now - (this._nextT - dt)) * 0.03;

    const horizon = now - this._renderDelay - (PPG_WINDOW_SECONDS + 1) * 1000;
    let drop = 0;
    while (drop < this._samples.length && this._samples[drop].t < horizon) drop++;
    if (drop) this._samples.splice(0, drop);
  }

  /** How far behind "now" the trace is drawn. Longer batches need a longer delay. */
  setRenderDelay(ms) {
    this._renderDelay = ms;
  }

  clear() {
    this._samples = [];
    this._nextT = null;
    this._lo = -1;
    this._hi = 1;
    this.redraw();
  }

  get hasSamples() {
    return this._samples.length > 1;
  }

  /** Runs the animation loop only while the dashboard is visible. */
  setLive(live) {
    if (live && this._raf === null) {
      const frame = () => {
        this._draw();
        this._raf = requestAnimationFrame(frame);
      };
      this._raf = requestAnimationFrame(frame);
    } else if (!live && this._raf !== null) {
      cancelAnimationFrame(this._raf);
      this._raf = null;
      this._draw();
    }
  }

  redraw() {
    this._draw();
  }

  _fitCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const rect = this._canvas.getBoundingClientRect();
    if (rect.width === 0 || rect.height === 0) return; // hidden view: refit when shown
    this._cssW = rect.width;
    this._cssH = rect.height;
    this._canvas.width = Math.round(rect.width * dpr);
    this._canvas.height = Math.round(rect.height * dpr);
    this._ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    this._draw();
  }

  _draw() {
    const ctx = this._ctx;
    const W = this._cssW;
    const H = this._cssH;
    if (!W || !H) return;
    const colors = this._getColors();
    const plotTop = 8;
    const plotBottom = H - 18; // leave room for the time labels
    const plotH = plotBottom - plotTop;
    const windowMs = PPG_WINDOW_SECONDS * 1000;
    const renderT = performance.now() - this._renderDelay;

    ctx.clearRect(0, 0, W, H);
    ctx.font = '600 11px Figtree, system-ui, sans-serif';
    ctx.textBaseline = "alphabetic";

    // one dashed line per second + baseline
    ctx.save();
    ctx.strokeStyle = colors.grid;
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 4]);
    for (let k = 1; k < PPG_WINDOW_SECONDS; k++) {
      const x = Math.round((W * k) / PPG_WINDOW_SECONDS) + 0.5;
      ctx.beginPath();
      ctx.moveTo(x, plotTop);
      ctx.lineTo(x, plotBottom);
      ctx.stroke();
    }
    ctx.setLineDash([]);
    ctx.beginPath();
    ctx.moveTo(0, plotBottom + 0.5);
    ctx.lineTo(W, plotBottom + 0.5);
    ctx.stroke();
    ctx.restore();

    ctx.fillStyle = colors.label;
    ctx.textAlign = "left";
    ctx.fillText(`−${PPG_WINDOW_SECONDS} s`, 0, H - 4);
    ctx.textAlign = "right";
    ctx.fillText("ahora", W, H - 4);

    // visible samples
    const tMin = renderT - windowMs;
    const visible = [];
    for (const s of this._samples) {
      if (s.t >= tMin && s.t <= renderT) visible.push(s);
    }
    if (visible.length < 2) {
      ctx.save();
      ctx.strokeStyle = colors.track;
      ctx.lineWidth = 3;
      ctx.lineCap = "round";
      ctx.setLineDash([1, 8]);
      ctx.beginPath();
      ctx.moveTo(2, plotTop + plotH / 2);
      ctx.lineTo(W - 2, plotTop + plotH / 2);
      ctx.stroke();
      ctx.restore();
      return;
    }

    // auto-scale (smoothed so the trace does not jump when the window slides)
    let lo = Infinity;
    let hi = -Infinity;
    for (const s of visible) {
      if (s.v < lo) lo = s.v;
      if (s.v > hi) hi = s.v;
    }
    if (hi - lo < 1e-6) {
      lo -= 1;
      hi += 1;
    }
    const pad = (hi - lo) * 0.12;
    lo -= pad;
    hi += pad;
    this._lo += (lo - this._lo) * 0.08;
    this._hi += (hi - this._hi) * 0.08;
    const span = this._hi - this._lo || 1;

    const xOf = (t) => (W - PPG_PAD_RIGHT) * (1 - (renderT - t) / windowMs);
    const yOf = (v) => plotBottom - ((v - this._lo) / span) * plotH;

    const path = new Path2D();
    visible.forEach((s, i) => {
      const x = xOf(s.t);
      const y = Math.min(plotBottom, Math.max(plotTop, yOf(s.v)));
      if (i === 0) path.moveTo(x, y);
      else path.lineTo(x, y);
    });

    // soft area under the trace
    const first = visible[0];
    const last = visible[visible.length - 1];
    const area = new Path2D(path);
    area.lineTo(xOf(last.t), plotBottom);
    area.lineTo(xOf(first.t), plotBottom);
    area.closePath();
    const fillGradient = ctx.createLinearGradient(0, plotTop, 0, plotBottom);
    fillGradient.addColorStop(0, withAlpha(colors.line, 0.2));
    fillGradient.addColorStop(1, withAlpha(colors.line, 0));
    ctx.fillStyle = fillGradient;
    ctx.fill(area);

    // trace fades in from the left, brightest at the live edge
    const strokeGradient = ctx.createLinearGradient(0, 0, W, 0);
    strokeGradient.addColorStop(0, withAlpha(colors.line, 0.18));
    strokeGradient.addColorStop(0.7, withAlpha(colors.line, 0.9));
    strokeGradient.addColorStop(1, withAlpha(colors.line, 1));
    ctx.strokeStyle = strokeGradient;
    ctx.lineWidth = 2.5;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    ctx.stroke(path);

    // live head
    const hx = xOf(last.t);
    const hy = Math.min(plotBottom, Math.max(plotTop, yOf(last.v)));
    ctx.beginPath();
    ctx.arc(hx, hy, 4.5, 0, Math.PI * 2);
    ctx.fillStyle = colors.line;
    ctx.fill();
    ctx.lineWidth = 2.5;
    ctx.strokeStyle = colors.surface;
    ctx.stroke();
  }
}
