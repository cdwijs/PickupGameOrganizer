// Password prototype. Wires up the sign-in and sign-up forms, submits them
// without leaving the page, and asks the browser to save the credentials via
// the Credential Management API.
//
// What makes the browser offer to save a password:
//   1. A <form> with type="password" and type="text"/"email" inputs.
//   2. Correct autocomplete attributes: "username" + "current-password" for
//      login, "username" + "new-password" for signup.
//   3. A real submit (or a navigation) that the browser can attribute the
//      credentials to. For SPAs that don't navigate, the Credential Management
//      API (navigator.credentials.store) is the explicit hook.
//
// Chromium browsers implement PasswordCredential; Firefox and Safari currently
// do not. On those, the ordinary form-save heuristic still works because the
// form has proper autocomplete semantics.

const $ = (id) => document.getElementById(id);

const logEl = $('log');
function log(msg, kind = '') {
  const div = document.createElement('div');
  div.className = kind;
  const t = new Date().toISOString().slice(11, 23);
  div.textContent = `${t}  ${msg}`;
  logEl.prepend(div);
}

// ---- mode toggle ---------------------------------------------------------

const signinForm = $('signin-form');
const signupForm = $('signup-form');
const modeSignin = $('mode-signin');
const modeSignup = $('mode-signup');

function setMode(mode) {
  const isSignin = mode === 'signin';
  signinForm.hidden = !isSignin;
  signupForm.hidden = isSignin;
  modeSignin.classList.toggle('active', isSignin);
  modeSignup.classList.toggle('active', !isSignin);
  modeSignin.setAttribute('aria-selected', String(isSignin));
  modeSignup.setAttribute('aria-selected', String(!isSignin));
}
modeSignin.addEventListener('click', () => setMode('signin'));
modeSignup.addEventListener('click', () => setMode('signup'));

// ---- form submission -----------------------------------------------------

const lastStatus = $('last-status');
const lastOut = $('last-out');

function maskPassword(pw) {
  if (!pw) return '';
  return '•'.repeat(Math.min(pw.length, 24));
}

// Ask the browser to save the credential. Works only in Chromium; on other
// browsers PasswordCredential is undefined and we fall back to relying on the
// browser's own form-save heuristic (which the correct autocomplete attributes
// on the <form> already enable).
async function storeCredential(username, password) {
  if (typeof PasswordCredential === 'undefined' || !navigator.credentials?.store) {
    log('PasswordCredential API not available — relying on browser form-save heuristic', '');
    return { stored: false, reason: 'unsupported' };
  }
  try {
    const cred = new PasswordCredential({
      id: username,
      password,
      name: username,
    });
    await navigator.credentials.store(cred);
    log(`navigator.credentials.store(${username}) — done`, 'ok');
    return { stored: true };
  } catch (err) {
    log(`navigator.credentials.store failed: ${err.message}`, 'err');
    return { stored: false, reason: err.message };
  }
}

async function handleSubmit(evt, kind) {
  evt.preventDefault();
  const form = evt.currentTarget;
  const fd = new FormData(form);
  const username = String(fd.get('username') || '').trim();
  const password = String(fd.get(kind === 'signup' ? 'new-password' : 'password') || '');
  const email = String(fd.get('email') || '').trim();

  if (kind === 'signup') {
    const confirm = String(fd.get('new-password-confirm') || '');
    if (password !== confirm) {
      lastStatus.className = 'pill err';
      lastStatus.textContent = 'passwords differ';
      log('Sign-up rejected: password confirmation does not match', 'err');
      return;
    }
  }

  lastStatus.className = 'pill ok';
  lastStatus.textContent = kind === 'signup' ? 'account created' : 'signed in';
  lastOut.textContent = [
    `mode:     ${kind}`,
    `username: ${username}`,
    email ? `email:    ${email}` : null,
    `password: ${maskPassword(password)}  (${password.length} chars)`,
  ].filter(Boolean).join('\n');

  log(`${kind === 'signup' ? 'Sign-up' : 'Sign-in'} submitted for "${username}"`, 'ok');

  await storeCredential(username, password);
}

signinForm.addEventListener('submit', (e) => handleSubmit(e, 'signin'));
signupForm.addEventListener('submit', (e) => handleSubmit(e, 'signup'));

// ---- clear + credential lookup ------------------------------------------

$('clear-last').addEventListener('click', (e) => {
  e.preventDefault();
  lastOut.textContent = '';
  lastStatus.className = 'pill';
  lastStatus.textContent = 'nothing yet';
  log('Last submission cleared');
});

$('try-credmgr').addEventListener('click', async (e) => {
  e.preventDefault();
  if (!navigator.credentials?.get) {
    log('navigator.credentials.get not available in this browser', 'err');
    return;
  }
  try {
    const cred = await navigator.credentials.get({
      password: true,
      mediation: 'optional',
    });
    if (!cred) {
      log('navigator.credentials.get returned null (no saved credential, or user dismissed)', '');
      return;
    }
    log(`Got credential: id="${cred.id}", type=${cred.type}`, 'ok');
    // Populate the sign-in form so the user can see the round-trip.
    setMode('signin');
    $('signin-username').value = cred.id || '';
    if ('password' in cred && cred.password) {
      $('signin-password').value = cred.password;
    }
  } catch (err) {
    log(`navigator.credentials.get failed: ${err.message}`, 'err');
  }
});

// ---- environment probe --------------------------------------------------

function reportEnv() {
  const rows = [];
  const secure = window.isSecureContext;
  rows.push(`isSecureContext:        ${secure}`);
  rows.push(`protocol:               ${location.protocol}`);
  rows.push(`host:                   ${location.host}`);
  rows.push(`PasswordCredential:     ${typeof PasswordCredential !== 'undefined'}`);
  rows.push(`navigator.credentials:  ${!!navigator.credentials}`);
  rows.push(`  .store:               ${!!(navigator.credentials && navigator.credentials.store)}`);
  rows.push(`  .get:                 ${!!(navigator.credentials && navigator.credentials.get)}`);
  rows.push(`serviceWorker support:  ${'serviceWorker' in navigator}`);
  $('env-out').textContent = rows.join('\n');

  const status = $('env-status');
  if (!secure) {
    status.className = 'pill err';
    status.textContent = 'insecure — password saving disabled';
  } else if (typeof PasswordCredential === 'undefined') {
    status.className = 'pill';
    status.textContent = 'secure — form-save only';
  } else {
    status.className = 'pill ok';
    status.textContent = 'secure — full support';
  }
}

// ---- service worker -----------------------------------------------------

if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('./sw.js').then(
      (reg) => log(`Service worker registered (scope ${reg.scope})`, 'ok'),
      (err) => log(`Service worker registration failed: ${err.message}`, 'err'),
    );
  });
}

reportEnv();
log('Ready');
