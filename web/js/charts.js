// Two independent single-series line charts (SpO2 %, BPM). Deliberately NOT
// a combined dual-axis chart — the two vitals have incompatible scales and
// independent thresholds (see dataviz skill anti-pattern: never two y-scales
// on one chart). Each keeps its own rolling buffer, its own time-range
// window, its own pause state, and paints its zone bands via a small custom
// plugin instead of pulling in the annotation plugin as a second dependency.
//
// Colors are passed in as literal hex/rgba strings, never CSS var()/
// color-mix() references — <canvas> fillStyle parsing is stricter than the
// main CSS engine on some mobile browsers and can silently drop those.

const CHART_BUFFER_MAX_POINTS = 3600; // 1h at 1Hz

const RANGE_OPTIONS = [
  { key: "2m", label: "2 min", seconds: 120 },
  { key: "10m", label: "10 min", seconds: 600 },
  { key: "1h", label: "1 h", seconds: 3600 },
];

function zoneBandsPlugin(getZones) {
  return {
    id: "zoneBands",
    beforeDraw(chart) {
      const zones = getZones();
      if (!zones || !zones.length) return;
      const { ctx, chartArea, scales } = chart;
      if (!chartArea) return;
      const y = scales.y;
      ctx.save();
      for (const zone of zones) {
        const yTop = y.getPixelForValue(zone.max);
        const yBottom = y.getPixelForValue(zone.min);
        ctx.fillStyle = zone.color;
        ctx.fillRect(chartArea.left, yTop, chartArea.right - chartArea.left, yBottom - yTop);
      }
      ctx.restore();
    },
  };
}

class VitalChart {
  /**
   * @param {object} opts
   * @param {HTMLCanvasElement} opts.canvas
   * @param {HTMLElement} opts.emptyStateEl
   * @param {string} opts.lineColor
   * @param {[number, number]} opts.yRange
   * @param {() => Array<{min:number,max:number,color:string}>} opts.getZones
   * @param {string} opts.unit
   * @param {{axisText: string, grid: string}} opts.theme
   */
  constructor({ canvas, emptyStateEl, lineColor, yRange, getZones, unit, theme }) {
    this._buffer = []; // { t: epoch ms, v: number|null }
    this._paused = false;
    this._rangeSeconds = RANGE_OPTIONS[1].seconds;
    this._emptyStateEl = emptyStateEl;
    this._unit = unit;

    const prefersReducedMotion = window.matchMedia?.("(prefers-reduced-motion: reduce)").matches;

    this._chart = new Chart(canvas.getContext("2d"), {
      type: "line",
      data: {
        datasets: [
          {
            data: [],
            borderColor: lineColor,
            backgroundColor: lineColor,
            borderWidth: 2,
            pointRadius: 0,
            pointHoverRadius: 4,
            pointHitRadius: 12,
            tension: 0.25,
            spanGaps: false,
            cubicInterpolationMode: "monotone",
          },
        ],
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: prefersReducedMotion ? false : { duration: 200 },
        parsing: false,
        interaction: { mode: "nearest", intersect: false, axis: "x" },
        scales: {
          x: {
            type: "linear",
            grid: { display: false },
            ticks: {
              color: theme.axisText,
              maxTicksLimit: 5,
              callback: (value) => formatRelativeTick(value),
            },
          },
          y: {
            min: yRange[0],
            max: yRange[1],
            grid: { color: theme.grid },
            ticks: { color: theme.axisText },
          },
        },
        plugins: {
          legend: { display: false },
          tooltip: {
            callbacks: {
              title: (items) => new Date(items[0].parsed.x).toLocaleTimeString(),
              label: (item) => `${item.parsed.y} ${unit}`,
            },
          },
        },
      },
      plugins: [zoneBandsPlugin(getZones)],
    });

    // Establish a sane initial x-axis window immediately (rather than
    // leaving min/max unset until the first push), otherwise Chart.js
    // defaults an empty linear scale to [0, 1] and the tick formatter reads
    // that "0" as the Unix epoch, printing a nonsense multi-million-minute
    // relative label before any data arrives.
    this._render();
  }

  push(t, value) {
    this._buffer.push({ t, v: value });
    if (this._buffer.length > CHART_BUFFER_MAX_POINTS) {
      this._buffer.splice(0, this._buffer.length - CHART_BUFFER_MAX_POINTS);
    }
    if (!this._paused) this._render();
  }

  setRangeSeconds(seconds) {
    this._rangeSeconds = seconds;
    this._render();
  }

  setPaused(paused) {
    this._paused = paused;
    if (!paused) this._render();
  }

  get isPaused() {
    return this._paused;
  }

  /** Repaints without touching data — used after a threshold edit, since
   * the zone bands read live threshold state through the getZones closure
   * on every beforeDraw and just need a redraw to reflect the new values. */
  redraw() {
    this._chart.update("none");
  }

  setTheme({ lineColor, axisText, grid }) {
    if (lineColor) {
      this._chart.data.datasets[0].borderColor = lineColor;
      this._chart.data.datasets[0].backgroundColor = lineColor;
    }
    this._chart.options.scales.x.ticks.color = axisText;
    this._chart.options.scales.y.ticks.color = axisText;
    this._chart.options.scales.y.grid.color = grid;
    this._chart.update("none");
  }

  _render() {
    const now = Date.now();
    const cutoff = now - this._rangeSeconds * 1000;
    const windowed = this._buffer.filter((p) => p.t >= cutoff);
    // Keep nulls in place (rather than filtering them out) so a dropout in
    // the signal (finger removed, invalid reading) renders as a real break
    // in the line via spanGaps:false, instead of Chart.js drawing a
    // misleading straight interpolation across the missing time.
    const points = windowed.map((p) => ({ x: p.t, y: p.v }));

    this._chart.options.scales.x.min = cutoff;
    this._chart.options.scales.x.max = now;
    this._chart.data.datasets[0].data = points;
    this._chart.update("none");
    this._updateEmptyState(windowed.some((p) => p.v !== null));
  }

  _updateEmptyState(hasData = this._buffer.some((p) => p.v !== null)) {
    if (this._emptyStateEl) this._emptyStateEl.hidden = hasData;
  }
}

function formatRelativeTick(epochMs) {
  const diffSec = Math.round((Date.now() - epochMs) / 1000);
  if (diffSec < 5) return "ahora";
  if (diffSec < 60) return `-${diffSec}s`;
  return `-${Math.round(diffSec / 60)}min`;
}
