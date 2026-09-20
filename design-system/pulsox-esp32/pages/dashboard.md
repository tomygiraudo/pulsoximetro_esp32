# Dashboard — Page Override

> Overrides `MASTER.md` for the pulse-oximetry dashboard screen. Landing-page
> patterns (App Store hero, download CTAs, GSAP grid stagger) from Master do
> **not** apply here — this is a single-screen real-time monitoring app, not a
> marketing page. Master's color tokens, typography, spacing scale, shadow
> depths and pre-delivery checklist still apply.

## Status palette (fixed — validated, never themed)

Sourced from the `dataviz` skill's reference palette and run through
`validate_palette.js`. These three are reserved for physiological state and
are never reused as decorative or brand color.

| Role | Hex | CSS Variable | Contrast on light surface | Usage |
|---|---|---|---|---|
| Good / normal | `#0CA30C` | `--color-status-good` | 3.27:1 | Icon, ring, chart zone tint |
| Caution | `#FAB219` | `--color-status-warning` | 1.79:1 | Icon + badge fill only — never as sole text color (sub-AA) |
| Danger | `#D03B3B` | `--color-status-critical` | 4.68:1 | Icon, ring, chart zone tint, can carry small text |

**Rule:** status is never color-alone. Every status surface pairs the color
with an SVG icon (check-circle / alert-triangle / alert-octagon) **and** a
text label ("Normal" / "Precaución" / "Peligro"). Caution's low contrast
(1.79:1) means amber is used as a background tint or icon/ring color, with
label text rendered in `--color-foreground`, never in raw amber-on-white.

Dark surface variants (same roles, stepped for `#1a1a19`-class backgrounds):
`--color-status-good-dark: #0ca30c` (already ≥3:1 dark), `--color-status-warning-dark: #fab219`, `--color-status-critical-dark: #e35b5b`.

## KPI Hero Card (SpO2 / BPM)

The dominant component — must be legible at arm's length on a phone screen.

```
┌─────────────────────────────────┐
│ SpO2                    [●icon] │  ← label 14px, uppercase, muted
│                                  │
│        97 %                     │  ← 72–88px, Figtree 700, tabular-nums
│                                  │
│  ● Normal                       │  ← status pill: icon + label, 16px 600
└─────────────────────────────────┘
```

- Container: `.card` from Master (radius 12px, `--shadow-md`), background
  tints toward the active status color at ~6% opacity (`color-mix` or a
  precomputed tinted variable), left border 4px solid status color — this is
  the *decorative reinforcement*, not the only signal.
- Number: `font-family: Figtree; font-weight: 700; font-size: clamp(56px, 16vw, 88px); font-variant-numeric: tabular-nums;` — tabular nums so digits don't jitter the layout on every update.
- Status pill: icon (Lucide/Heroicons outline, 20px) + label text, background = status color at 12% opacity, text = `--color-foreground` (never the raw status hex for body text, per contrast rule above).
- No finger / no signal state overrides the whole card: number replaced with
  an em-dash `—`, pill reads "Colocá el dedo en el sensor" in muted gray, no
  status color applied (this is "unknown", not "danger").
- Update transition: number cross-fades 200ms on change (Soft UI Evolution
  motion spec), never an instant snap — but no animated counting/odometer
  effect (adds latency perception to a live vital sign, avoid).

## Two KPI cards, not one combined card

SpO2 and BPM each get their own hero card, stacked on mobile (`< 640px`),
side-by-side on tablet/desktop (`≥ 640px`, `grid-template-columns: 1fr 1fr`).
Rationale: two independent vitals with independent thresholds and independent
"no signal" states — merging them into one card would force a shared status
color when SpO2 and BPM can disagree (e.g. SpO2 normal, BPM elevated).

## Signal quality bar

Thin secondary indicator under both KPI cards, not a chart:
`height: 6px; border-radius: 3px;` track in `--color-muted`, fill in
`--color-primary`, width = `signal_quality * 100%`. Label "Calidad de señal"
12px muted. Only rendered when `finger_detected: true`.

## Charts (see `dataviz` skill for full method)

**Two separate single-series line charts, never one dual-axis chart** — SpO2
(%) and BPM (bpm) have incompatible scales, so combining them on twin y-axes
was rejected per the dataviz skill's anti-pattern rule.

| Chart | Line color | Y range | Zone tints (bg bands, ~8% opacity) |
|---|---|---|---|
| SpO2 history | `--color-primary` `#0891B2` | 80–100% | red < 90, amber 90–94, green ≥ 95 |
| BPM history | violet `#4A3AA7` (categorical slot, kept distinct from status hues) | 40–160 bpm | red < 50 / > 120, amber 50–59 / 101–120, green 60–100 |

- Marks: 2px line, no point markers by default (real-time density), 4px
  rounded cap at the live end.
- No legend box (single series — chart title names it, per dataviz rule).
- Hover/touch: crosshair + tooltip showing exact value and relative time
  ("hace 12s"). On mobile this is touch-drag along the line, not hover.
- Time-range control: pill group above each chart — `2 min | 10 min | 1 h` —
  standard UI control, not part of the chart itself.
- Pause/resume control (chart domain rule for streaming data ≥1Hz): a single
  icon button top-right of the chart card; paused state freezes rendering but
  telemetry keeps buffering underneath so resuming doesn't lose data.
- `prefers-reduced-motion`: disable the line's draw-in animation and the
  cross-fade on new points; data still updates, just without the tween.

## History list

Reverse-chronological list below the charts, one row per stored reading
(not every raw packet — see `web/js/history.js` for the throttling rule).

```
[● icon]  97% SpO2   ·   74 bpm         hace 3 min
          Normal
```

- Row height ≥ 44px (touch target minimum), 8px vertical rhythm between rows.
- Leading status icon (16px) in the row's status color; trailing relative
  timestamp in `--color-foreground` muted, absolute time on tap/hover
  (title attribute + optional detail row).
- Filter chip row above the list: `Todas | Precaución | Peligro` — lets the
  user find the moments that mattered without scrolling a long normal-reading
  log.
- Empty state: centered icon + "Todavía no hay lecturas" — never a blank
  white area.

## Connection / mode badge (header)

Small pill in the header, always visible:

| State | Color | Label |
|---|---|---|
| Live (WebSocket connected) | `--color-status-good` dot + text | "En vivo" |
| Demo mode | `--color-secondary` dot + text | "Demo" |
| Reconnecting | `--color-status-warning` dot (pulsing, respects reduced-motion) | "Reconectando…" |
| Disconnected | `--color-muted` dot | "Sin conexión" |

## Settings drawer

Bottom sheet on mobile (`< 640px`), right-side panel on desktop. Contains:
WebSocket URL field (persisted to `localStorage`), demo-mode toggle,
threshold editors for SpO2/BPM zone boundaries (number inputs, not sliders —
precision matters for a health reading), "Borrar historial" destructive
action behind a confirm step, unit info, and the medical disclaimer.

## Anti-patterns specific to this page (in addition to Master's list)

- ❌ Dual-axis chart combining SpO2 and BPM.
- ❌ Status conveyed by background color alone on any element (icon + label
  always accompany it).
- ❌ Odometer/count-up animation on the hero number (misrepresents a live
  vital as if it were "loading up" to a value).
- ❌ Auto-clearing or truncating history without an explicit user action.
- ❌ Blocking the UI or showing a spinner while WebSocket reconnects — the
  last known reading stays visible with the connection badge reflecting
  staleness, per the "show last known state, don't blank the screen" rule
  for live dashboards.
