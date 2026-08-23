// Differential test: the same roster through prototype-minimal's JavaScript
// and through the C module, compared byte for byte.
import fs from 'node:fs';
const SC = process.env.WASM_TEST_BUILD || '/shared/tmp/prototype-webassembly';
const js = await import(SC + '/js_roster.mjs');
const { instance } = await WebAssembly.instantiate(
  fs.readFileSync(new URL('../app.wasm', import.meta.url).pathname), {});
const e = instance.exports;
const enc = new TextEncoder(), dec = new TextDecoder();
const u8 = () => new Uint8Array(e.memory.buffer);
const put = s => { const b = enc.encode(s), p = e.wasm_alloc(b.length); u8().set(b,p); u8()[p+b.length]=0; return p; };
const str = p => { const m=u8(); let q=p; while(m[q])q++; return dec.decode(m.slice(p,q)); };
let fails = 0;
const check = (n, got, want) => { const ok = String(got)===String(want);
  if (!ok) { fails++; console.log(`  FAIL ${n}\n    got  ${JSON.stringify(got)}\n    want ${JSON.stringify(want)}`); }
  else console.log(`  ok   ${n}`); };

const SAMPLE = `⚽ Terrible Football Haarlem
🕖 19.00 ~ 21:00
🧭 We play here: https://tinyurl.com/TF-pitch
🙌 8 players gets the game going. At 30, the pitch is full.

__________________________

🗓️ Friday 07.08.2026

01. Teize
3. Alex

__________________________

🗓️ Monday 10.08.2026

01. Teize
3. Amine
03.
04.

⚽ Football pools and other terrible ideas.`;

// capitalizeName / displayName across a spread of shapes
for (const name of ['cedric','CEDRIC','Cedric','jan-piet','van der berg',"o'brien",'mcKay','émile','ann marie']) {
  e.wasm_reset();
  check(`displayName(${JSON.stringify(name)})`, str(e.display_name(put(name))), js.displayName(name));
}

// extractTime over several preamble shapes
for (const t of ['🕖 19.00 ~ 21:00','19:00 - 21:00','9.30–11.00','no time here','x 7.05 — 8.00 y']) {
  e.wasm_reset();
  e.roster_parse(put(t + '\n🗓️ Friday 07.08.2026\n01. A'), put(''));
  check(`extractTime(${JSON.stringify(t)})`, JSON.parse(str(e.roster_info(0))).time, js.extractTime(t));
}

// the sample: block count, per-block fields, and the rewritten text
function jsState(text, username) {
  const parsed = js.parseRoster(text);
  js.normalizeUserSlots(parsed, username);
  return parsed;
}
for (const user of ['', 'Cedric', 'teize', 'ALEX']) {
  e.wasm_reset();
  const n = e.roster_parse(put(SAMPLE), put(user));
  const parsed = jsState(SAMPLE, user);
  check(`[${user||'signed out'}] block count`, n, parsed.blocks.length);
  for (let i = 0; i < n; i++) {
    const info = JSON.parse(str(e.roster_info(i)));
    const b = parsed.blocks[i];
    check(`[${user||'signed out'}] block ${i} date`, info.date, b.date);
    check(`[${user||'signed out'}] block ${i} weekday`, info.weekday, js.shortWeekday(b));
    check(`[${user||'signed out'}] block ${i} count`, info.count, js.countFilled(b));
    check(`[${user||'signed out'}] block ${i} going`, info.going, js.isUserIn(b, user));
  }
  check(`[${user||'signed out'}] rewritten text`, str(e.roster_out()), js.renderRoster(parsed));
}

// toggling: join then leave, both blocks, compared at every step
for (const user of ['Cedric', 'teize']) {
  e.wasm_reset();
  e.roster_parse(put(SAMPLE), put(user));
  const parsed = jsState(SAMPLE, user);
  for (const [idx, act] of [[0,'join'],[1,'join'],[0,'leave'],[1,'leave'],[0,'join']]) {
    e.roster_flip(idx);
    const b = parsed.blocks[idx];
    if (js.isUserIn(b, user)) js.removeUser(b, user); else js.addUser(b, user);
    check(`[${user}] ${act} block ${idx} -> text`, str(e.roster_out()), js.renderRoster(parsed));
    check(`[${user}] ${act} block ${idx} -> count`,
      JSON.parse(str(e.roster_info(idx))).count, js.countFilled(b));
  }
}

// awkward inputs
const ODD = [
  ['empty', ''],
  ['no blocks', 'just some text\nwith lines'],
  ['date with no weekday', '🗓️ 07.08.2026\n01. A\n02.'],
  ['no variation selector', '🗓 Friday 07.08.2026\n01. A'],
  ['two-digit year', '🗓️ Sunday 07.08.26\n01. A'],
  ['all slots full', '🗓️ Friday 07.08.2026\n01. A\n02. B'],
  ['CRLF', '🗓️ Friday 07.08.2026\r\n01. A\r\n02.\r\n'],
  ['no trailing newline', '🗓️ Friday 07.08.2026\n01. A'],
  ['blank line inside players', '🗓️ Friday 07.08.2026\n01. A\n\n02. B'],
];
for (const [label, text] of ODD) {
  e.wasm_reset();
  const n = e.roster_parse(put(text), put('Cedric'));
  const parsed = jsState(text, 'Cedric');
  check(`odd: ${label} — blocks`, n, parsed.blocks.length);
  check(`odd: ${label} — round trip`, str(e.roster_out()), js.renderRoster(parsed));
  if (n > 0) {
    e.roster_flip(0);
    const b = parsed.blocks[0];
    if (js.isUserIn(b, 'Cedric')) js.removeUser(b, 'Cedric'); else js.addUser(b, 'Cedric');
    check(`odd: ${label} — after join`, str(e.roster_out()), js.renderRoster(parsed));
  }
}
console.log(fails ? `\n${fails} FAILURES` : '\nall passed');
process.exit(fails?1:0);
