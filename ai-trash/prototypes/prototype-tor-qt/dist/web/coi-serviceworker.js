// Cross-origin isolation for static hosts that cannot set response headers.
//
// Loaded as a page script, it registers itself as a service worker and
// reloads the page once the worker is in control. As the worker, it adds
//   Cross-Origin-Opener-Policy: same-origin
//   Cross-Origin-Embedder-Policy: require-corp
// to every same-origin response, which makes the page crossOriginIsolated and
// unlocks SharedArrayBuffer (wasm threads). Cross-origin requests (the
// Snowflake broker, CORS) pass through untouched.

if (typeof window === 'undefined') {
  self.addEventListener('install', () => self.skipWaiting());
  self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));

  self.addEventListener('fetch', event => {
    const request = event.request;
    if (new URL(request.url).origin !== self.location.origin)
      return;
    if (request.cache === 'only-if-cached' && request.mode !== 'same-origin')
      return;
    event.respondWith(fetch(request).then(response => {
      if (response.status === 0)
        return response; // opaque
      const headers = new Headers(response.headers);
      headers.set('Cross-Origin-Opener-Policy', 'same-origin');
      headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
      headers.set('Cross-Origin-Resource-Policy', 'same-origin');
      return new Response(response.body, {
        status: response.status,
        statusText: response.statusText,
        headers,
      });
    }));
  });
} else if (!window.crossOriginIsolated) {
  (() => {
    const flag = 'coiReloadedOnce';
    const remember = () => { try { sessionStorage.setItem(flag, '1'); } catch (e) { /* private mode */ } };
    const reloadedBefore = () => { try { return sessionStorage.getItem(flag) === '1'; } catch (e) { return false; } };

    if (!window.isSecureContext || !navigator.serviceWorker) {
      console.error('coi-serviceworker: needs HTTPS (or localhost) and service worker support');
      return;
    }
    navigator.serviceWorker.addEventListener('controllerchange', () => {
      remember();
      location.reload();
    });
    navigator.serviceWorker.register(document.currentScript.src).then(registration => {
      // Controlled already, yet not isolated: the worker is stale or the
      // headers did not take. Reload once, then give up instead of looping.
      if (navigator.serviceWorker.controller && !reloadedBefore()) {
        remember();
        location.reload();
      } else if (navigator.serviceWorker.controller) {
        console.error('coi-serviceworker: in control, but the page is still not cross-origin isolated');
      }
      registration.update();
    }, err => console.error('coi-serviceworker: registration failed:', err));
  })();
}
