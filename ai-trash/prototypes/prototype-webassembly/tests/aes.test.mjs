import fs from 'node:fs';
import crypto from 'node:crypto';
const SC = process.env.WASM_TEST_BUILD || '/shared/tmp/prototype-webassembly';
const { instance } = await WebAssembly.instantiate(fs.readFileSync(SC+'/t_aes.wasm'), {});
const e = instance.exports, base = e.buf();
const mem = () => new Uint8Array(e.memory.buffer);
const put = (off, b) => mem().set(b, base + off);
const get = (off, n) => Buffer.from(mem().slice(base + off, base + off + n));
let fails = 0;
const check = (n, got, want) => { const ok = got === want;
  if (!ok) { fails++; console.log(`  FAIL ${n}\n    got  ${got}\n    want ${want}`); } else console.log(`  ok   ${n}`); };

// FIPS-197 C.3: AES-256 single block
put(0, Buffer.from('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f','hex'));
put(64, Buffer.from('00112233445566778899aabbccddeeff','hex'));
e.t_block();
check('AES-256 block (FIPS-197 C.3)', get(8192,16).toString('hex'), '8ea2b7ca516745bfeafc49904b496089');

// GCM against node, several aad/pt lengths including empty and unaligned
for (const [aadlen, ptlen] of [[0,0],[0,16],[0,17],[13,64],[16,15],[20,100],[1,1],[64,512]]) {
  const key = crypto.randomBytes(32), iv = crypto.randomBytes(12);
  const aad = crypto.randomBytes(aadlen), pt = crypto.randomBytes(ptlen);
  put(0,key); put(64,iv); put(128,aad); put(1024,pt);
  e.t_enc(aadlen, ptlen);
  const ct = get(8192, ptlen), tag = get(12288, 16);
  const c = crypto.createCipheriv('aes-256-gcm', key, iv);
  if (aadlen) c.setAAD(aad);
  const nct = Buffer.concat([c.update(pt), c.final()]);
  check(`gcm enc aad=${aadlen} pt=${ptlen}`, ct.toString('hex')+tag.toString('hex'),
    nct.toString('hex')+c.getAuthTag().toString('hex'));
  // and decrypt what node produced
  put(1024, nct); put(12288, c.getAuthTag());
  const ok = e.t_dec(aadlen, ptlen);
  check(`gcm dec aad=${aadlen} pt=${ptlen}`, ok===1 ? get(8192, ptlen).toString('hex') : 'REJECTED', pt.toString('hex'));
}
// a flipped tag bit must be refused
{
  const key = crypto.randomBytes(32), iv = crypto.randomBytes(12), pt = crypto.randomBytes(48);
  put(0,key); put(64,iv); put(1024,pt); e.t_enc(0,48);
  const ct = get(8192,48), tag = get(12288,16);
  tag[3] ^= 0x01;
  put(1024, ct); put(12288, tag);
  check('gcm rejects a flipped tag bit', String(e.t_dec(0,48)), '0');
  tag[3] ^= 0x01; put(12288, tag);
  check('gcm accepts the intact tag',    String(e.t_dec(0,48)), '1');
}
console.log(fails ? `\n${fails} FAILURES` : '\nall passed');
process.exit(fails?1:0);
