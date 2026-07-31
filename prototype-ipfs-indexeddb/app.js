// ---------------------------------------------------------------
// Setup: element handles and simple loggers.
// ---------------------------------------------------------------
const $ = (id) => document.getElementById(id);
const els = {
  random: $('btn-random'), randomOut: $('random-out'),
  file: $('btn-file'), fileInfo: $('file-info'),
  add: $('btn-add'), cidOut: $('cid-out'),
  cidIn: $('cid-in'), retrieve: $('btn-retrieve'), retrievedOut: $('retrieved-out'),
  selfMa: $('self-multiaddr'), refreshMa: $('btn-refresh-ma'), copyMa: $('btn-copy-ma'),
  peerMa: $('peer-multiaddr'), connect: $('btn-connect'), peersOut: $('peers-out'),
  peerEvents: $('peer-events'),
  log: $('log'), clearLog: $('btn-clear-log'),
  status: $('status'),
  nodeState: $('node-state'), swarmState: $('swarm-state'), storageStat: $('storage-stat'),
};

function humanBytes(n) {
  if (!Number.isFinite(n)) return '?';
  const units = ['B', 'KB', 'MB', 'GB'];
  let i = 0;
  while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
  return (i === 0 ? n : n.toFixed(1)) + ' ' + units[i];
}

function ts() {
  const d = new Date();
  return d.toTimeString().slice(0, 8) + '.' + String(d.getMilliseconds()).padStart(3, '0');
}
function logTx(direction, msg) {
  const line = `[${ts()}] ${direction.padEnd(11)} ${msg}\n`;
  els.log.value += line;
  els.log.scrollTop = els.log.scrollHeight;
}
function logStatus(kind, msg) {
  const line = `[${ts()}] ${kind}: ${msg}\n`;
  els.status.value += line;
  els.status.scrollTop = els.status.scrollHeight;
  console.log(`[${kind}]`, msg);
}
function logPeer(msg) {
  const line = `[${ts()}] ${msg}\n`;
  els.peerEvents.value += line;
  els.peerEvents.scrollTop = els.peerEvents.scrollHeight;
  console.log('[peer]', msg);
}
els.clearLog.onclick = () => { els.log.value = ''; };

// Register the service worker so the app is installable / usable offline
// after first load. We don't cache the ESM modules yet — they come from the CDN.
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('sw.js').then(
    (reg) => logStatus('sw', 'registered scope=' + reg.scope),
    (err) => logStatus('sw', 'registration failed: ' + err.message)
  );
}

// ---------------------------------------------------------------
// Load Helia + libp2p from a CDN.
// We pin versions that work together (helia 5 / libp2p 2).
// ---------------------------------------------------------------
logStatus('boot', 'loading Helia modules from esm.sh …');

let helia, unixfs, IDBBlockstore, IDBDatastore, multiaddr, CID, bootstrap;
try {
  const [heliaMod, unixfsMod, idbBs, idbDs, maMod, cidMod, bootstrapMod] = await Promise.all([
    import('https://esm.sh/helia@5.5.1'),
    import('https://esm.sh/@helia/unixfs@5.1.0'),
    import('https://esm.sh/blockstore-idb@2.0.4'),
    import('https://esm.sh/datastore-idb@3.0.4'),
    import('https://esm.sh/@multiformats/multiaddr@13.0.1'),
    import('https://esm.sh/multiformats@13.3.6/cid'),
    import('https://esm.sh/@libp2p/bootstrap@11'),
  ]);
  helia = heliaMod;
  unixfs = unixfsMod.unixfs;
  IDBBlockstore = idbBs.IDBBlockstore;
  IDBDatastore = idbDs.IDBDatastore;
  multiaddr = maMod.multiaddr;
  CID = cidMod.CID;
  bootstrap = bootstrapMod.bootstrap;
} catch (err) {
  logStatus('boot', 'FAILED to import modules: ' + err.message);
  els.nodeState.textContent = 'boot failed';
  els.nodeState.classList.add('err');
  throw err;
}
logStatus('boot', 'modules loaded');

// ---------------------------------------------------------------
// Blockstore wrapper: log every put/get so users can see the
// IPFS ↔ IndexedDB conversation.
// ---------------------------------------------------------------
function shortCid(cid) {
  const s = String(cid);
  return s.length > 20 ? s.slice(0, 8) + '…' + s.slice(-6) : s;
}
function wrapBlockstore(inner) {
  return {
    async put(cid, block, options) {
      const res = await inner.put(cid, block, options);
      logTx('IPFS → IDB', `put ${shortCid(cid)} (${block.byteLength} B)`);
      return res;
    },
    async get(cid, options) {
      const block = await inner.get(cid, options);
      logTx('IDB → IPFS', `get ${shortCid(cid)} (${block.byteLength} B)`);
      return block;
    },
    async has(cid, options) { return inner.has(cid, options); },
    async delete(cid, options) {
      logTx('IDB delete', shortCid(cid));
      return inner.delete(cid, options);
    },
    async * putMany(source, options) {
      for await (const pair of source) {
        await inner.put(pair.cid, pair.block, options);
        logTx('IPFS → IDB', `putMany ${shortCid(pair.cid)} (${pair.block.byteLength} B)`);
        yield pair.cid;
      }
    },
    async * getMany(source, options) {
      for await (const cid of source) {
        const block = await inner.get(cid, options);
        logTx('IDB → IPFS', `getMany ${shortCid(cid)} (${block.byteLength} B)`);
        yield { cid, block };
      }
    },
    async * deleteMany(source, options) {
      for await (const cid of source) {
        await inner.delete(cid, options);
        logTx('IDB delete', `deleteMany ${shortCid(cid)}`);
        yield cid;
      }
    },
    async * getAll(options) { yield * inner.getAll(options); },
  };
}

// ---------------------------------------------------------------
// Init IndexedDB backing stores, then Helia with libp2pDefaults
// (which in a browser sets up WebRTC + circuit-relay + noise + yamux).
// ---------------------------------------------------------------
logStatus('boot', 'opening IndexedDB blockstore + datastore …');
const blockstore = new IDBBlockstore('helia-proto-blocks');
const datastore = new IDBDatastore('helia-proto-data');
await blockstore.open();
await datastore.open();

const wrappedBlockstore = wrapBlockstore(blockstore);

// Helia's default bootstrap list uses /dnsaddr/… entries that libp2p
// resolves at runtime via DNS-over-HTTPS (dns.google, cloudflare-dns.com).
// Firefox Enhanced Tracking Protection blocks those DoH requests, so
// we pre-resolve them here — /dnsaddr/bootstrap.libp2p.io → the five
// regional WSS-listening bootstrap peers. Plain browser DNS handles
// /dns/… so no DoH is needed.
const RESOLVED_BOOTSTRAP = [
  '/dns4/va1.bootstrap.libp2p.io/tcp/443/wss/p2p/12D3KooWKnDdG3iXw9eTFijk3EWSunZcFi54Zka4wmtqtt6rPxc8',
  '/dns4/am6.bootstrap.libp2p.io/tcp/443/wss/p2p/QmbLHAnMoJPWSCR5Zhtx6BHJX9KiKNN6tpvbUcqanj75Nb',
  '/dns4/ny5.bootstrap.libp2p.io/tcp/443/wss/p2p/QmQCU2EcMqAqQPR2i9bChDtGNJchTbq5TbXJJ16u19uLTa',
  '/dns4/sg1.bootstrap.libp2p.io/tcp/443/wss/p2p/QmcZf59bWwK5XFi76CZX8cbJ4BhTzzA3gU1ZjYZcYW3dwt',
  '/dns4/sv15.bootstrap.libp2p.io/tcp/443/wss/p2p/QmNnooDu7bfjPFoTZYxMNLWUQJyrVwtbZg5gBMjTezGAJN',
];
const libp2pConfig = helia.libp2pDefaults();
libp2pConfig.peerDiscovery = [bootstrap({ list: RESOLVED_BOOTSTRAP })];

logStatus('boot', 'creating Helia node …');
const heliaNode = await helia.createHelia({
  blockstore: wrappedBlockstore,
  datastore,
  libp2p: libp2pConfig,
  // libp2p defaults for the browser include:
  //   transports: [webRTC, webRTCDirect, webSockets, circuitRelayTransport]
  //   connectionEncryption: [noise]
  //   streamMuxers: [yamux]
  //   services: identify, ping, dcutr, kadDHT, autoNAT, ...
  // We replace peerDiscovery with the pre-resolved bootstrap list above
  // to avoid DoH lookups at startup (which Firefox ETP blocks).
});
const fs = unixfs(heliaNode);

window.helia = heliaNode; // exposed for debugging in the devtools console
window.fs = fs;

logStatus('boot', 'Helia ready, peer ID = ' + heliaNode.libp2p.peerId.toString());
els.nodeState.textContent = 'ready';
els.nodeState.classList.add('ok');
els.file.disabled = false;
els.add.disabled = false;
els.retrieve.disabled = false;
els.connect.disabled = false;

// ---------------------------------------------------------------
// Storage counter: blockstore contents + browser IDB estimate.
// ---------------------------------------------------------------
async function refreshStorageStat() {
  let count = 0, bytes = 0;
  try {
    for await (const { block } of blockstore.getAll()) {
      count++;
      bytes += block.byteLength;
    }
  } catch (e) {
    logStatus('storage', 'getAll failed: ' + e.message);
  }
  let quotaLine = '';
  try {
    const est = await navigator.storage?.estimate?.();
    if (est) quotaLine = ` · IDB ${humanBytes(est.usage || 0)} / ${humanBytes(est.quota || 0)}`;
  } catch { /* not supported */ }
  els.storageStat.textContent = `${count} blocks · ${humanBytes(bytes)}${quotaLine}`;
}
refreshStorageStat();
setInterval(refreshStorageStat, 5000);

// ---------------------------------------------------------------
// Track libp2p events for the status feed.
// ---------------------------------------------------------------
const libp2p = heliaNode.libp2p;

function updateSwarmState() {
  const conns = libp2p.getConnections();
  const relays = conns.filter((c) => c.remoteAddr.toString().includes('/p2p-circuit'));
  const direct = conns.length - relays.length;
  els.swarmState.textContent = `${conns.length} conn (${direct} direct, ${relays.length} relayed)`;
  els.swarmState.classList.toggle('ok', conns.length > 0);
  els.swarmState.classList.toggle('warn', conns.length === 0);
  const peers = conns.map((c) => `${c.remotePeer.toString().slice(-10)}  ${c.remoteAddr.toString()}`);
  els.peersOut.value = peers.join('\n');
}

libp2p.addEventListener('peer:connect', (ev) => {
  logPeer('connect    ' + ev.detail.toString().slice(-16));
  updateSwarmState();
});
libp2p.addEventListener('peer:disconnect', (ev) => {
  logPeer('disconnect ' + ev.detail.toString().slice(-16));
  updateSwarmState();
});
libp2p.addEventListener('self:peer:update', () => {
  refreshMultiaddrs();
  updateSwarmState();
});
libp2p.addEventListener('connection:open', updateSwarmState);
libp2p.addEventListener('connection:close', updateSwarmState);

// ---------------------------------------------------------------
// Self multiaddrs — the multiaddrs another peer needs to dial us.
// Only relayed and webrtc-reachable ones are actually usable from
// another browser behind NAT, so we filter and rank.
// ---------------------------------------------------------------
function refreshMultiaddrs() {
  const mas = libp2p.getMultiaddrs().map((m) => m.toString());
  // Rank: /webrtc via /p2p-circuit are most useful to another browser;
  // plain /p2p-circuit is second best; direct WSS is fine on a LAN.
  const rank = (m) => {
    if (m.includes('/webrtc') && m.includes('/p2p-circuit')) return 0;
    if (m.includes('/p2p-circuit')) return 1;
    if (m.includes('/wss') || m.includes('/tls/ws')) return 2;
    if (m.includes('/webrtc')) return 3;
    return 4;
  };
  const sorted = mas.slice().sort((a, b) => rank(a) - rank(b));
  els.selfMa.value = sorted.join('\n') || '(no dialable addresses yet — waiting for a circuit-relay reservation)';
}
els.refreshMa.onclick = refreshMultiaddrs;
els.copyMa.onclick = async () => {
  const first = els.selfMa.value.split('\n')[0];
  if (first) {
    try {
      await navigator.clipboard.writeText(first);
      logStatus('ui', 'copied first multiaddr to clipboard');
    } catch (e) {
      logStatus('ui', 'clipboard write failed: ' + e.message);
    }
  }
};
refreshMultiaddrs();
updateSwarmState();
// Some multiaddrs (esp. relayed ones) appear only after we get a reservation.
setInterval(refreshMultiaddrs, 3000);

// ---------------------------------------------------------------
// Action 1: random 40-char string
// ---------------------------------------------------------------
const ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
let currentString = null;
let currentFile = null;
els.random.onclick = () => {
  const bytes = new Uint8Array(40);
  crypto.getRandomValues(bytes);
  currentString = Array.from(bytes, (b) => ALPHABET[b % ALPHABET.length]).join('');
  els.randomOut.value = currentString;
  logStatus('action', `generated random string (${currentString.length} chars)`);
  currentFile = null;
  els.fileInfo.textContent = '(no file yet)';
};

// ---------------------------------------------------------------
// Action 2: wrap string in a file
// ---------------------------------------------------------------
els.file.onclick = () => {
  if (!currentString) {
    logStatus('action', 'no string yet — click "Generate" first');
    return;
  }
  currentFile = new TextEncoder().encode(currentString);
  els.fileInfo.textContent = `file: ${currentFile.byteLength} bytes, text/plain`;
  logStatus('action', `wrapped string in a ${currentFile.byteLength}-byte file`);
};

// ---------------------------------------------------------------
// Action 3: add file to IPFS
// ---------------------------------------------------------------
els.add.onclick = async () => {
  if (!currentFile) {
    logStatus('action', 'no file yet — click "Wrap in a file" first');
    return;
  }
  els.add.disabled = true;
  try {
    logStatus('action', 'unixfs.addBytes(…) → …');
    const cid = await fs.addBytes(currentFile);
    els.cidOut.value = cid.toString();
    els.cidIn.value = cid.toString();
    logStatus('action', 'added, CID = ' + cid.toString());
    refreshStorageStat();
  } catch (err) {
    logStatus('action', 'add failed: ' + err.message);
  } finally {
    els.add.disabled = false;
  }
};

// ---------------------------------------------------------------
// Action 4: retrieve by CID
// ---------------------------------------------------------------
const RETRIEVE_TIMEOUT_MS = 30_000;
els.retrieve.onclick = async () => {
  const cidStr = (els.cidIn.value || els.cidOut.value).trim();
  if (!cidStr) {
    logStatus('action', 'no CID supplied');
    return;
  }
  let cid;
  try { cid = CID.parse(cidStr); }
  catch (e) { logStatus('action', 'invalid CID: ' + e.message); return; }

  els.retrieve.disabled = true;
  els.retrievedOut.value = '';
  // fs.cat searches Bitswap forever if the block isn't local and no
  // connected peer has it. Abort after RETRIEVE_TIMEOUT_MS so the UI
  // doesn't stay stuck.
  const signal = AbortSignal.timeout(RETRIEVE_TIMEOUT_MS);
  try {
    const isLocal = await blockstore.has(cid);
    logStatus('action', `unixfs.cat(${shortCid(cid)}) → … (${isLocal ? 'in local blockstore' : 'searching peers'})`);
    const chunks = [];
    for await (const chunk of fs.cat(cid, { signal })) chunks.push(chunk);
    const total = chunks.reduce((n, c) => n + c.byteLength, 0);
    const merged = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) { merged.set(c, off); off += c.byteLength; }
    const text = new TextDecoder().decode(merged);
    els.retrievedOut.value = text;
    logStatus('action', `retrieved ${total} bytes`);
    refreshStorageStat();
  } catch (err) {
    // AbortSignal.timeout throws a TimeoutError; make that message clearer.
    const msg = err?.name === 'TimeoutError' || /abort/i.test(err?.message || '')
      ? `no peer had this CID within ${RETRIEVE_TIMEOUT_MS / 1000}s — try another CID or wait for more peers`
      : err.message;
    logStatus('action', 'retrieve failed: ' + msg);
  } finally {
    els.retrieve.disabled = false;
  }
};

// ---------------------------------------------------------------
// Connect to a peer via a pasted multiaddr.
// libp2p handles NAT traversal: if the multiaddr contains
// /p2p-circuit/webrtc it will hole-punch, otherwise it stays relayed.
// ---------------------------------------------------------------
els.connect.onclick = async () => {
  const raw = els.peerMa.value.trim();
  if (!raw) { logStatus('swarm', 'no multiaddr entered'); return; }
  let ma;
  try { ma = multiaddr(raw); }
  catch (e) { logStatus('swarm', 'invalid multiaddr: ' + e.message); return; }
  els.connect.disabled = true;
  try {
    logStatus('swarm', 'dialing ' + raw + ' …');
    await libp2p.dial(ma);
    logStatus('swarm', 'dial succeeded');
  } catch (err) {
    logStatus('swarm', 'dial failed: ' + err.message);
  } finally {
    els.connect.disabled = false;
  }
};
