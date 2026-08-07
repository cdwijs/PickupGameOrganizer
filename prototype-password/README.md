# Password Prototype

A minimal installable PWA whose main job is to demonstrate what it takes for a
browser to recognize a form as a login form and offer to save (or auto-fill)
the username and password.

No build step, no bundler, no server dependency. Everything runs in the page.

## What the browser looks for

The three ingredients that make Chrome/Firefox/Safari offer "save password?":

1. **A real `<form>`** containing a text/email input and a password input.
2. **Correct `autocomplete` attributes** on the inputs:
   - `username` on the username field, `current-password` on the password
     field for a login form
   - `new-password` on the password field for a sign-up / password-change form
3. **A submit that the browser can attribute to the form** — either a real
   page navigation, or (in Chromium) an explicit
   `navigator.credentials.store(new PasswordCredential(...))` call.

The forms in `index.html` set all three up. On submit, the JS in `app.js`
calls `navigator.credentials.store()` so the browser is explicitly asked to
save. In Firefox/Safari, where `PasswordCredential` is not available, the
correct autocomplete attributes are enough for the browser's own form-save
heuristic to trigger.

## Run

Serve the folder over HTTP:

```sh
cd prototype-password
python3 -m http.server 8080
# then open http://localhost:8080 in a modern browser
```

`http://localhost` counts as a secure context, so password saving works there.
On a phone, `http://<lan-ip>:8080` will **not** trigger save — the browser
requires HTTPS off-localhost. See
[`prototype-qr-scanner/README.md`](../prototype-qr-scanner/README.md) for how
to get HTTPS on the LAN.

## What to try

1. Open the app, fill in any username and password, hit **Sign in**. The
   browser should surface a "save password?" prompt.
2. Reload the page. Focus the username field — the browser should offer to
   auto-fill from what you just saved.
3. Switch to **Create account**. The password field is tagged
   `autocomplete="new-password"`, which triggers the password-generator
   suggestion in Chrome/Safari instead of the auto-fill picker.
4. Click **Try `navigator.credentials.get()`**. In Chromium the browser will
   offer any credential it has saved for this origin and populate the form.

## Files

- `index.html` — sign-in and sign-up forms with correct autocomplete markup.
- `app.js` — form submit handlers, `PasswordCredential` calls, environment
  probe, service-worker registration.
- `manifest.json` — PWA manifest; makes the page installable.
- `sw.js` — cache-first service worker over the app shell. Bump `CACHE` when
  any shell file changes.
- `icon.svg` — padlock icon.

## Notes and limitations

- Nothing is sent to a server; the "sign in" is a client-only display of what
  the form submitted. Real login flows must additionally send the credentials
  to a backend over HTTPS.
- `PasswordCredential` is Chromium-only at time of writing. Firefox and Safari
  users still get the ordinary "save password?" prompt through the form-save
  heuristic — the autocomplete attributes are the load-bearing part there.
- The password confirmation on sign-up is validated in the page. HTML has no
  built-in `matches-other-field` validator; the check lives in `app.js`.
- Because the form calls `preventDefault()` and never navigates, the browser
  cannot use its "form submitted, page changed" heuristic; the explicit
  `navigator.credentials.store` call is what makes the save prompt reliable
  in a single-page PWA.
