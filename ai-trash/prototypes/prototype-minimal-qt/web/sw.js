// Service worker over the app shell, so the page keeps working offline.
//
// Network-first, same as prototype-minimal's: the network wins whenever it
// answers and its response refreshes the cache, so a rebuilt .wasm is picked up
// on the next load instead of one load later. The cache is the offline
// fallback, not the source of truth — which matters more here than in a
// hand-written PWA, because index.html, minimal-qt.js and minimal-qt.wasm are
// generated together and a stale one of the three is an unexplainable crash
// rather than a dead button.

const CACHE = 'minimal-qt-shell-v1';
const APP_SHELL = [
  './',
  './index.html',
  './minimal-qt.js',
  './minimal-qt.wasm',
  './qtloader.js',
  './qtlogo.svg',
  './manifest.json',
  './icon.svg',
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    // `cache: 'reload'` bypasses the HTTP cache while filling the shell cache;
    // without it a revalidation-free hit can bake a stale file into a fresh
    // cache generation.
    caches.open(CACHE)
      .then((cache) => cache.addAll(APP_SHELL.map((url) => new Request(url, { cache: 'reload' }))))
      .catch(() => {})
  );
  self.skipWaiting();
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

// Keep the offline copy current on every successful fetch. Failures are
// ignored: a full quota must not break the response the page is waiting for.
function refresh(request, response) {
  if (!response || !response.ok || response.type === 'opaque') return;
  const copy = response.clone();
  caches.open(CACHE).then((cache) => cache.put(request, copy)).catch(() => {});
}

self.addEventListener('fetch', (event) => {
  if (event.request.method !== 'GET') return;
  const url = new URL(event.request.url);
  if (url.origin !== self.location.origin) return;
  event.respondWith(
    fetch(event.request)
      .then((response) => {
        refresh(event.request, response);
        return response;
      })
      .catch(() => caches.match(event.request).then((hit) => {
        if (hit) return hit;
        if (event.request.mode === 'navigate') return caches.match('./index.html');
        return Response.error();
      }))
  );
});
