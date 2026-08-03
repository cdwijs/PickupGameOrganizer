// Signing prototype. Every step below has its own button; each button records
// how long its step took via performance.now() and writes it into the pill in
// the section header plus the timing summary at the bottom.
//
// Signature + hash algorithms come from the two <select>s at the top of the
// page. Changing either resets all downstream state, because the CryptoKey
// material is bound to the algorithm at generation time. Extractable keys are
// used so the export-key steps can show something; a real app would generate
// with extractable:false and keep the CryptoKey handle in IndexedDB.

// Returns { keygen, sig } — the argument objects for generateKey and sign.
// h is the selected hash name (ignored by Ed25519, which uses SHA-512 internally).
function algoSpec(sigId, h) {
  const RSA_KEYGEN = (bits, name) => ({
    name, modulusLength: bits, publicExponent: new Uint8Array([1, 0, 1]),
    hash: { name: h },
  });
  const HASH_BYTES = { 'SHA-1': 20, 'SHA-256': 32, 'SHA-384': 48, 'SHA-512': 64 };
  switch (sigId) {
    case 'ECDSA-P256': return { keygen: { name: 'ECDSA', namedCurve: 'P-256' }, sig: { name: 'ECDSA', hash: { name: h } } };
    case 'ECDSA-P384': return { keygen: { name: 'ECDSA', namedCurve: 'P-384' }, sig: { name: 'ECDSA', hash: { name: h } } };
    case 'ECDSA-P521': return { keygen: { name: 'ECDSA', namedCurve: 'P-521' }, sig: { name: 'ECDSA', hash: { name: h } } };
    case 'RSA-PSS-2048': return { keygen: RSA_KEYGEN(2048, 'RSA-PSS'), sig: { name: 'RSA-PSS', saltLength: HASH_BYTES[h] } };
    case 'RSA-PSS-3072': return { keygen: RSA_KEYGEN(3072, 'RSA-PSS'), sig: { name: 'RSA-PSS', saltLength: HASH_BYTES[h] } };
    case 'RSA-PSS-4096': return { keygen: RSA_KEYGEN(4096, 'RSA-PSS'), sig: { name: 'RSA-PSS', saltLength: HASH_BYTES[h] } };
    case 'RSASSA-PKCS1-2048': return { keygen: RSA_KEYGEN(2048, 'RSASSA-PKCS1-v1_5'), sig: { name: 'RSASSA-PKCS1-v1_5' } };
    case 'Ed25519': return { keygen: { name: 'Ed25519' }, sig: { name: 'Ed25519' } };
    default: throw new Error(`unknown signature algorithm: ${sigId}`);
  }
}

// State held between steps. Cleared by "Reset all".
const state = {
  keyPair: null,       // { publicKey, privateKey } — CryptoKey pair
  keyAlgo: null,       // signature-algorithm id used at keygen ('ECDSA-P256', ...)
  keyHash: null,       // hash-algorithm id used at keygen ('SHA-256', ...)
  signAlgo: null,      // resolved sign() parameter object matching the keys
  publicKeyDer: null,  // ArrayBuffer, SPKI
  privateKeyDer: null, // ArrayBuffer, PKCS#8
  messageBytes: null,  // Uint8Array
  messageHash: null,   // ArrayBuffer (32/48/64 bytes depending on hash)
  signature: null,     // ArrayBuffer (size depends on algorithm)
};

const el = (id) => document.getElementById(id);
const $ = {
  sigAlgo: el('sig-algo'),
  hashAlgo: el('hash-algo'),
  btnGen: el('btn-gen'),
  btnReset: el('btn-reset'),
  btnExportPub: el('btn-export-pub'),
  btnExportPriv: el('btn-export-priv'),
  btnEncode: el('btn-encode'),
  btnHash: el('btn-hash'),
  btnSign: el('btn-sign'),
  btnVerify: el('btn-verify'),
  btnTamper: el('btn-tamper'),
  msg: el('msg'),
  pubOut: el('pub-out'),
  privOut: el('priv-out'),
  msgHex: el('msg-hex'),
  hashOut: el('hash-out'),
  sigOut: el('sig-out'),
  s1Status: el('s1-status'),
  s7Result: el('s7-result'),
  s8Result: el('s8-result'),
  log: el('log'),
};

// --- helpers ------------------------------------------------------------

const bufToHex = (buf) => {
  const view = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  let out = '';
  for (let i = 0; i < view.length; i++) out += view[i].toString(16).padStart(2, '0');
  return out;
};

const bufToBase64 = (buf) => {
  const view = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  let s = '';
  const CHUNK = 0x8000;
  for (let i = 0; i < view.length; i += CHUNK) {
    s += String.fromCharCode.apply(null, view.subarray(i, i + CHUNK));
  }
  return btoa(s);
};

const fmtMs = (ms) => {
  if (ms < 1) return `${(ms * 1000).toFixed(0)} µs`;
  if (ms < 10) return `${ms.toFixed(2)} ms`;
  if (ms < 1000) return `${ms.toFixed(1)} ms`;
  return `${(ms / 1000).toFixed(3)} s`;
};

const log = (msg, cls = '') => {
  const div = document.createElement('div');
  const t = new Date().toISOString().slice(11, 23);
  div.textContent = `${t}  ${msg}`;
  if (cls) div.className = cls;
  $.log.appendChild(div);
  $.log.scrollTop = $.log.scrollHeight;
};

const setTime = (step, ms) => {
  el(`s${step}-time`).textContent = fmtMs(ms);
  const row = document.querySelector(`.step-ms[data-step="${step}"]`);
  if (row) {
    row.textContent = fmtMs(ms);
    row.classList.remove('pending', 'fail');
    row.classList.add('done');
  }
};

const setTimeFail = (step) => {
  el(`s${step}-time`).textContent = 'failed';
  const row = document.querySelector(`.step-ms[data-step="${step}"]`);
  if (row) {
    row.textContent = 'failed';
    row.classList.remove('pending', 'done');
    row.classList.add('fail');
  }
};

// Wrap a step so that all bookkeeping (timing, error catching, log) lives in
// one place instead of being copy-pasted into every button handler.
async function runStep(step, name, fn) {
  const btn = document.activeElement && document.activeElement.tagName === 'BUTTON'
    ? document.activeElement
    : null;
  if (btn) btn.disabled = true;
  const start = performance.now();
  try {
    const result = await fn();
    const ms = performance.now() - start;
    setTime(step, ms);
    log(`step ${step} · ${name} · ${fmtMs(ms)}`, 'ok');
    return result;
  } catch (err) {
    const ms = performance.now() - start;
    setTimeFail(step);
    log(`step ${step} · ${name} · FAILED after ${fmtMs(ms)}: ${err.message || err}`, 'err');
    throw err;
  } finally {
    if (btn) btn.disabled = false;
  }
}

// --- step 1 : generate key pair -----------------------------------------

$.btnGen.addEventListener('click', async () => {
  const spec = algoSpec($.sigAlgo.value, $.hashAlgo.value);
  const pair = await runStep(1, `generateKey (${$.sigAlgo.value})`, () =>
    crypto.subtle.generateKey(spec.keygen, /* extractable */ true, ['sign', 'verify'])
  );
  state.keyPair = pair;
  state.keyAlgo = $.sigAlgo.value;
  state.keyHash = $.hashAlgo.value;
  $.s1Status.textContent = `Key pair generated (${state.keyAlgo}, hash ${state.keyHash}). Private key held in memory only.`;
  $.btnExportPub.disabled = false;
  $.btnExportPriv.disabled = false;
  if (state.messageBytes) $.btnSign.disabled = false;
});

// --- step 2 : export public key -----------------------------------------

$.btnExportPub.addEventListener('click', async () => {
  const der = await runStep(2, 'exportKey (SPKI)', () =>
    crypto.subtle.exportKey('spki', state.keyPair.publicKey)
  );
  state.publicKeyDer = der;
  $.pubOut.textContent = bufToBase64(der);
});

// --- step 3 : export private key ----------------------------------------

$.btnExportPriv.addEventListener('click', async () => {
  const der = await runStep(3, 'exportKey (PKCS#8)', () =>
    crypto.subtle.exportKey('pkcs8', state.keyPair.privateKey)
  );
  state.privateKeyDer = der;
  $.privOut.textContent = bufToBase64(der);
});

// --- step 4 : encode message --------------------------------------------

$.btnEncode.addEventListener('click', async () => {
  const bytes = await runStep(4, 'TextEncoder.encode', () => {
    // performance.now() around a synchronous call is fine — the timing
    // resolution is sub-millisecond in secure contexts.
    return new TextEncoder().encode($.msg.value);
  });
  state.messageBytes = bytes;
  $.msgHex.textContent = bufToHex(bytes);
  $.btnHash.disabled = false;
  if (state.keyPair) $.btnSign.disabled = false;
});

// --- step 5 : hash message ----------------------------------------------

$.btnHash.addEventListener('click', async () => {
  const h = $.hashAlgo.value;
  const digest = await runStep(5, `subtle.digest (${h})`, () =>
    crypto.subtle.digest(h, state.messageBytes)
  );
  state.messageHash = digest;
  $.hashOut.textContent = bufToHex(digest);
});

// --- step 6 : sign ------------------------------------------------------

$.btnSign.addEventListener('click', async () => {
  const spec = algoSpec(state.keyAlgo, state.keyHash);
  state.signAlgo = spec.sig;
  const sig = await runStep(6, `subtle.sign (${spec.sig.name})`, () =>
    crypto.subtle.sign(spec.sig, state.keyPair.privateKey, state.messageBytes)
  );
  state.signature = sig;
  $.sigOut.textContent = bufToBase64(sig);
  $.btnVerify.disabled = false;
  $.btnTamper.disabled = false;
});

// --- step 7 : verify ----------------------------------------------------

$.btnVerify.addEventListener('click', async () => {
  const ok = await runStep(7, 'subtle.verify', () =>
    crypto.subtle.verify(state.signAlgo, state.keyPair.publicKey, state.signature, state.messageBytes)
  );
  $.s7Result.textContent = ok ? 'valid ✓' : 'invalid ✗';
  $.s7Result.className = ok ? 'pill ok' : 'pill err';
  log(ok ? 'verify → true' : 'verify → false', ok ? 'ok' : 'err');
});

// --- step 8 : tamper test ----------------------------------------------

$.btnTamper.addEventListener('click', async () => {
  const ok = await runStep(8, 'subtle.verify (tampered signature)', async () => {
    // Flip one bit of the signature; keep the original in state.signature.
    const tampered = new Uint8Array(state.signature.byteLength);
    tampered.set(new Uint8Array(state.signature));
    tampered[0] ^= 0x01;
    return crypto.subtle.verify(state.signAlgo, state.keyPair.publicKey, tampered, state.messageBytes);
  });
  // Expectation: ok === false. That is a successful tamper test — the
  // signature no longer matches, which is exactly what a signature should do.
  const pass = ok === false;
  $.s8Result.textContent = pass ? 'detected ✓' : 'MISSED ✗';
  $.s8Result.className = pass ? 'pill ok' : 'pill err';
  log(pass
    ? 'tamper detected: verify returned false on flipped signature'
    : 'tamper NOT detected: verify returned true (should not happen)',
    pass ? 'ok' : 'err'
  );
});

// --- reset --------------------------------------------------------------

$.btnReset.addEventListener('click', () => {
  Object.keys(state).forEach((k) => { state[k] = null; });
  $.pubOut.textContent = '';
  $.privOut.textContent = '';
  $.msgHex.textContent = '';
  $.hashOut.textContent = '';
  $.sigOut.textContent = '';
  $.s1Status.textContent = 'No keys yet.';
  $.s7Result.textContent = 'not run';
  $.s7Result.className = 'pill';
  $.s8Result.textContent = 'not run';
  $.s8Result.className = 'pill';
  for (let i = 1; i <= 8; i++) {
    el(`s${i}-time`).textContent = '— ms';
    const row = document.querySelector(`.step-ms[data-step="${i}"]`);
    if (row) {
      row.textContent = '—';
      row.classList.remove('done', 'fail');
      row.classList.add('pending');
    }
  }
  $.btnExportPub.disabled = true;
  $.btnExportPriv.disabled = true;
  $.btnHash.disabled = true;
  $.btnSign.disabled = true;
  $.btnVerify.disabled = true;
  $.btnTamper.disabled = true;
  $.log.replaceChildren();
  log('reset');
});

// --- algorithm-select behaviour ----------------------------------------

// Changing either select invalidates the current key material (RSA keys bind
// the hash at generation time, so *every* algorithm change forces regenerate
// for consistency). Skip the reset if nothing was ever generated.
function onAlgoChange() {
  if (state.keyPair) {
    $.btnReset.click();
    log('algorithm changed → keys and downstream state cleared', 'err');
  }
}
$.sigAlgo.addEventListener('change', onAlgoChange);
$.hashAlgo.addEventListener('change', onAlgoChange);

// Feature-detect Ed25519. It landed in Chromium 137 but isn't in every
// browser yet — disable the option if generateKey rejects.
(async function detectEd25519() {
  const opt = Array.from($.sigAlgo.options).find((o) => o.value === 'Ed25519');
  if (!opt) return;
  try {
    await crypto.subtle.generateKey({ name: 'Ed25519' }, false, ['sign', 'verify']);
  } catch {
    opt.disabled = true;
    opt.textContent = 'Ed25519 (not supported in this browser)';
    log('Ed25519 not available in this browser — option disabled', 'err');
  }
})();

// --- service worker -----------------------------------------------------

if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('sw.js').catch((err) => {
      log(`service worker registration failed: ${err.message}`, 'err');
    });
  });
}

log('ready. press "Generate" to create a key pair.');
