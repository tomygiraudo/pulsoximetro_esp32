// Minimal app-shell cache so the dashboard still opens (in demo mode, or to
// review the last-seen values) with a flaky/offline connection — this is a
// local monitoring tool, not a content site, so we only cache our own static
// shell, not telemetry (which never goes through fetch/cache anyway; it's
// WebSocket).

const CACHE_NAME = "pulsox-shell-v1";
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
  "./js/charts.js",
  "./js/app.js",
  "./manifest.webmanifest",
  "./assets/icons/icon-192.png",
  "./assets/icons/icon-512.png",
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => cache.addAll(SHELL_ASSETS)).then(() => self.skipWaiting())
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

  event.respondWith(
    caches.match(event.request).then(
      (cached) =>
        cached ||
        fetch(event.request)
          .then((response) => {
            const copy = response.clone();
            caches.open(CACHE_NAME).then((cache) => cache.put(event.request, copy));
            return response;
          })
          .catch(() => cached)
    )
  );
});
