// Service worker over the app shell. Network-first, the same strategy
// prototype-minimal settled on: the network wins whenever it answers and its
// response refreshes the cache, so a reload always runs the current code, and
// the cache is only there to keep the shell working offline.
//
// The API calls are cross-origin and never touch this — a cached map extract
// would be both useless and enormous.

const CACHE = 'osm-shell-v1';
const APP_SHELL = [
  './',
  './index.html',
  './app.js',
  './manifest.json',
  './icon.svg',
];

self.addEventListener('install', (event) => {
  event.waitUntil(
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
