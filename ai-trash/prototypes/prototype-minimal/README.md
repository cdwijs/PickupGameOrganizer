# Minimal Prototype

A single-page PWA that stitches together three ideas from the other
prototypes:

- A login form that the browser recognizes and offers to save (pattern from
  [`prototype-password/`](../prototype-password/)).
- Two compact agenda cards (styling from
  [`mockups/agenda-view/`](../mockups/agenda-view/)) whose date, weekday and
  player count are filled in from the roster the user pastes.
- A paste box that parses a Terrible Football roster message and an output
  box that regenerates it with the user's name added or removed — with
  clipboard read/write buttons and a *Going* / *Not going* toggle per game.

The password is captured only so the browser saves it; the app does not
otherwise use it.

## Run

Serve the folder over HTTP:

```sh
cd prototype-minimal
python3 -m http.server 8080
# then open http://localhost:8080 in a modern browser
```

`http://localhost` counts as a secure context, so the Clipboard API,
password saving and service-worker registration all work there. On a phone,
`http://<lan-ip>:8080` will **not** work — the browser requires HTTPS
off-localhost.

## Hosting over HTTPS

The setup below is the same one used by `prototype-qr-scanner/`. See
[`../prototype-qr-scanner/README.md`](../prototype-qr-scanner/README.md) for
the mkcert / tunnel / `adb reverse` alternatives; a self-signed certificate
plus a six-line Python TLS server is enough for laptop and phone testing.

Generate a key and certificate that cover `localhost` **and** your LAN
address (the SAN list is what the browser matches — a missing IP entry
produces a name-mismatch error even after you trust the certificate):

```sh
# Keep certificates out of the source tree — repo-root tmp/ is gitignored.
mkdir -p ../tmp/tls && cd ../tmp/tls

# LAN address of the interface that actually reaches the network.
# (This only queries the routing table — it sends no traffic to 1.1.1.1.)
LANIP=$(ip route get 1.1.1.1 | awk '{for (i = 1; i <= NF; i++) if ($i == "src") {print $(i+1); exit}}')
# macOS:  LANIP=$(ipconfig getifaddr en0)     # en0 = Wi-Fi
echo "$LANIP"

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 365 \
  -keyout dev-key.pem -out dev-cert.pem \
  -subj "/CN=minimal-prototype-dev" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:$LANIP"

openssl x509 -in dev-cert.pem -noout -subject -ext subjectAltName -dates
```

Save this next to the certificate as `serve-https.py`:

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
from this folder and point at the certificate by path):

```sh
cd /path/to/prototype-minimal
python3 ../tmp/tls/serve-https.py 8443 ../tmp/tls/dev-cert.pem ../tmp/tls/dev-key.pem
# open https://localhost:8443/ on desktop, or https://<lan-ip>:8443/ on a phone
```

**Trust the certificate — don't click through the warning.** Bypassing the
interstitial leaves the origin flagged with a certificate error, which
disables service-worker registration (so the PWA install prompt and offline
mode stay broken). The per-OS steps (Chrome/Firefox/macOS/Windows/Android/iOS)
are in the `prototype-qr-scanner` README under *Option 1*.

## What to try

1. Sign in with any username and password — the browser should offer to save
   the credentials.
2. Paste the sample message below into **Paste roster**. The two cards should
   fill in with the two dates, the weekday, and the player count.
3. Tap **Not going** on either card. Your username is appended into the first
   empty slot (or a new slot if all are full) and the *Updated roster* box
   rewrites itself. Tap **Going** to remove your name.
4. Tap **Copy to clipboard** — paste the result into another chat as your
   reply.
5. Reload. The browser should offer the saved credential; the *Account* pill
   shows who you're signed in as, and the toggles pick up your going status
   from whatever is currently pasted.

### Sample message

```
⚽ Terrible Football Haarlem
🕖 19.00 ~ 21:00
🧭 We play here: https://tinyurl.com/TF-pitch
📜 Read our house rules here: https://tinyurl.com/TF-Huisregels
🙌 8 players gets the game going. At 30, the pitch is full.
🔥 Joining the fun? Copy this message, add your name and post the updated version below!

__________________________

🗓️ Friday 07.08.2026

01. Teize
02. Alex

__________________________

🗓️ Monday 10.08.2026

01. Teize
02. Amine
03.
04.

⚽ Football pools, 🍖 barbecues, 🧠 pub quizzes and plenty of other terrible ideas. Join the fun in our Terrible Offtopic group: 👉 https://tinyurl.com/TF-offtopic
```

## Files

- `index.html` — markup, styling, and the three sections (login, agenda, paste).
- `app.js` — form handling, roster parser and rewriter, toggle logic,
  clipboard glue, service-worker registration.
- `manifest.json` — PWA manifest; makes the page installable.
- `sw.js` — cache-first service worker over the app shell. Bump `CACHE` when
  any shell file changes.
- `icon.svg` — soccer-ball icon on the standard rounded background.

## Notes and limitations

- The parser recognises the first *two* `🗓️` date blocks and populates the
  two cards in the order they appear. Extra blocks are ignored.
- Player lines must start with a number and a dot (`01.`, `1.`, `03.`, …).
  Names between dots are trimmed. Empty slots (`03. `) are kept when the
  user leaves and refilled first when they toggle back to going.
- Going / not-going is only a rewrite of the pasted text; nothing is sent
  anywhere. Copy the *Updated roster* back into the group chat to actually
  publish your reply.
- The Clipboard read requires user activation and a secure context; on
  browsers without `navigator.clipboard.readText` (or when it's denied) the
  button surfaces an error and the user can still paste manually into the
  textarea.
