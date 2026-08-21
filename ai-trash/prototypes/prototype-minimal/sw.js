// Service worker over the whole app shell. No CDN dependency, so caching these
// files makes the app fully usable offline.
//
// Network-first, not cache-first. Cache-first plus a hand-bumped CACHE name
// meant every shell change was served stale for at least one load — and a
// forgotten bump served it stale indefinitely. Worse, a fresh index.html could
// sit next to a stale app.js, which looks like a dead button rather than a
// caching problem. Now the network wins whenever it answers and the cache is
// refreshed from it; the cache is the offline fallback, not the source of truth.

const CACHE = 'minimal-shell-v9';
const APP_SHELL = [
  './',
  './index.html',
  './app.js',
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
      // Offline (or the server is gone): fall back to whatever was cached, and
      // let a navigation land on the cached shell even if that exact URL was
      // never stored.
      .catch(() => caches.match(event.request).then((hit) => {
        if (hit) return hit;
        if (event.request.mode === 'navigate') return caches.match('./index.html');
        return Response.error();
      }))
  );
});
