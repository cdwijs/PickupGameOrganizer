// Minimal service worker: just enough to satisfy PWA installability.
// We do NOT cache the CDN-loaded Helia modules here — they're big and
// versioned, so for the prototype we let the browser HTTP cache handle
// them. Same for the app shell; iterate freely without cache-busting.

const APP_SHELL = ['./', './index.html', './manifest.json', './icon.svg'];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open('ipfs-idb-shell-v1').then((cache) => cache.addAll(APP_SHELL))
      .catch(() => {}) // best effort
  );
  self.skipWaiting();
});

self.addEventListener('activate', (event) => {
  event.waitUntil(self.clients.claim());
});

self.addEventListener('fetch', (event) => {
  const url = new URL(event.request.url);
  // Only serve same-origin app-shell requests from cache; let everything
  // else (esm.sh imports, libp2p signaling, WebRTC) hit the network.
  if (url.origin !== self.location.origin) return;
  event.respondWith(
    caches.match(event.request).then((hit) => hit || fetch(event.request))
  );
});
