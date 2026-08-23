// prototype-minimal, with the thinking moved into WebAssembly.
//
// Nothing here parses a roster, formats a plaintext, merges a vault or touches
// a cipher. All of that is in src/*.c, compiled to app.wasm. What is left is
// the part a wasm module genuinely cannot do: the DOM, localStorage, the
// clipboard, the credential store, the service worker — and entropy, which
// comes from crypto.getRandomValues and is handed to the module as bytes.
//
// The interface is strings in linear memory. JS writes UTF-8 with a NUL
// terminator into arena space the module hands out, calls an export, and reads
// a NUL-terminated result back. The arena is reset before every operation, so
// no result outlives the next call — anything worth keeping is copied into a
// JS string immediately, which reading it as UTF-8 does anyway.

const $ = (id) => document.getElementById(id);

// ---- the module ----------------------------------------------------------

let W = null;              // exports, once loaded
const textEnc = new TextEncoder();
const textDec = new TextDecoder();

function bytes() { return new Uint8Array(W.memory.buffer); }

// Copy a JS string into the arena as NUL-terminated UTF-8.
function put(s) {
  const b = textEnc.encode(s === undefined || s === null ? '' : String(s));
  const ptr = W.wasm_alloc(b.length);
  if (!ptr) throw new Error('the module is out of arena space');
  const mem = bytes();
  mem.set(b, ptr);
  mem[ptr + b.length] = 0;
  return ptr;
}

function putBytes(b) {
  const ptr = W.wasm_alloc(b.length);
  if (!ptr) throw new Error('the module is out of arena space');
  bytes().set(b, ptr);
  return ptr;
}

// Read a NUL-terminated UTF-8 string back out.
function str(ptr) {
  if (!ptr) return '';
  const mem = bytes();
  let end = ptr;
  while (mem[end]) end++;
  return textDec.decode(mem.subarray(ptr, end));
}

const json = (ptr) => JSON.parse(str(ptr));

// Every operation starts from an empty arena. Results are decoded into JS
// strings before the next reset, which is why each helper resets on entry
// rather than on exit.
function op(fn) {
  W.wasm_reset();
  return fn();
}

// The module needs its own randomness for a new account: an id, a salt, a
// nonce and the candidate scalars for the keypair.
function randomFor(n) {
  const b = new Uint8Array(n);
  crypto.getRandomValues(b);
  return b;
}

async function loadModule() {
  // Deliberately not instantiateStreaming: it insists on an
  // application/wasm content type, and the throwaway static servers used to
  // test this — python3 -m http.server among them — do not always send one.
  const res = await fetch('./app.wasm');
  if (!res.ok) throw new Error(`app.wasm: HTTP ${res.status}`);
  const buf = await res.arrayBuffer();
  const { instance } = await WebAssembly.instantiate(buf, {});
  W = instance.exports;
  if (W.__wasm_call_ctors) W.__wasm_call_ctors();
  return buf.byteLength;
}

// ---- storage -------------------------------------------------------------

// A separate namespace from prototype-minimal so both can be open at once,
// even though the blob format is identical and a vault pastes across.
const LS_VAULT = 'prototype-webassembly:vault:v1';
const LS_SESSION = 'prototype-webassembly:session:v1';
const LS_DEBUG = 'prototype-webassembly:debug:v1';

function lsGet(key) {
  try { return localStorage.getItem(key); } catch { return null; }
}
function lsSet(key, value) {
  try { localStorage.setItem(key, value); return true; } catch { return false; }
}
function lsRemove(key) {
  try { localStorage.removeItem(key); } catch { /* nothing to do */ }
}

// The vault is one JSON string, opaque to this file: only the module reads
// inside it.
function loadVault() { return lsGet(LS_VAULT) || '[]'; }
function saveVault(v) { return lsSet(LS_VAULT, v); }

function saveSession(id, username) {
  lsSet(LS_SESSION, JSON.stringify({ id, username }));
}

function readSession() {
  const raw = lsGet(LS_SESSION);
  if (!raw) return null;
  try {
    const obj = JSON.parse(raw);
    if (!obj || typeof obj.username !== 'string' || !obj.username) return null;
    if (typeof obj.id !== 'string' || !obj.id) return null;
    return obj;
  } catch {
    return null;
  }
}

function clearSession() { lsRemove(LS_SESSION); }

// ---- state ---------------------------------------------------------------

const state = {
  raw: '',
  blocks: [],       // what the module reports about each date block
  username: '',
  userId: '',
  userData: '',     // decrypted plaintext, only while this page holds the key
  debug: lsGet(LS_DEBUG) === '1',
};

const mainView = $('main-view');
const signinView = $('signin-view');
const signinForm = $('signin-form');
const usernameInput = $('signin-username');
const passwordInput = $('signin-password');
const accountStatus = $('account-status');
const accountBtn = $('account-btn');
const signinSubmit = $('signin-submit');
const wasmStatus = $('wasm-status');
const pasteIn = $('paste-in');
const pasteOut = $('paste-out');
const parseStatus = $('parse-status');
const outStatus = $('out-status');
const userdataBox = $('userdata-box');
const userdataStatus = $('userdata-status');
const deleteUserBtn = $('delete-user');
const deleteAllBtn = $('delete-all');
const plainBox = $('plain-box');
const plainStatus = $('plain-status');
const plainUnlockBtn = $('plain-unlock');
const debugPanel = $('debug-panel');
const debugBtn = $('debug-btn');
const PLAIN_PLACEHOLDER = plainBox.placeholder;

function setPill(el, text, kind) {
  el.className = kind ? `pill ${kind}` : 'pill';
  el.textContent = text;
}

// ---- module-backed operations -------------------------------------------

function vaultCount() { return op(() => W.vault_count(put(loadVault()))); }
function vaultHas(id) { return op(() => W.vault_has(put(loadVault()), put(id)) === 1); }
function vaultText() { return op(() => str(W.vault_text(put(loadVault())))); }

function displayName(username) {
  return op(() => str(W.display_name(put(username))));
}

// The module owns the parse; this side keeps only what the cards render from.
function parseRoster() {
  op(() => {
    const n = W.roster_parse(put(state.raw), put(state.username));
    state.blocks = [];
    for (let i = 0; i < n; i++) state.blocks.push(json(W.roster_info(i)));
    // The rewritten text comes back in the same arena pass: normalising the
    // user's slots can change it even when nothing was toggled.
    state.out = str(W.roster_out());
  });
}

function toggleBlock(idx) {
  op(() => {
    W.roster_parse(put(state.raw), put(state.username));
    W.roster_flip(idx);
    state.raw = str(W.roster_out());
    const n = W.roster_parse(put(state.raw), put(state.username));
    state.blocks = [];
    for (let i = 0; i < n; i++) state.blocks.push(json(W.roster_info(i)));
    state.out = str(W.roster_out());
  });
}

// ---- views + auth state --------------------------------------------------

function showView(name, { focus = 'username' } = {}) {
  const showSignin = name === 'signin';
  mainView.hidden = showSignin;
  signinView.hidden = !showSignin;
  if (showSignin) {
    const field = focus === 'password' ? passwordInput : usernameInput;
    setTimeout(() => field.focus(), 0);
  }
}

function renderAccount() {
  if (state.username) {
    accountStatus.className = 'pill ok';
    accountStatus.textContent = `signed in as ${state.username}`;
    accountBtn.textContent = 'Sign out';
  } else {
    accountStatus.className = 'pill';
    accountStatus.textContent = 'not signed in';
    accountBtn.textContent = 'Sign in';
  }
}

async function storeCredential(username, password) {
  if (typeof PasswordCredential === 'undefined' || !navigator.credentials?.store) return;
  try {
    await navigator.credentials.store(new PasswordCredential({
      id: username, password, name: username,
    }));
  } catch { /* browser will still fall back to its form-save heuristic */ }
}

// PBKDF2 at 310 000 iterations costs about half a second here, and sign-in
// pays it once per stored blob, so the form is locked while it runs.
function setSigninBusy(busy) {
  signinSubmit.disabled = busy;
  signinSubmit.textContent = busy ? 'Working…' : 'Sign in';
  usernameInput.disabled = busy;
  passwordInput.disabled = busy;
}

function signOutLocal() {
  state.username = '';
  state.userId = '';
  state.userData = '';
  passwordInput.value = '';
  clearSession();
}

let unlockTargetId = '';

// The single sign-in path, same shape as prototype-minimal's: `interactive`
// separates a real submit from the silent attempt made on load, and `unlockId`
// names one blob to open rather than searching the vault.
async function attemptSignIn(username, password, { interactive, unlockId = '' }) {
  setSigninBusy(true);
  // Yield once so the disabled state paints before the module blocks the
  // thread — the derivation is synchronous inside wasm.
  await new Promise((r) => setTimeout(r, 0));
  try {
    let hit;
    if (unlockId) {
      hit = op(() => json(W.account_unlock(put(loadVault()), put(unlockId), put(password))));
      if (hit.error === 'gone') {
        signOutLocal();
        showView('main');
        render();
        if (interactive) alert('That user was deleted on this device.');
        return false;
      }
      if (hit.error) {
        if (interactive) alert('Incorrect password.');
        return false;
      }
    } else {
      hit = op(() => json(W.account_signin(put(loadVault()), put(username), put(password))));
    }

    if (hit.error) {
      if (!interactive) return false;
      alert('Incorrect username or password.');
      if (!confirm(`Create a new user "${username.trim()}" with this password?`)) return false;
      const rnd = randomFor(W.rnd_needed());
      const made = op(() => json(
        W.account_create(put(loadVault()), put(username.trim()), put(password), putBytes(rnd))
      ));
      if (made.error) {
        alert(`Could not create the user: ${made.error}`);
        return false;
      }
      if (!saveVault(made.vault)) {
        alert('Created, but this browser would not store it — copy the vault '
          + 'out of the user-data box, it is gone on reload.');
      }
      hit = made;
    }

    state.username = hit.username;
    state.userId = hit.id;
    state.userData = hit.text;
    saveSession(hit.id, hit.username);
    await storeCredential(hit.username, password);
    passwordInput.value = '';
    showView('main');
    render();
    return true;
  } catch (err) {
    if (interactive) alert(`Sign in failed: ${err.message || err}`);
    return false;
  } finally {
    setSigninBusy(false);
  }
}

signinForm.addEventListener('submit', async (evt) => {
  evt.preventDefault();
  const username = usernameInput.value.trim();
  const password = passwordInput.value;
  if (!username) {
    alert('Username can not be empty.');
    usernameInput.focus();
    return;
  }
  if (!password) {
    alert('Password can not be empty.');
    passwordInput.focus();
    return;
  }
  const ok = await attemptSignIn(username, password, {
    interactive: true, unlockId: unlockTargetId,
  });
  if (ok) unlockTargetId = '';
});

$('signin-cancel').addEventListener('click', () => {
  usernameInput.value = state.username;
  passwordInput.value = '';
  unlockTargetId = '';
  showView('main');
});

accountBtn.addEventListener('click', () => {
  if (state.username) {
    signOutLocal();
    render();
  } else {
    unlockTargetId = '';
    showView('signin');
  }
});

function restoreSession() {
  const session = readSession();
  if (!session) return false;
  if (!vaultHas(session.id)) {
    clearSession();
    return false;
  }
  state.username = session.username;
  state.userId = session.id;
  usernameInput.value = session.username;
  return true;
}

// Offer a saved credential on load. With a restored session this is what
// re-derives the key so the decrypted box is filled rather than locked; the
// credential has to belong to the restored user, or a reload would silently
// switch accounts.
async function tryPrefill() {
  if (!navigator.credentials?.get) return;
  try {
    const cred = await navigator.credentials.get({ password: true, mediation: 'optional' });
    if (!cred?.id) return;
    const restored = state.username;
    if (restored && cred.id.trim().toLowerCase() !== restored.toLowerCase()) return;
    if (!restored) usernameInput.value = cred.id;
    const password = 'password' in cred ? cred.password : '';
    if (password) passwordInput.value = password;
    if (password && vaultCount()) {
      await attemptSignIn(restored || cred.id, password, { interactive: false });
    }
  } catch { /* user dismissed, or no credential */ }
}

// ---- render --------------------------------------------------------------

function renderDebug() {
  debugPanel.hidden = !state.debug;
  debugBtn.setAttribute('aria-expanded', String(state.debug));
  debugBtn.classList.toggle('secondary', !state.debug);
}

function renderUserData() {
  const count = vaultCount();
  if (document.activeElement !== userdataBox) {
    userdataBox.value = count ? vaultText() : '';
  }
  userdataStatus.className = count ? 'pill ok' : 'pill';
  if (!count) {
    userdataStatus.textContent = 'empty';
  } else {
    const n = `${count} blob${count === 1 ? '' : 's'}`;
    userdataStatus.textContent = state.userData ? `${n} · unlocked` : n;
  }
  deleteUserBtn.disabled = !state.userId;
  deleteAllBtn.disabled = !count;

  plainBox.value = state.userData;
  plainStatus.className = state.userData ? 'pill ok' : 'pill';
  plainStatus.textContent = state.userData
    ? `${state.userData.length} chars`
    : (state.username ? 'locked' : '—');
  const locked = Boolean(state.username) && !state.userData;
  plainUnlockBtn.hidden = !locked;
  plainBox.placeholder = locked
    ? 'Locked. Press Unlock and enter your password to see the plaintext.'
    : PLAIN_PLACEHOLDER;
}

function render() {
  renderAccount();
  renderDebug();
  renderUserData();
  parseRoster();
  const blocks = state.blocks;

  if (!state.raw) {
    parseStatus.className = 'pill';
    parseStatus.textContent = 'empty';
    outStatus.className = 'pill';
    outStatus.textContent = '—';
  } else {
    parseStatus.className = blocks.length ? 'pill ok' : 'pill err';
    parseStatus.textContent = `${blocks.length} date block${blocks.length === 1 ? '' : 's'}`;
    outStatus.className = 'pill ok';
    outStatus.textContent = `${state.out.length} chars`;
  }

  // Two cards; extra blocks are ignored and a missing block resets its card,
  // the same as prototype-minimal.
  for (let i = 0; i < 2; i++) {
    const card = $(`game-${i}`);
    const whenEl = card.querySelector('[data-when]');
    const countEl = card.querySelector('[data-count]');
    const toggle = card.querySelector('[data-toggle]');
    const block = blocks[i];

    if (!block) {
      whenEl.textContent = '—';
      countEl.textContent = '0';
      card.classList.remove('is-going');
      toggle.className = 'status';
      toggle.textContent = 'Not going';
      toggle.disabled = true;
      continue;
    }

    whenEl.textContent = block.time ? `${block.weekday} · ${block.time}` : block.weekday;
    countEl.textContent = String(block.count);
    toggle.disabled = !state.username;
    if (block.going) {
      card.classList.add('is-going');
      toggle.className = 'status going';
      toggle.innerHTML = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" '
        + 'stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">'
        + '<path d="M5 12.5l4.2 4.3L19 6.5"/></svg><span>Going</span>';
    } else {
      card.classList.remove('is-going');
      toggle.className = 'status';
      toggle.textContent = 'Not going';
    }
  }

  pasteOut.value = state.raw ? state.out : '';
}

// ---- clipboard + panels --------------------------------------------------

pasteIn.addEventListener('input', () => {
  state.raw = pasteIn.value;
  render();
});

$('clear-in').addEventListener('click', () => {
  pasteIn.value = '';
  state.raw = '';
  render();
});

$('paste-btn').addEventListener('click', async () => {
  try {
    const text = await navigator.clipboard.readText();
    pasteIn.value = text;
    state.raw = text;
    render();
  } catch (err) {
    alert(`Clipboard read failed: ${err.message}`);
  }
});

$('copy-btn').addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText(pasteOut.value);
    setPill(outStatus, 'copied', 'ok');
    setTimeout(render, 1200);
  } catch (err) {
    alert(`Clipboard write failed: ${err.message}`);
  }
});

$('userdata-copy').addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText(userdataBox.value);
    setPill(userdataStatus, 'copied', 'ok');
    setTimeout(render, 1200);
  } catch (err) {
    alert(`Clipboard write failed: ${err.message}`);
  }
});

// Merging is the module's rule, not this file's: same id replaces, an
// identical salt and blob is a no-op, anything else is added.
function ingest(text) {
  if (!text.trim()) return;
  const rnd = randomFor(512);
  const res = op(() => json(W.vault_ingest(put(loadVault()), put(text), putBytes(rnd), 512)));
  if (res.error) {
    setPill(userdataStatus, 'bad paste', 'err');
    userdataBox.title = res.error;
    return;
  }
  userdataBox.title = '';
  saveVault(res.vault);
  render();
  if (res.added || res.replaced) {
    setPill(userdataStatus, `+${res.added} ~${res.replaced}`, 'ok');
    setTimeout(render, 1500);
  }
}

$('userdata-paste').addEventListener('click', async () => {
  try {
    const text = await navigator.clipboard.readText();
    userdataBox.value = text;
    ingest(text);
  } catch (err) {
    alert(`Clipboard read failed: ${err.message}`);
  }
});

userdataBox.addEventListener('input', () => ingest(userdataBox.value));

debugBtn.addEventListener('click', () => {
  state.debug = !state.debug;
  lsSet(LS_DEBUG, state.debug ? '1' : '0');
  render();
});

plainUnlockBtn.addEventListener('click', () => {
  usernameInput.value = state.username;
  passwordInput.value = '';
  unlockTargetId = state.userId;
  showView('signin', { focus: 'password' });
});

deleteUserBtn.addEventListener('click', () => {
  if (!state.userId) return;
  if (!confirm('Delete this user? The encrypted blob is removed from this '
    + 'device and cannot be recovered without a copy.')) return;
  const next = op(() => str(W.vault_delete(put(loadVault()), put(state.userId))));
  saveVault(next);
  signOutLocal();
  render();
});

deleteAllBtn.addEventListener('click', () => {
  const count = vaultCount();
  if (!count) return;
  if (!confirm(`Delete all ${count} user${count === 1 ? '' : 's'}? Every encrypted `
    + 'blob on this device is removed and cannot be recovered without a copy.')) return;
  saveVault('[]');
  signOutLocal();
  render();
});

document.querySelectorAll('[data-toggle]').forEach((btn) => {
  btn.addEventListener('click', () => {
    if (!state.username) return;
    const idx = Number(btn.closest('.game').dataset.block);
    if (!state.blocks[idx]) return;
    toggleBlock(idx);
    pasteIn.value = state.raw;
    render();
  });
});

// ---- boot ----------------------------------------------------------------

if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('./sw.js').catch(() => {});
  });
}

showView('main');

// Nothing on the page works before the module is in: every render asks it for
// the vault count. The controls stay disabled until it lands.
(async () => {
  accountBtn.disabled = true;
  try {
    const size = await loadModule();
    setPill(wasmStatus, `${(size / 1024).toFixed(1)} kB module`, 'ok');
  } catch (err) {
    setPill(wasmStatus, 'module failed', 'err');
    accountStatus.textContent = 'unavailable';
    alert(`The WebAssembly module did not load: ${err.message || err}\n\n`
      + 'Everything in this prototype runs inside it, so nothing works without it. '
      + 'Run build.sh if app.wasm is missing.');
    return;
  }
  accountBtn.disabled = false;
  restoreSession();
  render();
  tryPrefill().then(render);
})();
