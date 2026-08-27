// Service worker over the app shell, so the page keeps working offline.
//
// Network-first: the network wins whenever it answers and its response
// refreshes the cache, so a rebuilt .wasm is picked up on the next load
// instead of one load later. The cache is the offline fallback, not the source
// of truth — which matters more here than in a hand-written PWA, because
// index.html, minimal-qt.js and minimal-qt.wasm are generated together and a
// stale one of the three is an unexplainable crash rather than a dead button.
//
// Two rules learned the hard way, both about what a worker must never do:
//
//   1. Never resolve respondWith() with undefined or Response.error(). A
//      caches.match() miss resolves to undefined, and handing that to
//      respondWith() fails the interception itself — Firefox then replaces the
//      page with NS_ERROR_INTERCEPTION_FAILED, which names neither the request
//      that failed nor the reason. Every path below ends in a real Response.
//   2. Never precache atomically. cache.addAll() rejects as a unit, so one
//      failure — a 404, or the 14 MB wasm against a tight storage quota —
//      leaves the cache completely empty, which is exactly the state that then
//      trips rule 1. Each file is cached on its own.

const CACHE = 'minimal-qt-shell-v2';
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

// Shown when a navigation cannot be served from either the network or the
// cache. Anything is better than a browser error code here: this is the one
// screen a user sees when the prototype is broken.
const OFFLINE_PAGE = `<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Minimal Prototype — Qt</title>
<style>
 body { font: 16px/1.5 system-ui, sans-serif; margin: 0; padding: 2rem;
        background: #14161a; color: #e8eaee; }
 code { background: #22262e; padding: .1em .4em; border-radius: 4px; }
 p { max-width: 34rem; }
</style></head><body>
<h1>Offline, and the app is not cached</h1>
<p>This page is served by its own service worker. It could not reach the
server, and the 14 MB WebAssembly build is not in the offline cache yet — so
there is nothing to run.</p>
<p>Start the server and reload:</p>
<p><code>cd dist/pwa &amp;&amp; python3 -m http.server 8080</code></p>
<p>The first successful load fills the cache; after that this page stays
usable with the server switched off.</p>
</body></html>`;

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE).then((cache) =>
      // One request at a time and one failure at a time. `cache: 'reload'`
      // bypasses the HTTP cache while filling the shell cache; without it a
      // revalidation-free hit can bake a stale file into a fresh generation.
      Promise.allSettled(APP_SHELL.map((url) =>
        cache.add(new Request(url, { cache: 'reload' }))))
    ).catch(() => {})
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

async function fromCacheOrExplain(request) {
  const hit = await caches.match(request);
  if (hit) return hit;

  if (request.mode === 'navigate') {
    const shell = await caches.match('./index.html');
    if (shell) return shell;
    return new Response(OFFLINE_PAGE, {
      status: 503,
      headers: { 'Content-Type': 'text/html; charset=utf-8' },
    });
  }
  // A missing asset, offline. Say which one: the browser's own message for a
  // failed WebAssembly fetch mentions neither the file nor the worker.
  return new Response(`Offline and not cached: ${new URL(request.url).pathname}`, {
    status: 503,
    headers: { 'Content-Type': 'text/plain; charset=utf-8' },
  });
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
      .catch(() => fromCacheOrExplain(event.request))
  );
});
