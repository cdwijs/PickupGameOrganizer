import fs from 'node:fs';
import crypto from 'node:crypto';
const DIR = new URL('..', import.meta.url).pathname;
const { instance } = await WebAssembly.instantiate(fs.readFileSync(DIR + 'app.wasm'), {});
const e = instance.exports;
const enc = new TextEncoder(), dec = new TextDecoder();
const u8 = () => new Uint8Array(e.memory.buffer);
function put(s) {
  const b = enc.encode(s), p = e.wasm_alloc(b.length);
  u8().set(b, p); u8()[p + b.length] = 0; return p;
}
function putBytes(b) { const p = e.wasm_alloc(b.length); u8().set(b, p); return p; }
function str(p) {
  if (!p) return '';
  const m = u8(); let end = p; while (m[end]) end++;
  return dec.decode(m.slice(p, end));
}
const J = p => JSON.parse(str(p));
let fails = 0;
const check = (n, got, want) => { const ok = String(got) === String(want);
  if (!ok) { fails++; console.log(`  FAIL ${n}\n    got  ${got}\n    want ${want}`); } else console.log(`  ok   ${n}`); };

const RND = () => crypto.randomBytes(e.rnd_needed());

// --- create an account ---
e.wasm_reset();
let out = J(e.account_create(put('[]'), put('Cedric'), put('hunter2'), putBytes(RND())));
check('create returns a username', out.username, 'Cedric');
const lines = out.text.split('\n');
check('plaintext line 1', lines[0], 'Readable: Cedric');
check('plaintext line 2', lines[1], 'This is a placeholder for the user data');
check('has a [group1] section', lines[3], '[group1]');
check('alg line', lines[4], 'alg: ECDSA P-256');
check('public is 91-byte SPKI', lines[5].slice(8).length, 182);
check('private is 138-byte PKCS8', lines[6].slice(9).length, 276);
let vault = out.vault, id = out.id;
check('vault holds one record', e.vault_count(put(vault)), 1);

// --- the C-generated key must be a real P-256 key ---
const spki = Buffer.from(lines[5].slice(8), 'hex');
const pkcs8 = Buffer.from(lines[6].slice(9), 'hex');
const pub = await crypto.webcrypto.subtle.importKey('spki', spki, {name:'ECDSA',namedCurve:'P-256'}, true, ['verify']);
const priv = await crypto.webcrypto.subtle.importKey('pkcs8', pkcs8, {name:'ECDSA',namedCurve:'P-256'}, true, ['sign']);
const sig = await crypto.webcrypto.subtle.sign({name:'ECDSA',hash:'SHA-256'}, priv, Buffer.from('x'));
check('group1 keypair signs and verifies',
  await crypto.webcrypto.subtle.verify({name:'ECDSA',hash:'SHA-256'}, pub, sig, Buffer.from('x')), true);

// --- the blob must open in WebCrypto: same format as prototype-minimal ---
const rec = JSON.parse(vault)[0];
{
  const key = await crypto.webcrypto.subtle.importKey('raw', enc.encode('hunter2'), 'PBKDF2', false, ['deriveKey']);
  const aes = await crypto.webcrypto.subtle.deriveKey(
    {name:'PBKDF2', salt: Buffer.from(rec.salt,'hex'), iterations: 310000, hash:'SHA-256'},
    key, {name:'AES-GCM', length:256}, false, ['decrypt']);
  const env = Buffer.from(rec.data,'hex');
  const pt = await crypto.webcrypto.subtle.decrypt({name:'AES-GCM', iv: env.slice(0,12)}, aes, env.slice(12));
  check('WebCrypto opens the C blob', dec.decode(new Uint8Array(pt)) === out.text, true);
}

// --- and a blob written the prototype-minimal way must open in C ---
{
  const salt = crypto.randomBytes(16), nonce = crypto.randomBytes(12);
  const text = 'Readable: Teize\nThis is a placeholder for the user data';
  const key = crypto.pbkdf2Sync('other-pw', salt, 310000, 32, 'sha256');
  const c = crypto.createCipheriv('aes-256-gcm', key, nonce);
  const ct = Buffer.concat([c.update(Buffer.from(text)), c.final(), c.getAuthTag()]);
  const foreign = JSON.stringify([{v:2,kdf:'PBKDF2-SHA256',iterations:310000,id:'aabbccdd',
    salt:salt.toString('hex'), data:Buffer.concat([nonce,ct]).toString('hex')}]);
  e.wasm_reset();
  const hit = J(e.account_signin(put(foreign), put('teize'), put('other-pw')));
  check('C opens a blob written elsewhere', hit.text, text);
  check('  and keeps the stored spelling', hit.username, 'Teize');
}

// --- sign in / wrong password ---
e.wasm_reset();
check('signin, right password', J(e.account_signin(put(vault), put('cedric'), put('hunter2'))).username, 'Cedric');
check('signin, wrong password', J(e.account_signin(put(vault), put('Cedric'), put('nope'))).error, 'nomatch');
check('signin, unknown user',   J(e.account_signin(put(vault), put('Nobody'), put('hunter2'))).error, 'nomatch');
check('unlock by id',           J(e.account_unlock(put(vault), put(id), put('hunter2'))).username, 'Cedric');
check('unlock, wrong password', J(e.account_unlock(put(vault), put(id), put('nope'))).error, 'badpassword');
check('unlock, deleted blob',   J(e.account_unlock(put(vault), put('deadbeef'), put('hunter2'))).error, 'gone');

// --- second account, same password, different user ---
e.wasm_reset();
const two = J(e.account_create(put(vault), put('Teize'), put('hunter2'), putBytes(RND())));
vault = two.vault;
check('two blobs now', e.vault_count(put(vault)), 2);
check('  Cedric still opens', J(e.account_signin(put(vault), put('Cedric'), put('hunter2'))).username, 'Cedric');
check('  Teize opens too',    J(e.account_signin(put(vault), put('Teize'),  put('hunter2'))).username, 'Teize');
check('  distinct keypairs',
  J(e.account_signin(put(vault), put('Cedric'), put('hunter2'))).text.split('\n')[5] !==
  J(e.account_signin(put(vault), put('Teize'),  put('hunter2'))).text.split('\n')[5], true);

// --- vault text / ingest / delete ---
e.wasm_reset();
const box = str(e.vault_text(put(vault)));
check('vault text is valid JSON of 2', JSON.parse(box).length, 2);
const merged = J(e.vault_ingest(put('[]'), put(box), putBytes(crypto.randomBytes(512)), 512));
check('ingest into an empty vault adds 2', `${merged.added}/${merged.replaced}`, '2/0');
const again = J(e.vault_ingest(put(vault), put(box), putBytes(crypto.randomBytes(512)), 512));
check('re-ingesting the same pair is a no-op', `${again.added}/${again.replaced}/${again.total}`, '0/0/2');
const one = JSON.parse(box)[0];
const changed = JSON.stringify([{...one, data: one.data.slice(0, -2) + (one.data.slice(-2) === 'ff' ? 'ee' : 'ff')}]);
check('same id, different blob replaces',
  (x => `${x.added}/${x.replaced}`)(J(e.vault_ingest(put(vault), put(changed), putBytes(crypto.randomBytes(512)), 512))), '0/1');
check('bad JSON rejected',     J(e.vault_ingest(put(vault), put('{nope'), putBytes(crypto.randomBytes(512)), 512)).error, 'not valid JSON');
check('wrong kdf rejected',    J(e.vault_ingest(put(vault), put(JSON.stringify([{...one, kdf:'scrypt'}])), putBytes(crypto.randomBytes(512)), 512)).error, 'unsupported kdf');
check('wrong iterations rejected', J(e.vault_ingest(put(vault), put(JSON.stringify([{...one, iterations:1000}])), putBytes(crypto.randomBytes(512)), 512)).error, 'unsupported iteration count');
check('short salt rejected',   J(e.vault_ingest(put(vault), put(JSON.stringify([{...one, salt:'aabb'}])), putBytes(crypto.randomBytes(512)), 512)).error, 'salt must be 16 bytes');
check('delete one leaves one', e.vault_count(put(str(e.vault_delete(put(vault), put(id))))), 1);

console.log(fails ? `\n${fails} FAILURES` : '\nall passed');
process.exit(fails?1:0);
