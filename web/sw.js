// Minimal app-shell cache so the dashboard still opens (in demo mode, or to
// review the last-seen values) with a flaky/offline connection — this is a
// local monitoring tool, not a content site, so we only cache our own static
// shell, not telemetry (which never goes through fetch/cache anyway: it's a
// WebSocket, or — for the cloud source — cross-origin requests to Firebase,
// which the origin check below leaves alone).

const CACHE_NAME = "pulsox-shell-v5";
const SHELL_ASSETS = [
  "./",
  "./index.html",
  "./css/tokens.css",
  "./css/styles.css",
  "./js/icons.js",
  "./js/protocol.js",
  "./js/simulator.js",
  "./js/connection.js",
  "./js/history.js",
  "./js/cloud-config.js",
  "./js/cloud.js",
  "./js/charts.js",
  "./js/ppg.js",
  "./js/app.js",
  "./manifest.webmanifest",
  "./assets/icons/icon-192.png",
  "./assets/icons/icon-512.png",
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    // "reload" skips the HTTP cache (GitHub Pages serves max-age=600) so the shell is one coherent release
    caches
      .open(CACHE_NAME)
      .then((cache) => cache.addAll(SHELL_ASSETS.map((asset) => new Request(asset, { cache: "reload" }))))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches
      .keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== CACHE_NAME).map((k) => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener("fetch", (event) => {
  const url = new URL(event.request.url);
  if (event.request.method !== "GET" || url.origin !== self.location.origin) return;

  // Network first, cache only as the offline fallback: a new release (GitHub Pages)
  // must show up on the next load without bumping CACHE_NAME, and the cloud settings
  // (cloud-config.js) change without a code release.
  event.respondWith(
    fetch(event.request)
      .then((response) => {
        if (response.ok) {
          const copy = response.clone();
          caches.open(CACHE_NAME).then((cache) => cache.put(event.request, copy));
        }
        return response;
      })
      .catch(() => caches.match(event.request))
  );
});
