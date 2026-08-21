// Minimal prototype. Login form (pattern from prototype-password) backed by a
// vault of password-encrypted user blobs (pattern from
// prototype-encrypted-userdata), two game cards, and a paste-and-parse
// pipeline that keeps the updated roster in sync with the toggles.
//
// Roster grammar (informal), from the example message:
//   header lines...
//   __________________________
//   🗓️ Friday 07.08.2026
//                                  (may be a blank line, then...)
//   01. Alice
//   02. Bob
//   03.
//   __________________________
//   🗓️ Monday 10.08.2026
//   01. …
//   trailing lines...
//
// The parser records where each date block's player lines start and end, so
// the rewriter can splice in an updated list without disturbing anything else.

const $ = (id) => document.getElementById(id);

// ---- user data: vault of password-encrypted blobs ------------------------

// One blob per user. Each blob is AES-GCM over this plaintext:
//
//   Readable: <username>
//   This is a placeholder for the user data
//
// The username lives *inside* the ciphertext, so nothing stored in the clear
// says who the blobs belong to. Signing in means trying every blob with the
// entered password: the one that decrypts to a plaintext starting with
// "Readable" and carrying the entered username is that user's blob. Anything
// else is an incorrect username or password — the two are indistinguishable
// from the outside, which is the point.
const READABLE_PREFIX = 'Readable';
const USER_DATA_BODY = 'This is a placeholder for the user data';

function makePlaintext(username) {
  return `${READABLE_PREFIX}: ${username}\n${USER_DATA_BODY}`;
}

// Split a decrypted plaintext into { username, body, text }, or null when it
// doesn't carry the marker — i.e. when this wasn't the right key after all.
// `text` is the plaintext verbatim, which is what the decrypted-data box shows.
function parsePlaintext(text) {
  const head = `${READABLE_PREFIX}: `;
  if (!text.startsWith(head)) return null;
  const nl = text.indexOf('\n');
  const username = (nl === -1 ? text.slice(head.length) : text.slice(head.length, nl)).trim();
  if (!username) return null;
  return { username, body: nl === -1 ? '' : text.slice(nl + 1), text };
}

// localStorage keys. The vault is one JSON array; the session names which blob
// is open. v1 stored a single salt/blob pair with no username inside it — that
// cannot be migrated (there is no way to learn the username without the
// password), so those keys are dropped on load.
const LS_USERS = 'prototype-minimal:users:v2';
const LS_SESSION = 'prototype-minimal:session:v2';
const LS_LEGACY = [
  'prototype-minimal:salt:v1',
  'prototype-minimal:userdata:v1',
  'prototype-minimal:session:v1',
];

// prototype-encrypted-userdata derives its KEK with Argon2id from hash-wasm,
// which means a CDN download on first run. This prototype is an offline-first
// PWA with no external dependency, so the same salt-plus-password derivation
// is done with PBKDF2-SHA256 from WebCrypto instead. Everything else matches:
// random salt per blob, AES-GCM over the plaintext, nonce prefixed to the
// ciphertext.
const KDF = { name: 'PBKDF2-SHA256', iterations: 310000, hash: 'SHA-256' };
const SALT_BYTES = 16;
const NONCE_BYTES = 12;

const textEnc = new TextEncoder();
const textDec = new TextDecoder();

function bytesToHex(bytes) {
  let out = '';
  for (const b of bytes) out += b.toString(16).padStart(2, '0');
  return out;
}

function hexToBytes(hex) {
  const clean = String(hex).trim().replace(/\s+/g, '');
  if (!clean) throw new Error('hex string is empty');
  if (clean.length % 2) throw new Error('hex string has an odd length');
  if (!/^[0-9a-f]+$/i.test(clean)) throw new Error('hex string has non-hex characters');
  const out = new Uint8Array(clean.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(clean.substr(i * 2, 2), 16);
  return out;
}

// localStorage throws in private-browsing modes and when the quota is full;
// every access is wrapped so the prototype degrades to a session-only vault.
function lsGet(key) {
  try { return localStorage.getItem(key); } catch { return null; }
}
function lsSet(key, value) {
  try { localStorage.setItem(key, value); return true; } catch { return false; }
}
function lsRemove(key) {
  try { localStorage.removeItem(key); } catch { /* nothing to do */ }
}

// A blob id only exists so the session and the delete buttons can point at one
// blob. It is public, unlike everything else about the user.
function newId() {
  return bytesToHex(crypto.getRandomValues(new Uint8Array(8)));
}

// ---- the vault -----------------------------------------------------------

// Records on disk: [{ id, saltHex, dataHex }, …]. Anything unparseable is
// dropped rather than thrown, so one bad entry can't lock the app out.
function loadUsers() {
  const raw = lsGet(LS_USERS);
  if (!raw) return [];
  try {
    const arr = JSON.parse(raw);
    if (!Array.isArray(arr)) return [];
    return arr.filter((r) => r && typeof r.saltHex === 'string' && typeof r.dataHex === 'string')
      .map((r) => ({ id: typeof r.id === 'string' && r.id ? r.id : newId(), saltHex: r.saltHex, dataHex: r.dataHex }));
  } catch {
    return [];
  }
}

// Returns false when the browser refused to store (private mode, quota) — the
// vault then only lasts for this page load, which the caller reports.
function saveUsers(users) {
  return lsSet(LS_USERS, JSON.stringify(users));
}

function findUserById(id) {
  return loadUsers().find((r) => r.id === id) || null;
}

// ---- portable form -------------------------------------------------------

// What the copy button emits and the paste box accepts: the whole vault as a
// JSON array. Salt and ciphertext travel together per entry, because a blob
// without its salt cannot be decrypted anywhere.
function recordToJson(rec) {
  return { v: 2, kdf: KDF.name, iterations: KDF.iterations, id: rec.id, salt: rec.saltHex, data: rec.dataHex };
}

function vaultToText(users) {
  return JSON.stringify(users.map(recordToJson), null, 2);
}

// Validate one entry. Throws with a readable message on anything unusable — a
// mismatched KDF or iteration count would otherwise only surface later as a
// failed decryption, which reads as "wrong password" and is worse.
function recordFromJson(obj) {
  if (!obj || typeof obj !== 'object') throw new Error('not a record object');
  if (obj.kdf && obj.kdf !== KDF.name) throw new Error(`unsupported kdf "${obj.kdf}"`);
  if (obj.iterations && Number(obj.iterations) !== KDF.iterations) {
    throw new Error(`unsupported iteration count ${obj.iterations}`);
  }
  const salt = hexToBytes(obj.salt || '');
  const data = hexToBytes(obj.data || '');
  if (salt.length !== SALT_BYTES) throw new Error(`salt must be ${SALT_BYTES} bytes`);
  if (data.length < NONCE_BYTES + 16) throw new Error('encrypted data is too short');
  return {
    id: typeof obj.id === 'string' && /^[0-9a-f]{4,64}$/i.test(obj.id) ? obj.id.toLowerCase() : newId(),
    saltHex: bytesToHex(salt),
    dataHex: bytesToHex(data),
  };
}

// Accepts a single record or an array of them.
function textToRecords(text) {
  let obj;
  try {
    obj = JSON.parse(String(text).trim());
  } catch {
    throw new Error('not valid JSON');
  }
  const list = Array.isArray(obj) ? obj : [obj];
  if (!list.length) throw new Error('no records in there');
  return list.map(recordFromJson);
}

// Merge pasted records into the vault: same id replaces (the same account,
// re-encrypted), an identical salt+blob pair is a no-op, anything else is a
// new user. Returns { added, replaced }.
function mergeRecords(incoming) {
  const users = loadUsers();
  let added = 0;
  let replaced = 0;
  for (const rec of incoming) {
    const byId = users.findIndex((r) => r.id === rec.id);
    if (byId >= 0) {
      if (users[byId].saltHex !== rec.saltHex || users[byId].dataHex !== rec.dataHex) replaced++;
      users[byId] = rec;
      continue;
    }
    if (users.some((r) => r.saltHex === rec.saltHex && r.dataHex === rec.dataHex)) continue;
    users.push(rec);
    added++;
  }
  return { stored: saveUsers(users), added, replaced, total: users.length };
}

// ---- crypto --------------------------------------------------------------

async function deriveKey(password, saltBytes) {
  const base = await crypto.subtle.importKey(
    'raw', textEnc.encode(password), 'PBKDF2', false, ['deriveKey']
  );
  return crypto.subtle.deriveKey(
    { name: 'PBKDF2', salt: saltBytes, iterations: KDF.iterations, hash: KDF.hash },
    base,
    { name: 'AES-GCM', length: 256 },
    false,
    ['encrypt', 'decrypt']
  );
}

// Create: fresh random salt, key derived from it plus the password, the
// username and user data encrypted under that key. Returns the record; the
// caller stores it.
async function createRecord(username, password) {
  const salt = crypto.getRandomValues(new Uint8Array(SALT_BYTES));
  const key = await deriveKey(password, salt);
  const nonce = crypto.getRandomValues(new Uint8Array(NONCE_BYTES));
  const ct = new Uint8Array(await crypto.subtle.encrypt(
    { name: 'AES-GCM', iv: nonce }, key, textEnc.encode(makePlaintext(username))
  ));
  const envelope = new Uint8Array(nonce.length + ct.length);
  envelope.set(nonce, 0);
  envelope.set(ct, nonce.length);
  return { id: newId(), saltHex: bytesToHex(salt), dataHex: bytesToHex(envelope) };
}

// Unlock one blob: returns { username, body } on success, null when the
// password doesn't fit this blob (failed AES-GCM tag) or the plaintext isn't
// one of ours.
async function unlockRecord(password, rec) {
  try {
    const salt = hexToBytes(rec.saltHex);
    const envelope = hexToBytes(rec.dataHex);
    if (envelope.length < NONCE_BYTES + 16) return null;
    const key = await deriveKey(password, salt);
    const pt = await crypto.subtle.decrypt(
      { name: 'AES-GCM', iv: envelope.slice(0, NONCE_BYTES) }, key, envelope.slice(NONCE_BYTES)
    );
    return parsePlaintext(textDec.decode(pt));
  } catch {
    return null;
  }
}

// Find the blob that belongs to this username/password pair. Every blob has
// its own salt, so there is no shortcut: each candidate costs one derivation.
// Fine for the handful of users a phone would hold; a real client would key
// the lookup on something public.
//
// The match is case-insensitive, and the *stored* spelling wins — sign in as
// "cedric" on an account created as "Cedric" and the app calls you Cedric.
async function findUser(username, password) {
  const wanted = username.trim().toLowerCase();
  for (const rec of loadUsers()) {
    const opened = await unlockRecord(password, rec);
    if (opened && opened.username.toLowerCase() === wanted) return { rec, ...opened };
  }
  return null;
}

// ---- session -------------------------------------------------------------

// Staying signed in across reloads: the open blob's id and its username are
// remembered, the password is not. Re-deriving the key would need the password
// again, so a restored session carries no plaintext — enough for the roster
// toggles, which is all this prototype does with the account.
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

function clearSession() {
  lsRemove(LS_SESSION);
}

// v1 kept a single blob with no username in it. Nothing here can open it, so
// it is cleared rather than left to rot in localStorage.
function dropLegacyStorage() {
  for (const key of LS_LEGACY) if (lsGet(key) !== null) lsRemove(key);
}

// ---- views + auth state --------------------------------------------------

const mainView = $('main-view');
const signinView = $('signin-view');
const signinForm = $('signin-form');
const usernameInput = $('signin-username');
const passwordInput = $('signin-password');
const accountStatus = $('account-status');
const accountBtn = $('account-btn');
const signinSubmit = $('signin-submit');

// `focus` names the field to land on: 'username' for a fresh sign-in (so the
// browser's auto-fill picker has somewhere to go), 'password' when the username
// is already known and only the key is missing — the unlock case.
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

// Derivation takes a moment (PBKDF2, 310k iterations), so the form is locked
// while it runs — otherwise a double-tap starts a second derivation.
function setSigninBusy(busy) {
  signinSubmit.disabled = busy;
  signinSubmit.textContent = busy ? 'Working…' : 'Sign in';
  usernameInput.disabled = busy;
  passwordInput.disabled = busy;
}

// Drop the signed-in state without touching the vault. Shared by the account
// button and by both delete buttons.
function signOutLocal() {
  state.username = '';
  state.userId = '';
  state.userData = '';
  passwordInput.value = '';
  clearSession();
}

// The single sign-in path. `interactive` distinguishes a real form submit
// (allowed to prompt) from the silent attempt made on load with a saved
// credential (must never pop anything up). `unlockId` names one blob to open
// instead of searching the vault: the session already says which blob is ours,
// it just lacks the key. That case knows the account exists, so a miss is
// plainly a wrong password and creating a second blob for the same username
// would be wrong.
async function attemptSignIn(username, password, { interactive, unlockId = '' }) {
  setSigninBusy(true);
  try {
    let hit;
    if (unlockId) {
      const rec = findUserById(unlockId);
      if (!rec) {
        // Deleted meanwhile, most likely from another tab. The session points
        // at nothing, so drop it rather than blame the password.
        signOutLocal();
        showView('main');
        render();
        if (interactive) alert('That user was deleted on this device.');
        return false;
      }
      const opened = await unlockRecord(password, rec);
      if (!opened) {
        if (interactive) alert('Incorrect password.');
        return false;
      }
      hit = { rec, ...opened };
    } else {
      hit = await findUser(username, password);
    }

    if (!hit) {
      // No blob in the vault opens with this pair. Whether the username is
      // unknown or the password is wrong is not something the app can tell —
      // and not something it should say.
      if (!interactive) return false;
      alert('Incorrect username or password.');
      if (!confirm(`Create a new user "${username.trim()}" with this password?`)) return false;
      const rec = await createRecord(username.trim(), password);
      const users = loadUsers();
      users.push(rec);
      if (!saveUsers(users)) {
        alert('Created, but this browser would not store it — copy the vault '
          + 'out of the user-data box, it is gone on reload.');
      }
      hit = { rec, ...parsePlaintext(makePlaintext(username.trim())) };
    }

    // The stored spelling of the username is the canonical one.
    state.username = hit.username;
    state.userId = hit.rec.id;
    state.userData = hit.text;
    saveSession(hit.rec.id, hit.username);
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

// Set while the form is open to unlock a known blob rather than to sign in:
// holds that blob's id. Cleared whenever the form is opened or left any other
// way, so a later ordinary sign-in is never restricted to it.
let unlockTargetId = '';

// The form is `novalidate` so an empty field reaches this handler instead of
// being swallowed by the browser's own constraint bubble — the prototype wants
// to say what's missing itself.
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

// Account button: open the sign-in view when signed out, sign out when signed in.
// Signing out drops the session but keeps the encrypted blob — the whole point
// is that it can be unlocked again with the same password.
accountBtn.addEventListener('click', () => {
  if (state.username) {
    signOutLocal();
    // Leave usernameInput populated so the next sign-in has it pre-filled.
    render();
  } else {
    unlockTargetId = '';
    showView('signin');
  }
});

// Restore the previous session so a reload stays signed in. Only meaningful
// while that blob is still in the vault; if it was deleted (here or on another
// tab) the session goes with it.
function restoreSession() {
  const session = readSession();
  if (!session) return false;
  if (!findUserById(session.id)) {
    clearSession();
    return false;
  }
  state.username = session.username;
  state.userId = session.id;
  usernameInput.value = session.username;
  return true;
}

// Offer any saved credential on load (Chromium — Firefox/Safari fall back to
// the browser's own auto-fill picker on the input, which the user sees only
// once they open the sign-in view). When the credential carries a password and
// this device has a record, use it to sign in silently; a failure just leaves
// the fields filled in for a manual attempt.
// It also runs when a session was restored: that session carries the username
// but no key, so the credential is the only way to fill the decrypted box
// without asking. In that case the credential must belong to the restored user
// — silently switching accounts on a reload would be a surprise — and the
// username field keeps the restored spelling.
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
    if (password && loadUsers().length) {
      await attemptSignIn(restored || cred.id, password, { interactive: false });
    }
  } catch { /* user dismissed, or no credential */ }
}

// ---- roster parser -------------------------------------------------------

// A single date line: "🗓️ Friday 07.08.2026" — weekday is optional, date is
// dd.mm.yyyy. The leading emoji sometimes has a variation selector (U+FE0F),
// so we match it loosely.
const DATE_RE = /^\s*🗓[️]?\s*([A-Za-z]+)?\s*(\d{1,2}\.\d{1,2}\.\d{2,4})\s*$/u;
// A player line: "01. Name" — number, dot, optional name. Name may be empty
// (an unfilled slot). Numbers are usually zero-padded but we don't require it.
const PLAYER_RE = /^\s*(\d{1,3})\.\s?(.*?)\s*$/;

function parseRoster(text) {
  const lines = text.split(/\r?\n/);
  const blocks = [];
  let current = null;

  lines.forEach((line, idx) => {
    const dm = line.match(DATE_RE);
    if (dm) {
      current = {
        dateLineIdx: idx,
        weekday: dm[1] || '',
        date: dm[2],
        playerStartIdx: null,
        playerEndIdx: null,
        players: [], // parallel to slots; empty string = empty slot
      };
      blocks.push(current);
      return;
    }
    if (!current) return;
    const pm = line.match(PLAYER_RE);
    if (pm) {
      if (current.playerStartIdx === null) current.playerStartIdx = idx;
      current.playerEndIdx = idx;
      current.players.push(pm[2].trim());
    }
  });

  return { lines, blocks };
}

// Format a player list back into "NN. Name" lines, zero-padded to two digits.
function formatPlayerLines(players) {
  return players.map((name, i) => {
    const n = String(i + 1).padStart(2, '0');
    return name ? `${n}. ${name}` : `${n}. `;
  });
}

// Rewrite the original text with the (possibly modified) players spliced back
// into each block. Untouched lines (headers, separators, blank lines) survive
// verbatim, so the round-trip is stable.
function renderRoster(parsed) {
  const out = parsed.lines.slice();
  // Splice back-to-front so earlier indices stay valid.
  for (let i = parsed.blocks.length - 1; i >= 0; i--) {
    const b = parsed.blocks[i];
    if (b.playerStartIdx === null) continue;
    const newLines = formatPlayerLines(b.players);
    out.splice(b.playerStartIdx, b.playerEndIdx - b.playerStartIdx + 1, ...newLines);
  }
  return out.join('\n');
}

// Signed-in players go into the roster as `<username> (app)` so a reader can
// tell app-added names from ones typed manually. Match is case-insensitive
// and tolerates either form so a manually-added "Cedric" still reads as going.
const APP_SUFFIX = ' (app)';
// Names go in capitalized ("cedric" -> "Cedric") so the roster reads the same
// whether the username was typed lowercase or not. Hyphenated and multi-word
// names get each part capitalized; the rest of the part is left as typed so
// "McKay" survives.
function capitalizeName(name) {
  return name.replace(/[^\s'-]+/gu, (part) => part.charAt(0).toUpperCase() + part.slice(1));
}
function displayName(username) { return `${capitalizeName(username)}${APP_SUFFIX}`; }
function isSameUser(slot, username) {
  const s = slot.toLowerCase();
  const u = username.toLowerCase();
  return s === u || s === u + APP_SUFFIX.toLowerCase();
}

function isUserIn(block, username) {
  if (!username) return false;
  return block.players.some((p) => isSameUser(p, username));
}

// Rewrite any bare "<username>" slot to "<username> (app)" so the output
// roster always tags the signed-in user. Idempotent — slots already in the
// (app) form are left alone.
function normalizeUserSlots(parsed, username) {
  if (!username) return;
  const tagged = displayName(username);
  for (const b of parsed.blocks) {
    for (let i = 0; i < b.players.length; i++) {
      if (isSameUser(b.players[i], username) && b.players[i] !== tagged) {
        b.players[i] = tagged;
      }
    }
  }
}

// Add username to the first empty slot, or append a new numbered slot if all
// slots are full. Matches the human convention: fill the gaps first.
//
// The splice in renderRoster covers the *original* player-line region; when
// players.length grows past the original count, splice grows the array to
// fit the new lines without touching the blank line that follows the block.
function addUser(block, username) {
  const name = displayName(username);
  const firstEmpty = block.players.findIndex((p) => p === '');
  if (firstEmpty >= 0) block.players[firstEmpty] = name;
  else block.players.push(name);
}

// Empty the user's slot (case-insensitive match; either "<name>" or
// "<name> (app)"). Keep the slot itself so the numbering doesn't shift.
function removeUser(block, username) {
  for (let i = 0; i < block.players.length; i++) {
    if (isSameUser(block.players[i], username)) block.players[i] = '';
  }
}

function countFilled(block) {
  return block.players.filter((p) => p !== '').length;
}

// ---- weekday label -------------------------------------------------------

// Match the mockup: short weekday + separator. Prefer the weekday actually
// written in the roster; if that's missing (a shorter separator line), compute
// it from the dd.mm.yyyy date.
const WEEKDAY_SHORT = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];

function shortWeekday(block) {
  if (block.weekday) return block.weekday.slice(0, 3);
  const m = block.date.match(/^(\d{1,2})\.(\d{1,2})\.(\d{2,4})$/);
  if (!m) return '';
  let [, d, mo, y] = m;
  if (y.length === 2) y = '20' + y;
  const dt = new Date(Number(y), Number(mo) - 1, Number(d));
  return WEEKDAY_SHORT[dt.getDay()];
}

// Try to lift the game time from the preamble ("🕖 19.00 ~ 21:00" → "19:00").
function extractTime(text) {
  const m = text.match(/(\d{1,2})[:.](\d{2})\s*[~\-–—]\s*\d{1,2}[:.]\d{2}/);
  if (!m) return '';
  return `${m[1]}:${m[2]}`;
}

// ---- state + render ------------------------------------------------------

const state = {
  raw: '',
  parsed: { lines: [], blocks: [] },
  time: '',
  username: '',
  userId: '',    // id of the open blob, so it can be deleted or re-found
  userData: '',  // decrypted plaintext, only while signed in this session
};

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
// The signed-out wording, kept so the locked variant can be swapped back out.
const PLAIN_PLACEHOLDER = plainBox.placeholder;

// The vault, shown so it can be copied to another device. The box is editable
// — pasting records in merges them — so it is only rewritten while the user
// isn't typing in it.
function renderUserData() {
  const users = loadUsers();
  if (document.activeElement !== userdataBox) {
    userdataBox.value = users.length ? vaultToText(users) : '';
  }
  userdataStatus.className = users.length ? 'pill ok' : 'pill';
  if (!users.length) {
    userdataStatus.textContent = 'empty';
  } else {
    const n = `${users.length} blob${users.length === 1 ? '' : 's'}`;
    userdataStatus.textContent = state.userData ? `${n} · unlocked` : n;
  }
  deleteUserBtn.disabled = !state.userId;
  deleteAllBtn.disabled = !users.length;

  // The plaintext only exists while this session holds the password's key, so
  // the box is empty after a reload even though the session survives it. That
  // is what the unlock button is for: signed in, blob present, no key.
  plainBox.value = state.userData;
  plainStatus.className = state.userData ? 'pill ok' : 'pill';
  plainStatus.textContent = state.userData
    ? `${state.userData.length} chars`
    : (state.username ? 'locked' : '—');
  const locked = Boolean(state.username) && !state.userData;
  plainUnlockBtn.hidden = !locked;
  // "Sign in to unlock" is wrong advice while already signed in — then the
  // only thing missing is the password.
  plainBox.placeholder = locked
    ? 'Locked. Press Unlock and enter your password to see the plaintext.'
    : PLAIN_PLACEHOLDER;
}

function render() {
  renderAccount();
  renderUserData();
  const username = state.username;
  normalizeUserSlots(state.parsed, username);
  const blocks = state.parsed.blocks;

  // Update the parse status pill.
  if (!state.raw) {
    parseStatus.className = 'pill';
    parseStatus.textContent = 'empty';
    outStatus.className = 'pill';
    outStatus.textContent = '—';
  } else {
    parseStatus.className = blocks.length ? 'pill ok' : 'pill err';
    parseStatus.textContent = `${blocks.length} date block${blocks.length === 1 ? '' : 's'}`;
  }

  // Update each game card. We wire up two cards; extra blocks are ignored,
  // missing blocks reset the card to its empty state.
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

    const wd = shortWeekday(block);
    whenEl.textContent = state.time ? `${wd} · ${state.time}` : wd;
    countEl.textContent = String(countFilled(block));

    const going = isUserIn(block, username);
    toggle.disabled = !username;
    if (going) {
      card.classList.add('is-going');
      toggle.className = 'status going';
      toggle.innerHTML = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.5l4.2 4.3L19 6.5"/></svg><span>Going</span>`;
    } else {
      card.classList.remove('is-going');
      toggle.className = 'status';
      toggle.textContent = 'Not going';
    }
  }

  // Refresh the output textbox.
  if (state.raw) {
    pasteOut.value = renderRoster(state.parsed);
    outStatus.className = 'pill ok';
    outStatus.textContent = `${pasteOut.value.length} chars`;
  } else {
    pasteOut.value = '';
    outStatus.className = 'pill';
    outStatus.textContent = '—';
  }
}

function ingest(text) {
  state.raw = text;
  state.parsed = parseRoster(text);
  state.time = extractTime(text);
  render();
}

// ---- events --------------------------------------------------------------

pasteIn.addEventListener('input', () => ingest(pasteIn.value));

// After a native paste the caret lands at the end of the pasted text, which
// scrolls the box to the bottom-right. Snap it back to the top-left so the
// user sees the start of the roster.
pasteIn.addEventListener('paste', () => {
  setTimeout(() => {
    pasteIn.scrollTop = 0;
    pasteIn.scrollLeft = 0;
  }, 0);
});

$('paste-btn').addEventListener('click', async () => {
  if (!navigator.clipboard?.readText) {
    alert('Clipboard read not available in this browser — paste into the box manually.');
    return;
  }
  try {
    const text = await navigator.clipboard.readText();
    pasteIn.value = text;
    ingest(text);
    pasteIn.scrollTop = 0;
    pasteIn.scrollLeft = 0;
  } catch (err) {
    alert(`Clipboard read failed: ${err.message}`);
  }
});

$('clear-in').addEventListener('click', () => {
  pasteIn.value = '';
  ingest('');
});

$('copy-btn').addEventListener('click', async () => {
  if (!pasteOut.value) return;
  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(pasteOut.value);
      outStatus.className = 'pill ok';
      outStatus.textContent = 'copied';
      return;
    } catch { /* fall through to select fallback */ }
  }
  pasteOut.focus();
  pasteOut.select();
  document.execCommand('copy');
  outStatus.className = 'pill ok';
  outStatus.textContent = 'copied';
});

// User-data panel. Copy hands the vault to another device; paste (button or
// straight into the box) merges what came back. Storing a foreign blob does
// not sign anyone in — the password still has to unlock it.
$('userdata-copy').addEventListener('click', async () => {
  if (!userdataBox.value) return;
  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(userdataBox.value);
      userdataStatus.className = 'pill ok';
      userdataStatus.textContent = 'copied';
      return;
    } catch { /* fall through to the select fallback */ }
  }
  userdataBox.focus();
  userdataBox.select();
  document.execCommand('copy');
  userdataStatus.className = 'pill ok';
  userdataStatus.textContent = 'copied';
});

// Merge whatever text claims to be one or more records; report why it was
// rejected. Clearing the box on its own deletes nothing — that's what the red
// buttons are for.
function ingestRecordText(text) {
  if (!String(text).trim()) {
    renderUserData();
    return false;
  }
  try {
    const result = mergeRecords(textToRecords(text));
    render();
    userdataStatus.className = 'pill ok';
    userdataStatus.textContent = result.added || result.replaced
      ? `+${result.added} new, ${result.replaced} updated`
      : 'already had those';
    if (!result.stored) {
      alert('This browser would not store the vault — it is gone on reload.');
    }
    return true;
  } catch (err) {
    userdataStatus.className = 'pill err';
    userdataStatus.textContent = err.message || 'unrecognised';
    return false;
  }
}

$('userdata-paste').addEventListener('click', async () => {
  if (!navigator.clipboard?.readText) {
    alert('Clipboard read not available in this browser — paste into the box manually.');
    return;
  }
  try {
    const text = await navigator.clipboard.readText();
    userdataBox.value = text;
    ingestRecordText(text);
  } catch (err) {
    alert(`Clipboard read failed: ${err.message}`);
  }
});

userdataBox.addEventListener('input', () => ingestRecordText(userdataBox.value));

// Unlock a restored-but-locked session: the username is already known, so the
// sign-in form only needs the password. The session names the blob, so the
// submit re-derives the key for that one blob and the plaintext lands in the
// box — see the unlockId branch in attemptSignIn.
plainUnlockBtn.addEventListener('click', () => {
  usernameInput.value = state.username;
  passwordInput.value = '';
  unlockTargetId = state.userId;
  showView('signin', { focus: 'password' });
});

// Destructive, and there is no server-side copy to fall back on, so both ask
// first and name what is about to go.
deleteUserBtn.addEventListener('click', () => {
  if (!state.userId) return;
  if (!confirm(`Delete the user "${state.username}"? Its encrypted blob is `
    + 'removed from this device and cannot be recovered without a copy.')) return;
  const left = loadUsers().filter((r) => r.id !== state.userId);
  saveUsers(left);
  signOutLocal();
  render();
});

deleteAllBtn.addEventListener('click', () => {
  const count = loadUsers().length;
  if (!count) return;
  if (!confirm(`Delete all ${count} user${count === 1 ? '' : 's'}? Every encrypted `
    + 'blob on this device is removed and cannot be recovered without a copy.')) return;
  saveUsers([]);
  signOutLocal();
  render();
});

// Card toggles: add or remove the signed-in user from the corresponding block.
document.querySelectorAll('[data-toggle]').forEach((btn) => {
  btn.addEventListener('click', () => {
    const username = state.username;
    if (!username) return;
    const idx = Number(btn.closest('.game').dataset.block);
    const block = state.parsed.blocks[idx];
    if (!block) return;
    if (isUserIn(block, username)) removeUser(block, username);
    else addUser(block, username);
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
dropLegacyStorage();
restoreSession();
render();
// Always reach for a saved credential: without a session it signs us in, and
// with a restored one it is what re-derives the key so the decrypted box is
// filled instead of locked. render() runs again either way so the unlock
// button reflects whether that worked.
tryPrefill().then(render);
