# QR Scanner Prototype

A minimal installable Progressive Web App that opens the device camera, scans
for a QR code, and shows the decoded payload in a textbox. The textbox is
editable, and whatever it contains — scanned or typed — is re-encoded as a QR
code below it, so the app works in both directions. No build step, no bundler,
no network dependency at runtime.

## Run

Serve the folder over HTTP (a service worker won't register from `file://`, and
`getUserMedia` needs a secure context):

```sh
cd prototype-qr-scanner
python3 -m http.server 8080
# then open http://localhost:8080 in a modern browser
```

`http://localhost` counts as a secure context, so the camera works there. To
test on a **phone**, `http://<your-ip>:8080` will *not* work — see
[Hosting over HTTPS](#hosting-over-https).

## Hosting over HTTPS

### Why it's needed

Both APIs the app depends on are gated behind a *secure context*:

| API | Requirement |
| --- | --- |
| `navigator.mediaDevices.getUserMedia` | secure context, plus a permission grant |
| `navigator.serviceWorker.register` | secure context, **and** no certificate error on the origin |

A secure context means `https://`, or one of the localhost exemptions
(`http://localhost`, `http://127.0.0.1`, `http://[::1]`). `http://192.168.1.20:8080`
is *not* exempt — the camera simply never starts, which is the single most
common reason this prototype appears broken on a phone.

Pick whichever option below matches where you're testing. Options 1 and 2 are
enough for a laptop; for a phone, prefer option 3 (any device) or option 4
(Android only).

### Option 1 — Self-signed certificate + Python TLS server

No extra tooling: `openssl` and Python's `ssl` module are all that's needed.

Generate a key and certificate that cover `localhost` *and* your LAN address —
the SAN list is what browsers actually match, so a missing IP entry produces a
name-mismatch error even after you trust the certificate:

```sh
# Keep certificates out of the source tree — repo-root tmp/ is gitignored.
mkdir -p ../tmp/tls && cd ../tmp/tls

# LAN address of the interface that actually reaches the network.
# (This only queries the routing table — it sends no traffic to 1.1.1.1.)
LANIP=$(ip route get 1.1.1.1 | awk '{for (i = 1; i <= NF; i++) if ($i == "src") {print $(i+1); exit}}')
# macOS:  LANIP=$(ipconfig getifaddr en0)      # en0 = Wi-Fi, en1 = wired on some models
echo "$LANIP"                                  # check this looks like your LAN address

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 365 \
  -keyout dev-key.pem -out dev-cert.pem \
  -subj "/CN=qr-prototype-dev" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:$LANIP"

openssl x509 -in dev-cert.pem -noout -subject -ext subjectAltName -dates   # sanity check
```

Two portability notes:

- `hostname -I` is a **net-tools** extension, so it exists on Debian/Ubuntu but
  fails with `invalid option -- 'I'` on Arch and anywhere else shipping
  inetutils' `hostname`. `ip route get` above works on any Linux with iproute2.
  If the machine has several interfaces, `ip -4 -o addr show scope global`
  lists them all so you can pick by hand.
- macOS ships LibreSSL, whose `openssl req` has no `-addext`. Use
  `brew install openssl` and call that binary, or use option 2.

`python3 -m http.server` cannot do TLS on its own, so wrap it in six lines —
save this as `serve-https.py` next to the certificate:

```python
#!/usr/bin/env python3
"""Serve the current directory over HTTPS (dev only, self-signed cert)."""
import http.server, ssl, sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8443
cert = sys.argv[2] if len(sys.argv) > 2 else "dev-cert.pem"
key = sys.argv[3] if len(sys.argv) > 3 else "dev-key.pem"

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)

httpd = http.server.HTTPServer(("0.0.0.0", port), http.server.SimpleHTTPRequestHandler)
httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
print(f"serving https://localhost:{port}/ (Ctrl-C to stop)")
httpd.serve_forever()
```

Serve the prototype (the server serves its *working directory*, so start it
from the prototype folder and point at the certificate by path):

```sh
cd /path/to/prototype-qr-scanner
python3 ../tmp/tls/serve-https.py 8443 ../tmp/tls/dev-cert.pem ../tmp/tls/dev-key.pem
# open https://localhost:8443/index.html
```

Verify from the shell before blaming the browser:

```sh
curl --cacert ../tmp/tls/dev-cert.pem https://localhost:8443/manifest.json
```

**Trust the certificate — don't just click through the warning.** A self-signed
certificate produces a *"Your connection is not private"* interstitial, and
bypassing it leaves the origin flagged with a certificate error, which blocks
service-worker registration (so offline mode and installability stay broken)
even though the camera may still work. Instead:

- **Linux / Chrome:** `chrome://settings/certificates` → *Authorities* → *Import*
  → `dev-cert.pem` → check *Trust this certificate for identifying websites*.
- **macOS:** `sudo security add-trusted-cert -d -r trustRoot \
  -k /Library/Keychains/System.keychain dev-cert.pem`.
- **Windows:** `certutil -addstore -f Root dev-cert.pem` (elevated).
- **Firefox** keeps its own store: *Settings → Privacy & Security →
  Certificates → View Certificates → Authorities → Import*.
- **Android:** copy `dev-cert.pem` to the device, then *Settings → Security →
  Encryption & credentials → Install a certificate → CA certificate*.
- **iOS:** AirDrop/email `dev-cert.pem` to the device, install the profile via
  *Settings → General → VPN & Device Management*, **then** enable it under
  *Settings → General → About → Certificate Trust Settings* — the second step
  is separate and easy to miss, and nothing works without it.

Restart the browser after importing.

### Option 2 — mkcert (locally-trusted CA)

[mkcert](https://github.com/FiloSottile/mkcert) automates all of the above: it
creates a local CA, installs it into the system *and* Firefox stores, and issues
leaf certificates from it, so no interstitial appears at all.

```sh
mkcert -install                                    # once per machine
mkcert localhost 127.0.0.1 "$LANIP"                # $LANIP as set in option 1
# -> ./localhost+2.pem and ./localhost+2-key.pem
python3 ../tmp/tls/serve-https.py 8443 localhost+2.pem localhost+2-key.pem
```

For phones, run `mkcert -CAROOT` and install the `rootCA.pem` from that
directory using the Android/iOS steps in option 1.

### Option 3 — Public HTTPS tunnel (works on any phone)

The least fiddly route for real devices: a tunnel gives you a publicly
resolvable hostname with a genuine, already-trusted certificate, so the camera,
the service worker and *Add to Home Screen* all behave exactly as in
production.

```sh
# terminal 1 — plain HTTP is fine; the tunnel terminates TLS
cd prototype-qr-scanner && python3 -m http.server 8080

# terminal 2 — pick one
cloudflared tunnel --url http://localhost:8080     # prints https://<random>.trycloudflare.com
ngrok http 8080                                    # prints https://<random>.ngrok-free.app
```

Open the printed URL on the phone. Note that the tunnel exposes the app to the
public internet for as long as it runs — fine for a prototype with no data
behind it, but don't leave it up unattended.

### Option 4 — Android without any TLS (`adb reverse`)

`adb reverse` makes the phone's own `localhost` point at the dev machine's port,
which lands inside the localhost secure-context exemption — no certificate, no
tunnel:

```sh
adb reverse tcp:8080 tcp:8080
# then browse to http://localhost:8080 on the phone
```

Chrome's *Insecure origins treated as secure* flag
(`chrome://flags/#unsafely-treat-insecure-origin-as-secure`, add
`http://<lan-ip>:8080`) achieves the same for plain LAN HTTP, and exists on
desktop Chrome as `--unsafely-treat-insecure-origin-as-secure=http://<lan-ip>:8080`.
Both are per-device debug switches, not a hosting strategy.

### Option 5 — Real static hosting

The app is six static files with no backend, so any static host works and gives
you HTTPS for free: GitHub Pages, GitLab Pages, Netlify, Cloudflare Pages, S3 +
CloudFront. Point the host at the `prototype-qr-scanner/` directory; there is no
build step. Two things to get right:

- Serve `vendor/jsQR.min.js` from the **same origin** as the page. The paths in
  `index.html`, `sw.js` and `manifest.json` are all relative, so the app works
  from a subdirectory (e.g. `https://user.github.io/repo/prototype-qr-scanner/`)
  without edits.
- Serve `sw.js` with `Content-Type: application/javascript` and a short
  `Cache-Control` — a stale service worker is the usual reason a deployed
  update doesn't appear. Bump `CACHE` in `sw.js` when the shell changes.

Self-hosting behind Caddy gets a Let's Encrypt certificate automatically:

```
qr.example.com {
    root * /srv/prototype-qr-scanner
    file_server
}
```

### Tooling not in this container

`openssl` and `python3` are present, so **option 1 works here as-is**.
`mkcert`, `cloudflared`, `ngrok`, `caddy` and `adb` are **not** in the image —
options 2–4 need them added to the relevant `Dockerfile` first, or run from the
host.

## How it works

1. **Engine pick** (`pickEngine`) — if the browser ships the native
   [`BarcodeDetector`](https://developer.mozilla.org/en-US/docs/Web/API/BarcodeDetector)
   API *and* it lists `qr_code` as a supported format, that's used: zero extra
   bytes, decoding on a background thread. Otherwise the app falls back to the
   vendored **jsQR** build. The chosen engine is shown in the *Camera* header.
2. **Camera** — `getUserMedia` with `facingMode: { ideal: 'environment' }` so
   phones open the rear camera. After permission is granted the app enumerates
   `videoinput` devices (labels are only exposed post-permission) and fills the
   dropdown; *Switch camera* cycles through them.
3. **Scan loop** — a `requestAnimationFrame` loop throttled to one decode every
   `SCAN_INTERVAL_MS` (100 ms). On the native path the video element is handed
   straight to `detector.detect()`. On the jsQR path the frame is drawn to an
   offscreen canvas downscaled to `DECODE_MAX_EDGE` (640 px on the long edge —
   full-resolution frames make jsQR crawl on phones) and the RGBA buffer goes
   to `jsQR(...)` with `inversionAttempts: 'attemptBoth'`, which also catches
   light-on-dark codes.
4. **Content** — the payload lands in the textbox with character/byte counts and
   a timestamp, plus *Copy* / *Clear*. Scanning keeps running: holding the same
   code in frame only refreshes the "last seen" time, while a different code
   replaces the value and triggers a short vibration where supported.
5. **Generate** — the textbox is *editable*, and `setContent()` is the single
   path both a scan and a keystroke go through, so the *Generated QR code*
   canvas always tracks the current text. Typing is debounced by 120 ms (each
   keystroke would otherwise redo the Reed-Solomon encode and repaint the
   canvas). *Download PNG* saves the canvas via `toDataURL`.

The camera is released on *Stop*, on `pagehide`, and whenever the page is
backgrounded — otherwise the OS camera indicator stays lit and the scan loop
burns battery behind other apps.

### Layout

One content column, `--measure` (480px) wide, centred. Three things keep narrow
phones from scrolling sideways, all of which had to be fixed after the page did
exactly that:

- `section { min-width: 0 }`. Grid items default to `min-width: auto`, so the
  column is at least as wide as its widest child's min-content width — and a
  `<select>` sizes itself to its longest `<option>`. One long camera label
  (`/dev/video0 (04f2:b6dd)` style names get much longer) therefore widened the
  whole page. The select also gets `text-overflow: ellipsis`.
- The generated code is sized from measured space, not a constant.
- `overflow-wrap: anywhere` on the hint lines, since camera labels and pasted
  URLs contain no break opportunities.

There is deliberately no `overflow-x: hidden` — it would hide this class of bug
rather than fix it.

### Overwriting scanned text

The scanner and the keyboard write to the same box, so the rule is: **a scan
only overwrites the text when it sees a *different* code than last time.**
Re-recognising the code already in frame refreshes the timestamp and nothing
else, which is what lets you scan a code, edit the result, and keep your edit
while the camera is still pointed at the original. Press *Stop* first if you
want the box left alone entirely; *Clear* resets the dedupe state so the code in
frame can be picked up again.

### Encoding details

- **Byte mode, UTF-8.** qrcode-generator's default string→bytes conversion is
  latin1 (`charCodeAt & 0xff`), which silently corrupts anything non-ASCII —
  `app.js` overrides `qrcode.stringToBytes` with `TextEncoder`. Note that byte
  mode carries no ECI header, so a reader that assumes latin1 rather than
  UTF-8 can still mis-render non-ASCII payloads; the vendored jsQR reads them
  back correctly.
- **Version 0 / level M.** Type `0` lets the library pick the smallest version
  that fits; error-correction level M (~15% recovery) is the usual default.
  Both are constants at the top of the *Content ↔ QR code* section in `app.js`.
- **Crisp modules.** The canvas is sized to an integer number of device pixels
  per module (plus the 4-module quiet zone the spec requires) and uses
  `image-rendering: pixelated`, so module edges never land on half pixels.
- **Fits the space available.** The rendered size comes from the wrapper's
  measured content box, clamped to `QR_MIN_PX`…`QR_MAX_PX` (120–320 CSS px), and
  is recomputed on resize and rotation. A hardcoded size is what makes a phone
  scroll sideways — see [Layout](#layout).
- **Always dark-on-white**, in both colour schemes — inverted codes are a coin
  flip across scanner apps.
- Payloads too long for a version-40 code are reported in the UI rather than
  thrown; qrcode-generator raises a bare string, not an `Error`.

## Files

- `index.html` — markup and CSS.
- `app.js` — engine selection, camera handling, scan loop, QR rendering, UI wiring.
- `vendor/jsQR.min.js` — [jsQR](https://github.com/cozmo/jsQR) 1.4.0 (MIT),
  the decoder fallback.
- `vendor/qrcode-generator.min.js` —
  [qrcode-generator](https://github.com/kazuhikoarase/qrcode-generator) 1.4.4
  (MIT), the encoder. Both are vendored so the app has no CDN dependency and
  works offline.
- `sw.js` — service worker; cache-first over the whole app shell. Bump `CACHE`
  when any shell file changes.
- `manifest.json` — PWA manifest, makes the page installable.
- `icon.svg` — app icon.

## Browser support

| Browser | Decode path |
| --- | --- |
| Chrome / Edge / Android WebView | native `BarcodeDetector` |
| Safari (macOS + iOS), Firefox | vendored jsQR |

Generating has no such split — qrcode-generator plus a 2D canvas works
everywhere. Since the whole shell is cached by the service worker (both vendored
libraries included), the app is fully functional offline after the first load.

## Verified

Tested headless against Chromium with a synthetic camera feed
(`--use-file-for-fake-video-capture` fed a generated Y4M clip of a QR code):

- jsQR path decodes the payload from the live camera stream exactly.
- Native path exercised by injecting a `BarcodeDetector` stub (the API is not
  implemented in Chromium on Linux).
- `Stop` and page-backgrounding both release the `MediaStreamTrack`.
- Reloading with the origin server killed still serves the whole shell from the
  service worker cache.
- The vendored jsQR decodes ASCII, URL, multi-byte UTF-8 and 300-character
  payloads, normal and colour-inverted.

Generation is checked by a **round trip**: the generated canvas is read back
with `getImageData` and decoded by jsQR inside the page, so the assertion is
"this code actually scans", not "a canvas got painted". 11/11 checks pass:

- A scanned payload re-encodes to a code that decodes back to the same string.
- Overwriting the textbox re-encodes to the *new* string (`input` event path).
- A UTF-8 payload (`Fußball 18:30 — Platz 3 ⚽`) survives the round trip — it
  does not with the library's default latin1 conversion.
- 3000 bytes shows *too long* with the library's own limit in the message and
  the download button disabled, and recovers on the next edit.
- Empty content clears the canvas; *Clear* resets canvas, buttons and pills.
- `toDataURL('image/png')` produces a PNG data URL.
- Both scan paths still pass unchanged after the change.

Layout was measured, not eyeballed, via `Emulation.setDeviceMetricsOverride` at
280 / 320 / 360 / 390 / 412 / 768 / 1280 px with device pixel ratios from 1 to 3,
comparing `documentElement.scrollWidth` against `clientWidth` with a generated
code on screen:

- Horizontal overflow is **0 px at every width**. Before the fix it was 67 px at
  320 px and 20 px at 360 px, with the `<section>`s measuring 364–371 px
  regardless of viewport.
- The code shrinks to fit (177 px at a 280 px viewport, 313 px at 412 px) and
  still decodes there — checked by round-tripping 12-, 200- and 500-character
  payloads at 280–360 px, where a 500-char code is 53×53 modules.

The HTTPS instructions in [option 1](#option-1--self-signed-certificate--python-tls-server)
were run end to end in this container:

- The `openssl` command produces a certificate whose SANs match both
  `https://localhost:8443` and `https://<lan-ip>:8443` (checked with
  `curl --cacert`).
- Served through `serve-https.py`, Chromium reports `isSecureContext === true`,
  registers the service worker, and completes a scan over TLS.
- Loading the same URL with the certificate untrusted gives
  `chrome-error://chromewebdata/` and the *"Your connection is not private"*
  interstitial instead of the app — which is why the trust step is not optional.

Steps for browsers and platforms other than Chromium-on-Linux (the certificate
imports, mkcert, tunnels, `adb reverse`) are from the vendors' documented
behaviour and were not exercised here.

## Known limitations

- Only the **first** QR code in a frame is reported when several are visible.
- No torch/flash toggle and no tap-to-focus; on cameras without continuous
  autofocus a small code may need manual distance adjustment.
- Nothing is persisted — a reload leaves whatever the browser restores in the
  textbox (which is re-encoded on load) and no scan history.
- The framing reticle is cosmetic: the decoder scans the whole frame, not just
  the area inside the box.
- The generator is fixed to byte mode at level M: no numeric/alphanumeric
  compaction for short digit-only payloads, no choice of error-correction level,
  and no logo/colour options.
- Leaving the camera running while typing means a *different* code entering the
  frame overwrites what you typed. Press *Stop* to edit undisturbed.
