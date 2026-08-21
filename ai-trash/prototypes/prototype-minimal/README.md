# Minimal Prototype

A single-page PWA that stitches together ideas from the other prototypes:

- A login form that the browser recognizes and offers to save (pattern from
  [`prototype-password/`](../prototype-password/)), backed by a local vault of
  password-encrypted user blobs (pattern from
  [`prototype-encrypted-userdata/`](../prototype-encrypted-userdata/)).
- Two compact agenda cards (styling from
  [`mockups/agenda-view/`](../mockups/agenda-view/)) whose date, weekday and
  player count are filled in from the roster the user pastes.
- A paste box that parses a Terrible Football roster message and an output
  box that regenerates it with the user's name added or removed — with
  clipboard read/write buttons and a *Going* / *Not going* toggle per game.

## Accounts

There is no server. Every account is one encrypted blob in a `localStorage`
vault, and the vault can hold as many as you like:

| key | contents |
|-----|----------|
| `prototype-minimal:users:v2` | `[{ id, saltHex, dataHex }, …]` — one entry per user |
| `prototype-minimal:session:v2` | `{"id": …, "username": …}` — so a reload stays signed in |
| `prototype-minimal:debug:v1` | `"1"` / `"0"` — are the diagnostic panels folded open |

Each entry's `dataHex` is `nonce ‖ AES-GCM(key, plaintext)` over

```
Readable: <username>
This is a placeholder for the user data

[group1]
alg: ECDSA P-256
public: 3059301306072a8648ce3d0201…
private: 308187020100301306072a8648…
```

so **the username is inside the ciphertext**. Nothing stored in the clear says
who the blobs belong to — only how many there are. The `id` is a random public
label that exists so the session and the delete buttons can point at one blob.

**Signing in** tries every blob in the vault with the entered password. A blob
counts as the right one when it decrypts to a plaintext starting with
`Readable` *and* the username on that line matches the one typed (comparison is
case-insensitive; the stored spelling is the one the app then shows). Anything
else — unknown username, wrong password, someone else's password — reports the
same thing:

> Incorrect username or password.

and then offers to create a new user with the username and password just typed.
Answering yes appends a new blob; it never overwrites an existing one, so two
people can share a device, and even share a password.

Each blob has its own salt, so there is no shortcut: a sign-in costs one key
derivation per stored blob until it finds the match. Fine for the handful of
users a phone would hold — a real client would key the lookup on something
public.

**Creating** generates a fresh 16-byte salt, derives a key from it and the
password, generates the `group1` keypair below, and encrypts the username, the
user data and that keypair under the derived key. The salt and the ciphertext
are stored; the password never is, and neither is the keypair in any other
form.

### The group1 keypair

Anything after the body is a section: a `[name]` line followed by `key: value`
lines. `group1` is written once, when the account is created, and holds an
**ECDSA P-256** keypair — the public key as SPKI, the private key as PKCS8,
both hex like everything else stored here.

It is a signing key: it signs group content and verifies what other members
signed. It is not an encryption key, so anything signed with it stays readable
by whoever holds it. P-256 rather than Ed25519 because WebCrypto's Ed25519 is
recent in Gecko and may be missing on the Android browsers this prototype gets
tested on, where `generateKey` would throw instead of creating the account.

Nothing signs anything yet — the prototype only generates the keypair and puts
it in the blob, where a later version can pick it up.

Accounts created before this existed have no `group1` section and keep working
without one; only a freshly created account gets a keypair. Re-create the
account to give it one.

The *Decrypted user data* box below the vault shows the plaintext that came out
of the blob — marker line, username and all. It is filled only while this
session still holds the derived key, and the key is not part of the session, so
a reload starts out locked even though the session itself survives.

Two things fill the box again after a reload:

- On load the app asks the browser for a saved credential. When there is one for
  the signed-in user and it carries the password, the key is re-derived silently
  and the box is filled with no prompt. Chromium hands passwords back this way;
  Firefox and Safari do not, so there the box stays locked.
- Otherwise the box shows an **Unlock** button. It opens the sign-in form with
  the username filled in and the cursor in the password field; the right
  password re-derives the key for that one blob. A wrong one says *Incorrect
  password* and leaves the session alone — unlike a normal sign-in it never
  offers to create a user, because the account is known to exist here.

### Showing and hiding the panels

The **🔍🐛** button in the account row — between the status pill and
*Sign in* / *Sign out* — folds the *Encrypted user data* and *Decrypted user
data* panels away as one unit, down to the hint under *Unlock*. Nothing else on
the page moves. They are what the prototype exists to show, but they are not
what an app would put in front of a user, so they start folded and the choice is
remembered in `localStorage`. The panels are kept up to date while folded, so
opening them never shows something stale.

### Moving accounts between devices

The *Encrypted user data* panel shows the whole vault as a JSON array. Copy it
to move accounts to another device; paste it — with the button or straight into
the box — to merge it in. Entries are merged, not replaced: a matching `id`
updates that entry, an identical salt/blob pair is ignored, anything else is
added. A single record object pastes in as well as an array.

Adopting a blob does not sign anyone in; the password still has to unlock it.
Clearing the box deletes nothing.

### Deleting

Two red buttons, both of which confirm first and neither of which can be undone
without a copy of the blob:

- **Delete this user** — removes the blob the current session has open, and
  signs out.
- **Delete all users** — empties the vault and signs out.

### Key derivation

`prototype-encrypted-userdata` derives its key with Argon2id from `hash-wasm`,
which means a CDN download on first run. This prototype is offline-first and
has no external dependency, so it does the same salt-plus-password derivation
with **PBKDF2-SHA256, 310 000 iterations** from WebCrypto, then AES-GCM-256
over the plaintext with a random 12-byte nonce. Argon2id is the better choice
for a real client; swapping it in means changing `KDF` in `app.js` and
re-creating the blobs.

Being a prototype, this is deliberately simple in ways a real client would not
be: the restored session trusts `localStorage` for who is signed in instead of
re-deriving the key (the key is only re-derived to fill the decrypted box), and
the decrypted user data is not used for anything.

An earlier version stored a single blob under `…:salt:v1` / `…:userdata:v1`
with no username inside it. Nothing here can open those (there is no way to
learn the username without the password), so they are deleted on load.

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

1. Press **🔍🐛** in the account row. The *Encrypted user data* and *Decrypted
   user data* panels fold open — the rest of the walkthrough refers to them.
   Press it again to fold them away; the choice survives a reload.
2. Press **Sign in** and submit with an empty password — a pop-up says the
   password can not be empty. Same for an empty username.
3. Sign in as `Cedric` with any password. The vault is empty, so the app says
   *"Incorrect username or password."* and then offers to create that user;
   accept, and it appears in the panel as one blob.
4. Sign out and add `Teize` with a **different** password, then `Alex` with
   **Cedric's** password. Three blobs, three separate accounts.
5. Sign back in as each of them — no prompts. Try `Cedric` with Teize's
   password: *"Incorrect username or password."* Decline the create offer and
   nothing changes.
6. Note that `cedric`, `CEDRIC` and `Cedric` all sign in, and the app shows the
   spelling the account was created with.
7. Paste the sample message below into **Paste roster**. The two cards should
   fill in with the two dates, the weekday, and the player count.
8. Tap **Not going** on either card. Your username is appended into the first
   empty slot (or a new slot if all are full) and the *Updated roster* box
   rewrites itself. Tap **Going** to remove your name.
9. Tap **Copy to clipboard** — paste the result into another chat as your
   reply.
10. Watch **Decrypted user data** while you switch accounts — it shows that
    user's plaintext, `Readable: <username>` line included, and clears on sign
    out.
11. Reload. You stay signed in (the session is remembered). If the browser
    saved the password, the decrypted box fills itself again with no prompt.
    Otherwise the vault pill drops its *unlocked* note, the decrypted box is
    empty with a *locked* pill, and an **Unlock** button appears under it —
    press it and enter the password to get the plaintext back. Get the password
    wrong and it says *Incorrect password*; you stay signed in and no second
    account is created.
12. Copy the vault, press **Delete this user**, then paste the vault back — the
    deleted account returns and unlocks with its original password.
13. Press **Delete all users**. The vault empties, you're signed out, and both
    red buttons go grey.

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
3. Alex

__________________________

🗓️ Monday 10.08.2026

01. Teize
3. Amine
03.
04.

⚽ Football pools, 🍖 barbecues, 🧠 pub quizzes and plenty of other terrible ideas. Join the fun in our Terrible Offtopic group: 👉 https://tinyurl.com/TF-offtopic
```

## Files

- `index.html` — markup, styling, and the sections (login, account, encrypted
  user data, decrypted user data, agenda, paste). The two user-data sections
  sit together in `#debug-panel`, which the 🔍🐛 button folds away.
- `app.js` — the encrypted-blob vault, sign-in and session handling, form
  handling, roster parser and rewriter, toggle logic, clipboard glue,
  service-worker registration.
- `manifest.json` — PWA manifest; makes the page installable.
- `sw.js` — service worker over the app shell. Network-first: the network wins
  whenever it answers and its response refreshes the cache, so a reload always
  runs the current code. The cache is the offline fallback only. It was
  cache-first, which served a changed shell stale for at least one load — and a
  fresh `index.html` next to a stale `app.js` looks like a dead button, not a
  caching problem. Bumping `CACHE` still clears the old generation.
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
- Key derivation takes a moment (310 000 PBKDF2 iterations, once per stored
  blob until one matches); the sign-in form disables itself and its button
  reads *Working…* while it runs.
- The decrypted plaintext is never persisted: it lives in memory for the
  current page load only, which is why a reload starts out locked and the key
  has to come from a saved credential or from **Unlock**.
- The `group1` private key is shown in the clear in the *Decrypted user data*
  box, because that box shows the plaintext verbatim. That is the point of the
  panel; the key is only ever *stored* inside the encrypted blob.
- Two accounts can hold the same username as long as their passwords differ —
  the app cannot see a clash it has no key for. Whichever blob the password
  opens is the one you get.
- The Clipboard read requires user activation and a secure context; on
  browsers without `navigator.clipboard.readText` (or when it's denied) the
  button surfaces an error and the user can still paste manually into the
  textarea.
