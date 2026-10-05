// Two independent single-series line charts (SpO2 %, BPM) shown one at a time
// in the Historial view. Deliberately NOT a combined dual-axis chart — the two
// vitals have incompatible scales and independent thresholds (never two
// y-scales on one chart). Each keeps its own rolling buffer and paints its
// zone bands via a small custom plugin instead of pulling in the annotation
// plugin as a second dependency.
//
// Colors are passed in as literal hex/rgba strings, never CSS var()/
// color-mix() references — <canvas> fillStyle parsing is stricter than the
// main CSS engine on some mobile browsers and can silently drop those.

const CHART_BUFFER_MAX_POINTS = 3600; // 1h at 1Hz

/** "#rrggbb" -> "rgba(r, g, b, a)". Shared with ppg.js (loaded after this file). */
function withAlpha(hex, alpha) {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex);
  if (!m) return hex;
  return `rgba(${parseInt(m[1], 16)}, ${parseInt(m[2], 16)}, ${parseInt(m[3], 16)}, ${alpha})`;
}

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
   * @param {string} opts.surfaceColor  fill of the ring around the live end point
   * @param {[number, number]} opts.yRange
   * @param {() => number[]} opts.getTickValues  y ticks (usually the threshold lines)
   * @param {() => Array<{min:number,max:number,color:string}>} opts.getZones
   * @param {string} opts.unit
   * @param {{axisText: string, grid: string}} opts.theme
   */
  constructor({ canvas, emptyStateEl, lineColor, surfaceColor, yRange, getTickValues, getZones, unit, theme }) {
    this._buffer = []; // { t: epoch ms, v: number|null }
    this._rangeSeconds = 600;
    this._emptyStateEl = emptyStateEl;
    this._unit = unit;
    this._lineColor = lineColor;

    const prefersReducedMotion = window.matchMedia?.("(prefers-reduced-motion: reduce)").matches;
    const lastPointVisible = (ctx) => ctx.dataIndex === ctx.dataset.data.length - 1 && ctx.raw && ctx.raw.y !== null;

    this._chart = new Chart(canvas.getContext("2d"), {
      type: "line",
      data: {
        datasets: [
          {
            data: [],
            borderColor: lineColor,
            borderWidth: 2.5,
            borderCapStyle: "round",
            borderJoinStyle: "round",
            fill: "start",
            backgroundColor: (context) => {
              const { chart } = context;
              const { ctx, chartArea } = chart;
              if (!chartArea) return "transparent";
              const gradient = ctx.createLinearGradient(0, chartArea.top, 0, chartArea.bottom);
              gradient.addColorStop(0, withAlpha(this._lineColor, 0.28));
              gradient.addColorStop(1, withAlpha(this._lineColor, 0));
              return gradient;
            },
            pointRadius: (ctx) => (lastPointVisible(ctx) ? 5 : 0),
            pointBackgroundColor: lineColor,
            pointBorderColor: surfaceColor,
            pointBorderWidth: 2.5,
            pointHoverRadius: 5,
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
        layout: { padding: { top: 6, right: 8 } },
        animation: prefersReducedMotion ? false : { duration: 200 },
        parsing: false,
        interaction: { mode: "nearest", intersect: false, axis: "x" },
        scales: {
          x: {
            type: "linear",
            grid: { display: false },
            border: { display: false },
            ticks: {
              color: theme.axisText,
              font: { family: "Figtree, system-ui, sans-serif", size: 11, weight: 600 },
              callback: (value) => formatRelativeTick(value),
            },
            // five evenly spaced ticks across the window (-range ... ahora)
            afterBuildTicks: (scale) => {
              const { min, max } = scale;
              scale.ticks = [0, 1, 2, 3, 4].map((i) => ({ value: min + (i * (max - min)) / 4 }));
            },
          },
          y: {
            min: yRange[0],
            max: yRange[1],
            grid: { color: theme.grid },
            border: { display: false },
            ticks: {
              color: theme.axisText,
              font: { family: "Figtree, system-ui, sans-serif", size: 11, weight: 600 },
            },
            afterBuildTicks: (scale) => {
              scale.ticks = getTickValues().map((value) => ({ value }));
            },
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
    this._render();
  }

  setRangeSeconds(seconds) {
    this._rangeSeconds = seconds;
    this._render();
  }

  /** Needed when the canvas becomes visible after being laid out hidden. */
  resize() {
    this._chart.resize();
    this._render();
  }

  /** Repaints without touching data — used after a threshold edit, since
   * the zone bands and tick values read live threshold state through closures
   * on every draw and just need a redraw to reflect the new values. */
  redraw() {
    this._chart.update("none");
  }

  setTheme({ lineColor, surfaceColor, axisText, grid }) {
    this._lineColor = lineColor;
    const dataset = this._chart.data.datasets[0];
    dataset.borderColor = lineColor;
    dataset.pointBackgroundColor = lineColor;
    dataset.pointBorderColor = surfaceColor;
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

  _updateEmptyState(hasData) {
    if (this._emptyStateEl) this._emptyStateEl.hidden = hasData;
  }
}

function formatRelativeTick(epochMs) {
  const diffSec = Math.round((Date.now() - epochMs) / 1000);
  if (diffSec < 5) return "ahora";
  if (diffSec < 60) return `-${diffSec}s`;
  const minutes = diffSec / 60;
  return `-${Number.isInteger(minutes) ? minutes : minutes.toFixed(1).replace(".", ",")}min`;
}
