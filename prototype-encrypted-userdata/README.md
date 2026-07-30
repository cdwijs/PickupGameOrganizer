# Encrypted Userdata Prototype

An interactive, single-file walkthrough of **password-derived envelope
encryption**: what a client actually does when it turns a username / password
into a key that decrypts a shared-storage record, without ever sending the
password to the server.

Same flow used by password managers (Bitwarden, 1Password, Standard Notes)
and by cloud KMS envelope-encryption schemes.

## Run

Open `index.html` in a modern browser. Chromium, Firefox, and Safari all work.
No build step. First load fetches `hash-wasm` from a CDN (~200 KB) for Argon2id;
after that the page can be used offline until the cache expires.

## What the page does

Every step has its own editable field and a **Calculate** button, so you can
run one step at a time and see the intermediate value (all bytes shown as hex).

### Account creation

1. `password` — the master password (UTF-8).
2. `salt = random(16)` — a fresh 16-byte random salt.
3. `KEK = Argon2id(password, salt)` — the Key Encryption Key.
4. `DEK = random(32)` — a fresh 32-byte Data Encryption Key.
5. `wrapped_DEK = AES-GCM(KEK, DEK)` — the DEK, encrypted under the KEK.
6. `blob = AES-GCM(DEK, plaintext)` — the user data, encrypted under the DEK.

Then **Push** `{ salt, wrapped_DEK, blob }` to the simulated server panel.
Those three items are the only things stored server-side — no password, no
KEK, no DEK.

### Decryption (login)

1. `fetch(salt, wrapped_DEK, blob)` — pull the record from the server panel.
2. `entered_password` — the password typed at login.
3. `KEK = Argon2id(entered_password, salt)` — re-derive the KEK.
4. `DEK = AES-GCM-decrypt(KEK, wrapped_DEK)` — unwrap. Fails cleanly with
   an authentication error if the password is wrong.
5. `data = AES-GCM-decrypt(DEK, blob)` — the plaintext.

## Try this

- **Wrong password** — change one character in `entered_password` and press
  Calculate on step D2, then D3. D3 fails with an AES-GCM auth error; there's
  no way to distinguish "wrong password" from "tampered ciphertext" — that's
  by design.
- **Tampered blob** — flip a hex nibble in `serverBlob`, re-fetch, and run
  the whole decrypt. D3 succeeds (the wrapped_DEK is intact), D4 fails.
- **Rotate the password without re-encrypting the blob** — change the
  password, re-derive KEK, but keep the same DEK. Re-wrap the DEK (step 5).
  The big blob is untouched.
- **Wrong salt** — every random salt produces a different KEK, so two users
  with the *same* password get different wrapped_DEKs; precomputed rainbow
  tables don't work.

## Persistence

Every field is autosaved to `localStorage` under the key
`prototype-encrypted-userdata:v1` and reloaded on refresh. Two buttons at
the top:

- **Reset to defaults** — put the example password and plaintext back;
  clear all derived / stored values.
- **Clear localStorage** — wipe the saved state entirely.

## Argon2id parameters

The page uses browser-friendly settings (m = 19 MiB, t = 2, p = 1, out = 32).
Production clients should go higher — password managers commonly use 64–256
MiB of memory. The parameter block is a single constant near the top of the
inline script in `index.html`.

## Files

- `index.html` — the entire prototype (HTML + CSS + JS in one file).
