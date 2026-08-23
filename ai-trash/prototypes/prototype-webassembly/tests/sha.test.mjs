import fs from 'node:fs';
import crypto from 'node:crypto';
const SC = process.env.WASM_TEST_BUILD || '/shared/tmp/prototype-webassembly';
const { instance } = await WebAssembly.instantiate(fs.readFileSync(SC+'/t_sha.wasm'), {});
const e = instance.exports;
const mem = () => new Uint8Array(e.memory.buffer);
const base = e.buf();
const put = (off, bytes) => mem().set(bytes, base + off);
const digest = () => Buffer.from(mem().slice(base + 4096, base + 4096 + 32));
let fails = 0;
const check = (name, got, want) => {
  const ok = got === want;
  if (!ok) { fails++; console.log(`  FAIL ${name}\n    got  ${got}\n    want ${want}`); }
  else console.log(`  ok   ${name}`);
};

// SHA-256 — NIST vectors then random inputs against node
put(0, Buffer.from('abc'));
e.t_sha256(3);
check('sha256("abc")', digest().toString('hex'),
  'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
e.t_sha256(0);
check('sha256("")', digest().toString('hex'),
  'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
for (const len of [1, 55, 56, 63, 64, 65, 127, 128, 1000, 4096]) {
  const data = crypto.randomBytes(len);
  put(0, data); e.t_sha256(len);
  check(`sha256(random ${len}B)`, digest().toString('hex'),
    crypto.createHash('sha256').update(data).digest('hex'));
}

// HMAC — RFC 4231 case 2, then randoms
put(0, Buffer.from('Jefe')); put(1024, Buffer.from('what do ya want for nothing?'));
e.t_hmac(4, 28);
check('hmac rfc4231 #2', digest().toString('hex'),
  '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843');
for (const [kl, ml] of [[1, 1], [32, 100], [64, 64], [65, 3], [200, 500]]) {
  const k = crypto.randomBytes(kl), m = crypto.randomBytes(ml);
  put(0, k); put(1024, m); e.t_hmac(kl, ml);
  check(`hmac(key ${kl}B, msg ${ml}B)`, digest().toString('hex'),
    crypto.createHmac('sha256', k).update(m).digest('hex'));
}

// PBKDF2 — against node, including the app's real 310 000 iterations
for (const [pw, salt, iters] of [['password', 'salt', 1], ['password', 'salt', 4096],
                                 ['hunter2', 'NaCl-ish', 1000], ['pässwörd ünicode', 'salty', 999]]) {
  const p = Buffer.from(pw), s = Buffer.from(salt);
  put(0, p); put(1024, s); e.t_pbkdf2(p.length, s.length, iters);
  check(`pbkdf2(${JSON.stringify(pw)}, ${iters})`, digest().toString('hex'),
    crypto.pbkdf2Sync(p, s, iters, 32, 'sha256').toString('hex'));
}
const p = Buffer.from('hunter2'), s = crypto.randomBytes(16);
put(0, p); put(1024, s);
const t0 = Date.now(); e.t_pbkdf2(p.length, 16, 310000); const ms = Date.now() - t0;
check('pbkdf2 310k (the real one)', digest().toString('hex'),
  crypto.pbkdf2Sync(p, s, 310000, 32, 'sha256').toString('hex'));
console.log(`\n  310k iterations took ${ms} ms in wasm`);
console.log(fails ? `\n${fails} FAILURES` : '\nall passed');
process.exit(fails ? 1 : 0);
