# prototype-webassembly

`prototype-minimal`, feature for feature, with every rule and every
cryptographic primitive written in C and compiled to WebAssembly.

Same sign-in, same vault of password-encrypted blobs, same `[group1]` ECDSA
keypair, same roster parser, same 🔍🐛 debug fold, same two game cards. What
changed is where the work happens: `app.js` no longer knows what a roster is,
what a plaintext looks like, or how AES works.

## The split

**In C** (`src/`, 47 kB of wasm):

| | |
|---|---|
| `sha256.c` | SHA-256, HMAC-SHA256, PBKDF2 |
| `aesgcm.c` | AES-256, GHASH, GCM |
| `p256.c` | 256-bit modular arithmetic, P-256 curve arithmetic, keygen, DER |
| `roster.c` | the roster grammar, rewriter, `(app)` names, weekday, time |
| `json.c` | a small JSON parser for the vault |
| `app.c` | plaintext format, `[group1]`, the vault and its merge rules, sign-in |
| `base.c` | arena allocator, `memcpy`, string builder, hex |

**In JavaScript** — only what a wasm module cannot reach: the DOM, events,
`localStorage`, the clipboard, the credential store, the service worker, and
`crypto.getRandomValues`, because the module has no entropy of its own and is
handed bytes instead.

`crypto.subtle` is **not used at all**.

## Building

No Emscripten, no libc, no dependency of any kind — clang can target wasm32 on
its own:

```sh
./build.sh          # -> app.wasm
./build.sh -g       # unoptimised, names kept
```

It needs `clang` and `wasm-ld`; on Debian both come with the LLVM packages
(`/usr/lib/llvm-20/bin` is added to `PATH` by the script). The flags that
matter are `--target=wasm32 -nostdlib -Wl,--no-entry -Wl,--export-dynamic`.

**`app.wasm` is committed.** It is a shipped web asset rather than a build
artifact: the other prototypes here are all zero-build, and without it a fresh
checkout cannot be served to a phone. Rebuild it whenever `src/` changes.

## Testing

```sh
./tests/run-tests.sh
```

Five suites, each checking the C against something that is not the C:

| Suite | Checked against |
|---|---|
| `sha.test.mjs` | NIST vectors, RFC 4231, then `node:crypto` on random inputs — including the real 310 000-iteration PBKDF2 |
| `aes.test.mjs` | FIPS-197 C.3, then `createCipheriv` both directions, plus a flipped tag bit |
| `p256.test.mjs` | `d·G` against keys Node generated, edge scalars, and DER imported into WebCrypto to sign and verify |
| `app.test.mjs` | the account and vault rules, and **both directions of interop** with prototype-minimal's blob format |
| `roster.test.mjs` | prototype-minimal's *own JavaScript*, lifted out of its `app.js` and run on the same inputs |

That last one is a differential test rather than a restatement of the
behaviour: 99 comparisons of parse, weekday, count, going, toggle and the
rewritten text, byte for byte. It caught the two real bugs in the port — an
extra newline after a block whose player lines ran to the end of the text, and
CRLF input, which the JS silently normalises to LF and the C originally
preserved.

Harnesses build into `/shared/tmp/prototype-webassembly` (override with
`$WASM_TEST_BUILD`), so nothing generated lands in the tree.

## Speed

| | |
|---|---|
| PBKDF2, 310 000 iterations | ~320 ms |
| ECDSA P-256 keypair | ~0.5 ms |

The derivation is the one you feel: WebCrypto does the same work in native code
in a fraction of that, so signing in here is visibly slower than in
`prototype-minimal` — and sign-in pays it once per stored blob, because the
username is inside the ciphertext and there is nothing public to look up on.
This is a plain portable implementation with no SIMD and no assembly. The one
optimisation it does make is to hash the two padded HMAC key blocks once and
copy the state per iteration, since the key is the password throughout: two
SHA-256 compressions per iteration instead of four, which is where the earlier
~560 ms went.

## The interface

Strings in linear memory. JS asks the module for arena space, writes UTF-8 with
a NUL, calls an export, and reads a NUL-terminated result back:

```js
W.wasm_reset();                       // every operation starts from empty
const hit = JSON.parse(str(W.account_signin(put(vault), put(user), put(pass))));
```

The arena is a bump allocator over a fixed 24 MB block; nothing is freed
individually and no result survives the next reset, so anything worth keeping
is decoded into a JS string immediately. The vault is one opaque JSON string
that only the module ever looks inside.

## Interoperable with prototype-minimal

Same PBKDF2-SHA256 at 310 000 iterations, same AES-256-GCM with the nonce
prefixed, same plaintext and `[group1]` layout, same record shape. A vault
copied out of one prototype pastes into the other and opens. Both directions
are covered in `app.test.mjs`.

The `localStorage` keys are deliberately *different*
(`prototype-webassembly:*`), so both can be open on the same device without
fighting over one vault.

## Caveats worth reading

- **The P-256 code is not hardened.** Scalar multiplication is
  double-and-always-add with a constant-time select, so the result choice does
  not branch on key bits — but point addition still branches on the infinity
  case, there is no scalar blinding, and nothing defends against what a cache
  or an optimising compiler does. It is correct, and it is a prototype
  generating a throwaway key. Do not lift it into anything real.
- **`capitalizeName` covers less alphabet than JavaScript does.** The JS calls
  `toUpperCase()`, which knows every script. The C raises ASCII and the Latin-1
  letters (`é` → `É`) that turn up in names here, and leaves anything else
  alone. This is the one deliberate behavioural difference from
  `prototype-minimal`.
- The module is loaded with `WebAssembly.instantiate` over `fetch`, not
  `instantiateStreaming`, because streaming insists on an `application/wasm`
  content type that throwaway static servers do not always send.
- Nothing works before the module loads, so the account button stays disabled
  until it does, and a failed load says so rather than looking broken.

## Running it

```sh
cd ai-trash/prototypes/prototype-webassembly
python3 -m http.server 8080
# then open http://localhost:8080
```

`http://localhost` is a secure context, so the clipboard, password saving and
the service worker all work. On a phone it needs HTTPS — the LAN certificate
setup is the same one `prototype-qr-scanner/` documents.

## Files

- `index.html` — markup and styling, unchanged from `prototype-minimal` apart
  from the module status line.
- `app.js` — the shell: DOM, storage, clipboard, credentials, and the string
  bridge into wasm.
- `app.wasm` — the compiled module (committed, see above).
- `src/` — the C.
- `tests/` — the five suites and their harnesses.
- `build.sh`, `tests/run-tests.sh`.
- `sw.js` — network-first service worker; caches `app.wasm` alongside the rest
  of the shell.
- `manifest.json`, `icon.svg`.
