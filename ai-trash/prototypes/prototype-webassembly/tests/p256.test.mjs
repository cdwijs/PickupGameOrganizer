import fs from 'node:fs';
import crypto from 'node:crypto';
const SC = process.env.WASM_TEST_BUILD || '/shared/tmp/prototype-webassembly';
const { instance } = await WebAssembly.instantiate(fs.readFileSync(SC+'/t_p256.wasm'), {});
const e = instance.exports, base = e.buf();
const mem = () => new Uint8Array(e.memory.buffer);
const put = (off, b) => mem().set(b, base + off);
const get = (off, n) => Buffer.from(mem().slice(base + off, base + off + n));
const b64u = s => Buffer.from(s, 'base64url');
let fails = 0;
const check = (n, got, want) => { const ok = got === want;
  if (!ok) { fails++; console.log(`  FAIL ${n}\n    got  ${got}\n    want ${want}`); } else console.log(`  ok   ${n}`); };

// 1. G itself: d = 1 must give the standard base point
put(0, Buffer.from('0000000000000000000000000000000000000000000000000000000000000001','hex'));
check('1*G == base point', e.t_mult()===1 ? get(512,32).toString('hex')+get(544,32).toString('hex') : 'FAILED',
  '6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296'
 +'4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5');
check('G is on the curve', String(e.t_curve()), '1');

// 2. d*G against keys Node generated — the test that catches any wrong constant
for (let i = 0; i < 6; i++) {
  const kp = crypto.generateKeyPairSync('ec', { namedCurve: 'P-256' });
  const jwk = kp.privateKey.export({ format: 'jwk' });
  put(0, b64u(jwk.d));
  const ok = e.t_mult();
  check(`d*G matches node key #${i+1}`, ok===1 ? get(512,32).toString('hex')+'/'+get(544,32).toString('hex') : 'FAILED',
    b64u(jwk.x).toString('hex')+'/'+b64u(jwk.y).toString('hex'));
}

// 3. edge scalars
{
  const n = BigInt('0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551');
  const hex = v => v.toString(16).padStart(64,'0');
  put(0, Buffer.from(hex(0n),'hex'));  check('d = 0 refused', String(e.t_mult()), '0');
  put(0, Buffer.from(hex(n),'hex'));   check('d = n refused', String(e.t_mult()), '0');
  put(0, Buffer.from(hex(n+1n),'hex'));check('d > n refused', String(e.t_mult()), '0');
  put(0, Buffer.from(hex(n-1n),'hex'));check('d = n-1 accepted and on curve',
    e.t_mult()===1 && e.t_curve()===1 ? 'yes' : 'no', 'yes');
}

// 4. keygen + DER, imported into WebCrypto and used for a real signature
const cands = crypto.randomBytes(32*8);
put(0, cands);
check('keygen picks a scalar', String(e.t_keygen(8)), '1');
const d = get(576,32), x = get(512,32), y = get(544,32);
check('generated point is on the curve', String(e.t_curve()), '1');
e.t_spki();  const spki  = get(1024, 91);
e.t_pkcs8(); const pkcs8 = get(1024, 138);
console.log(`  spki  ${spki.length}B  ${spki.toString('hex').slice(0,40)}…`);
console.log(`  pkcs8 ${pkcs8.length}B ${pkcs8.toString('hex').slice(0,40)}…`);

const pub  = await crypto.webcrypto.subtle.importKey('spki',  spki,  {name:'ECDSA',namedCurve:'P-256'}, true, ['verify']);
const priv = await crypto.webcrypto.subtle.importKey('pkcs8', pkcs8, {name:'ECDSA',namedCurve:'P-256'}, true, ['sign']);
check('WebCrypto accepts the SPKI',  'imported', 'imported');
const msg = Buffer.from('group1 message');
const sig = await crypto.webcrypto.subtle.sign({name:'ECDSA',hash:'SHA-256'}, priv, msg);
const ok  = await crypto.webcrypto.subtle.verify({name:'ECDSA',hash:'SHA-256'}, pub, sig, msg);
check('sign with C private key, verify with C public key', String(ok), 'true');
const bad = await crypto.webcrypto.subtle.verify({name:'ECDSA',hash:'SHA-256'}, pub, sig, Buffer.from('other'));
check('and it rejects a different message', String(bad), 'false');
const jwk = await crypto.webcrypto.subtle.exportKey('jwk', priv);
check('WebCrypto reads back the same d', b64u(jwk.d).toString('hex'), d.toString('hex'));
check('WebCrypto reads back the same x', b64u(jwk.x).toString('hex'), x.toString('hex'));
check('WebCrypto reads back the same y', b64u(jwk.y).toString('hex'), y.toString('hex'));

const t0 = Date.now();
for (let i = 0; i < 20; i++) { put(0, crypto.randomBytes(32*4)); e.t_keygen(4); }
console.log(`\n  keygen: ${((Date.now()-t0)/20).toFixed(1)} ms per keypair`);
console.log(fails ? `\n${fails} FAILURES` : '\nall passed');
process.exit(fails?1:0);
