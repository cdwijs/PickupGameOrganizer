# Minimal Prototype

A single-page PWA that stitches together ideas from the other prototypes:

- A login form that the browser recognizes and offers to save (pattern from
  [`prototype-password/`](../prototype-password/)), backed by a
  password-encrypted user-data record (pattern from
  [`prototype-encrypted-userdata/`](../prototype-encrypted-userdata/)).
- Two compact agenda cards (styling from
  [`mockups/agenda-view/`](../mockups/agenda-view/)) whose date, weekday and
  player count are filled in from the roster the user pastes.
- A paste box that parses a Terrible Football roster message and an output
  box that regenerates it with the user's name added or removed — with
  clipboard read/write buttons and a *Going* / *Not going* toggle per game.

## Accounts

There is no server. An "account" is one record in `localStorage`:

| key | contents |
|-----|----------|
| `prototype-minimal:salt:v1` | the 16-byte random salt, hex |
| `prototype-minimal:userdata:v1` | `nonce ‖ AES-GCM(key, user data)`, hex |
| `prototype-minimal:session:v1` | `{"username": …}` — so a reload stays signed in |

The protected plaintext is the fixed string
`Readable: This is a placeholder for the user data`.

**Signing in** derives a key from the entered password plus the stored salt and
decrypts the record. Decryption counts as successful when the plaintext starts
with `Readable`; the username is then simply taken from the form — the crypto
proves the password, not the name.

**Creating** generates a fresh 16-byte salt, derives a key from it and the
password, and encrypts the user data under that key. The salt and the
ciphertext are stored; the password never is.

If there is no record, or the record does not decrypt (wrong password, foreign
record, corrupted blob), the app asks **"User not found. Create?"**. Answering
yes creates a record with the password just typed and signs in with it —
**which replaces whatever record was there**, so a mistyped password answered
with "yes" discards the old account. That is the intended prototype behaviour;
copy the record out first if you care about it.

The *Encrypted user data* panel shows the record as one portable JSON blob
(salt and ciphertext together — a blob without its salt cannot be decrypted
anywhere). Copy it to move the account to another device; paste one in — with
the button or straight into the box — to adopt it here. Adopting a record does
not sign you in; the password still has to unlock it.

To get back to the "no user data" state, clear the site data (DevTools →
Application → Local storage, or `localStorage.clear()` in the console).

### Key derivation

`prototype-encrypted-userdata` derives its key with Argon2id from `hash-wasm`,
which means a CDN download on first run. This prototype is offline-first and
has no external dependency, so it does the same salt-plus-password derivation
with **PBKDF2-SHA256, 310 000 iterations** from WebCrypto, then AES-GCM-256
over the plaintext with a random 12-byte nonce. Argon2id is the better choice
for a real client; swapping it in means changing `KDF` in `app.js` and
re-creating the record.

Being a prototype, this is deliberately simple in ways a real client would not
be: there is a single record rather than one per user, the restored session
trusts `localStorage` instead of re-deriving the key, and the decrypted
plaintext is not used for anything.

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

1. Press **Sign in** and submit with an empty password — a pop-up says the
   password can not be empty. Same for an empty username.
2. Sign in with any username and password. With no record stored yet the app
   asks *"User not found. Create?"*; answer yes and it creates one, signs you
   in, and shows the encrypted record.
3. Sign out and sign back in with the **same** password — no prompt. Try a
   **different** password — *"User not found. Create?"* again; answer no and
   you stay signed out with the record untouched.
4. Paste the sample message below into **Paste roster**. The two cards should
   fill in with the two dates, the weekday, and the player count.
5. Tap **Not going** on either card. Your username is appended into the first
   empty slot (or a new slot if all are full) and the *Updated roster* box
   rewrites itself. Tap **Going** to remove your name.
6. Tap **Copy to clipboard** — paste the result into another chat as your
   reply.
7. Reload. You stay signed in (the session is remembered); the record shows as
   *stored, locked* because unlocking it again would need the password. The
   toggles pick up your going status from whatever is currently pasted.
8. Copy the record from **Encrypted user data**, clear local storage, paste it
   back, and sign in with the original password — the same account is back.

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

- `index.html` — markup, styling, and the sections (login, account, user-data
  record, agenda, paste).
- `app.js` — user-data encryption and session handling, form handling, roster
  parser and rewriter, toggle logic, clipboard glue, service-worker
  registration.
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
- The crypto needs a secure context (`https://`, `http://localhost`, or a
  `file://` page): without `crypto.subtle` the sign-in reports a failure.
- Key derivation takes a moment (310 000 PBKDF2 iterations); the sign-in form
  disables itself and its button reads *Working…* while it runs.
- The Clipboard read requires user activation and a secure context; on
  browsers without `navigator.clipboard.readText` (or when it's denied) the
  button surfaces an error and the user can still paste manually into the
  textarea.
