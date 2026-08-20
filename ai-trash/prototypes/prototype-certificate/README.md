# Signing Prototype

A minimal installable Progressive Web App that walks through the eight steps of
public-key message signing, one button at a time, and shows how long each step
took. No build step, no bundler, no network dependency at runtime — everything
uses the browser's built-in `SubtleCrypto`.

The eight steps:

1. **Generate key pair** — `crypto.subtle.generateKey` on curve P-256
2. **Export public key** — SPKI DER, base64
3. **Export private key** — PKCS#8 DER, base64 (demonstration only)
4. **Encode message** — `TextEncoder` (UTF-8)
5. **Hash message** — `crypto.subtle.digest('SHA-256', …)`
6. **Sign** — `crypto.subtle.sign` (ECDSA/SHA-256)
7. **Verify** — `crypto.subtle.verify` — expected `true`
8. **Tamper test** — flip one bit of the signature and verify again — expected `false`

Each button times its step with `performance.now()` and writes the result into
the pill in the section header plus the timing summary at the bottom of the
page.

## Run

Serve the folder over HTTP (a service worker won't register from `file://`, and
`SubtleCrypto` needs a secure context):

```sh
cd prototype-certificate
python3 -m http.server 8080
# then open http://localhost:8080 in a modern browser
```

`http://localhost` counts as a secure context, so `SubtleCrypto` works there.
On a phone, `http://<lan-ip>:8080` will **not** work — `SubtleCrypto` returns
`undefined` outside a secure context. See the sibling
[`prototype-qr-scanner/README.md`](../prototype-qr-scanner/README.md) for
end-to-end HTTPS options; they apply here unchanged.

## Algorithm choice

Two selects at the top of the page pick the primitives:

- **Signature algorithm** — ECDSA (P-256, P-384, P-521), RSA-PSS (2048/3072/4096),
  RSASSA-PKCS1-v1_5 (2048), Ed25519. RSA-4096 keygen takes a few hundred ms and
  makes the timing summary noticeably fatter.
- **Hash algorithm** — SHA-256, SHA-384, SHA-512, SHA-1 (legacy, disabled for
  new use). Ed25519 ignores this — it uses SHA-512 internally regardless.

Changing either select clears all downstream state (keys, message hash,
signature) and re-enables *Generate*, since RSA binds the hash at generation
time and it is easier to reason about a fresh pipeline than a mix of old and
new material.

The default is **ECDSA/P-256 + SHA-256** — universally supported and a compact
64-byte IEEE-P1363 signature.

Ed25519 landed in Chromium 137 and recent Firefox/Safari; the option is
feature-detected on load and disabled if `crypto.subtle.generateKey` rejects.

## Interpreting the timings

Each pill shows the wall-clock duration of one call, measured with
`performance.now()`. A few caveats to know when reading them:

- **Sub-millisecond steps** — the encode step and (on some machines) the hash
  and verify steps land under 1 ms; they are shown as `NNN µs`.
- **Timer resolution.** Cross-origin isolation and browser hardening cap the
  resolution of `performance.now()` at 5 µs–1 ms depending on the browser and
  context. The `µs` value is real but noisy at that scale.
- **JIT / thermal noise.** The first run of each step warms caches and paths
  in the JS engine; a second run often reports lower numbers. Rerun a few
  times before concluding "this is what it costs".
- **Sign vs. verify.** For ECDSA on P-256, verify is typically a bit slower
  than sign on desktop, and often quite a lot slower than sign on mobile —
  the app makes the asymmetry visible.

## Security note

The private key is generated with `extractable: true` so the *Export private
key* step has something to show. In real code you would generate with
`extractable: false` and keep the `CryptoKey` handle in IndexedDB — that way
the private key material never appears in JavaScript memory that the page (or
an XSS payload) can read.

## Files

- `index.html` — markup and CSS. Eight sections, one per step; a summary table;
  a log.
- `app.js` — `SubtleCrypto` calls, per-step timing wrapper, button wiring.
- `manifest.json` — PWA manifest, makes the page installable.
- `sw.js` — service worker; cache-first over the whole app shell. Bump `CACHE`
  when any shell file changes.
- `icon.svg` — app icon.

## Known limitations

- Nothing is persisted. Reloading the page clears the key pair, signature and
  log. A future revision could stash the (non-extractable) `CryptoKey` in
  IndexedDB.
- The tamper test flips a single bit of the signature; it does not also
  demonstrate the mirror case (flip a bit of the message and re-verify against
  the untouched signature). Both should fail; only one is wired to a button.
- Signature encoding is IEEE P1363 (raw `r‖s`), which is what `SubtleCrypto`
  produces natively. Interop with OpenSSL / X.509 tools requires wrapping into
  an ASN.1 DER `Ecdsa-Sig-Value` — not done here.
