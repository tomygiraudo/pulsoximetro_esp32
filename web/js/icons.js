// Hand-authored outline SVG icons (no icon font / no external CDN / no
// emoji — per the design system's "no emojis as icons" rule). Each is a
// self-contained <svg> string using currentColor so it inherits the
// surrounding text/status color.

const ICONS = {
  good: `<path d="M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18Z"/><path d="M8 12.5l2.5 2.5L16 9.5"/>`,
  warning: `<path d="M12 3.5 21 20H3Z" stroke-linejoin="round"/><line x1="12" y1="9.5" x2="12" y2="14"/><circle cx="12" cy="17" r="0.75" fill="currentColor" stroke="none"/>`,
  critical: `<polygon points="7.5,3 16.5,3 21,7.5 21,16.5 16.5,21 7.5,21 3,16.5 3,7.5" stroke-linejoin="round"/><line x1="12" y1="8" x2="12" y2="13"/><circle cx="12" cy="16" r="0.75" fill="currentColor" stroke="none"/>`,
  unknown: `<polyline points="3,12 8,12 10,6 14,18 16,12 21,12"/>`,
  wifi: `<path d="M4 9.5a13 13 0 0 1 16 0"/><path d="M7 13a8.5 8.5 0 0 1 10 0"/><path d="M10 16.5a4 4 0 0 1 4 0"/><circle cx="12" cy="19.5" r="1" fill="currentColor" stroke="none"/>`,
  "wifi-off": `<path d="M4 9.5a13 13 0 0 1 16 0"/><path d="M7 13a8.5 8.5 0 0 1 10 0"/><path d="M10 16.5a4 4 0 0 1 4 0"/><circle cx="12" cy="19.5" r="1" fill="currentColor" stroke="none"/><line x1="3" y1="3" x2="21" y2="21"/>`,
  "refresh-cw": `<path d="M20 11a8 8 0 0 0-14.6-4.6"/><path d="M5.4 6.4V11h4.6"/><path d="M4 13a8 8 0 0 0 14.6 4.6"/><path d="M18.6 17.6V13H14"/>`,
  settings: `<circle cx="12" cy="12" r="3"/><path d="M12 2.5v3M12 18.5v3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M2.5 12h3M18.5 12h3M4.9 19.1l2.1-2.1M17 7l2.1-2.1"/>`,
  x: `<line x1="5" y1="5" x2="19" y2="19"/><line x1="19" y1="5" x2="5" y2="19"/>`,
  pause: `<rect x="6" y="4" width="4" height="16" rx="1" fill="currentColor" stroke="none"/><rect x="14" y="4" width="4" height="16" rx="1" fill="currentColor" stroke="none"/>`,
  play: `<polygon points="7,4 20,12 7,20" fill="currentColor" stroke="none" stroke-linejoin="round"/>`,
  trash: `<path d="M4 7h16"/><path d="M9 7V4.5A1.5 1.5 0 0 1 10.5 3h3A1.5 1.5 0 0 1 15 4.5V7"/><path d="M6 7l1 13.5A1.5 1.5 0 0 0 8.5 22h7a1.5 1.5 0 0 0 1.5-1.5L18 7"/><line x1="10" y1="11" x2="10" y2="17"/><line x1="14" y1="11" x2="14" y2="17"/>`,
};

function iconSvg(name, { className = "", size = 20 } = {}) {
  const body = ICONS[name] || ICONS.unknown;
  return `<svg class="icon ${className}" width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" focusable="false">${body}</svg>`;
}
