// Minimal prototype. Login form (pattern from prototype-password), two game
// cards, and a paste-and-parse pipeline that keeps the updated roster in sync
// with the toggles.
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

// ---- views + auth state --------------------------------------------------

const mainView = $('main-view');
const signinView = $('signin-view');
const signinForm = $('signin-form');
const usernameInput = $('signin-username');
const passwordInput = $('signin-password');
const accountStatus = $('account-status');
const accountBtn = $('account-btn');
const accountHint = $('account-hint');

function showView(name) {
  const showSignin = name === 'signin';
  mainView.hidden = showSignin;
  signinView.hidden = !showSignin;
  if (showSignin) {
    // Focus username so the browser's auto-fill picker has somewhere to land.
    setTimeout(() => usernameInput.focus(), 0);
  }
}

function renderAccount() {
  if (state.username) {
    accountStatus.className = 'pill ok';
    accountStatus.textContent = `signed in as ${state.username}`;
    accountBtn.textContent = 'Sign out';
    accountHint.innerHTML = 'Signed in — tap <em>Not going</em> / <em>Going</em> on a card to update the roster.';
  } else {
    accountStatus.className = 'pill';
    accountStatus.textContent = 'not signed in';
    accountBtn.textContent = 'Sign in';
    accountHint.innerHTML = 'Sign in to toggle <em>going</em> per game.';
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

signinForm.addEventListener('submit', async (evt) => {
  evt.preventDefault();
  const fd = new FormData(signinForm);
  const username = String(fd.get('username') || '').trim();
  const password = String(fd.get('password') || '');
  if (!username) return;
  state.username = username;
  await storeCredential(username, password);
  showView('main');
  render();
});

$('signin-cancel').addEventListener('click', () => {
  usernameInput.value = state.username;
  passwordInput.value = '';
  showView('main');
});

// Account button: open the sign-in view when signed out, sign out when signed in.
accountBtn.addEventListener('click', () => {
  if (state.username) {
    state.username = '';
    passwordInput.value = '';
    // Leave usernameInput populated so the next sign-in has it pre-filled.
    render();
  } else {
    showView('signin');
  }
});

// Offer any saved credential on load (Chromium — Firefox/Safari fall back to
// the browser's own auto-fill picker on the input, which the user sees only
// once they open the sign-in view).
async function tryPrefill() {
  if (!navigator.credentials?.get) return;
  try {
    const cred = await navigator.credentials.get({ password: true, mediation: 'optional' });
    if (cred?.id) {
      usernameInput.value = cred.id;
      if ('password' in cred && cred.password) passwordInput.value = cred.password;
      state.username = cred.id;
      render();
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

// Case-insensitive "does the roster contain the user" — a signed-in user
// counts as going for a block if their username appears in any non-empty slot.
function isUserIn(block, username) {
  if (!username) return false;
  const u = username.toLowerCase();
  return block.players.some((p) => p.toLowerCase() === u);
}

// Add username to the first empty slot, or append a new numbered slot if all
// slots are full. Matches the human convention: fill the gaps first.
//
// The splice in renderRoster covers the *original* player-line region; when
// players.length grows past the original count, splice grows the array to
// fit the new lines without touching the blank line that follows the block.
function addUser(block, username) {
  const firstEmpty = block.players.findIndex((p) => p === '');
  if (firstEmpty >= 0) block.players[firstEmpty] = username;
  else block.players.push(username);
}

// Empty the user's slot (case-insensitive match). Keep the slot itself so the
// numbering doesn't shift, matching the "01. Alice / 02. / 03. " pattern.
function removeUser(block, username) {
  const u = username.toLowerCase();
  for (let i = 0; i < block.players.length; i++) {
    if (block.players[i].toLowerCase() === u) block.players[i] = '';
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
};

const pasteIn = $('paste-in');
const pasteOut = $('paste-out');
const parseStatus = $('parse-status');
const outStatus = $('out-status');
const agendaStatus = $('agenda-status');

function render() {
  renderAccount();
  const username = state.username;
  const blocks = state.parsed.blocks;

  // Update the parse status pill.
  if (!state.raw) {
    parseStatus.className = 'pill';
    parseStatus.textContent = 'empty';
    outStatus.className = 'pill';
    outStatus.textContent = '—';
    agendaStatus.className = 'pill';
    agendaStatus.textContent = 'paste a roster below';
  } else {
    parseStatus.className = blocks.length ? 'pill ok' : 'pill err';
    parseStatus.textContent = `${blocks.length} date block${blocks.length === 1 ? '' : 's'}`;
    agendaStatus.className = username ? 'pill ok' : 'pill';
    agendaStatus.textContent = username ? `roster loaded — ${username}` : 'roster loaded';
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

$('paste-btn').addEventListener('click', async () => {
  if (!navigator.clipboard?.readText) {
    alert('Clipboard read not available in this browser — paste into the box manually.');
    return;
  }
  try {
    const text = await navigator.clipboard.readText();
    pasteIn.value = text;
    ingest(text);
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
render();
tryPrefill();
