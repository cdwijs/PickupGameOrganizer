# IPFS ↔ IndexedDB Prototype

A single-page Progressive Web App that shows, in real time, every read and
write that a Helia (JS IPFS) node makes against its IndexedDB-backed
blockstore. Two instances of the PWA can find each other across NAT / firewalls
using libp2p's WebRTC + circuit-relay-v2 transports.

## Run

Serve the folder over HTTP (a service worker won't register from `file://`):

```sh
cd prototype-ipfs-indexeddb
python3 -m http.server 8080
# then open http://localhost:8080 in a modern browser
```

First load pulls Helia and libp2p from `esm.sh` (~1.5 MB, cached by the browser
afterwards). No build step, no bundler.

## What the UI does

**Actions panel**

1. **Generate random 40-char string** — pure `crypto.getRandomValues`.
2. **Wrap in a file** — turn the string into a `Uint8Array` payload.
3. **Add file to IPFS** — `unixfs.addBytes(...)`. Every block the encoder
   emits lands in the IndexedDB blockstore; the transaction log records the
   `IPFS → IDB put` for each one and the resulting CID appears in the
   *CID* box.
4. **Retrieve** — `unixfs.cat(CID)`. Every block read produces an
   `IDB → IPFS get`. Bytes are decoded back to text.

**Swarm panel**

- **This node's dialable addresses** — the multiaddrs libp2p reports as
  externally reachable, sorted so the WebRTC-over-circuit-relay entries
  (the ones another browser can actually dial) show first. Copy one and
  send it to the other peer.
- **Peer connection info** — paste the other peer's multiaddr and hit
  **Connect to peer**. libp2p dials, upgrades to direct WebRTC when it can,
  and stays on the relay otherwise.

**Transaction log** — every put/get on the blockstore, timestamped.

**Status** — libp2p events (peer connect/disconnect, self-address updates)
and coarse action logs.

## Bootstrap without DoH

Helia's default bootstrap list is `/dnsaddr/bootstrap.libp2p.io/...`, which
libp2p resolves at runtime through DNS-over-HTTPS (`dns.google`,
`cloudflare-dns.com`). Firefox's Enhanced Tracking Protection blocks those
DoH requests, so no bootstrap peers are reachable and the swarm stays empty.

This prototype sidesteps that by pre-resolving the five regional bootstrap
peers into `/dns4/.../tcp/443/wss/p2p/...` multiaddrs and passing them as
libp2p's `peerDiscovery` list directly. Plain browser DNS handles `/dns4/…`,
so no DoH lookup is needed and Firefox connects the same way Chromium does.

If a bootstrap peer moves or is renamed, the hardcoded list will need
updating — run `dig TXT _dnsaddr.bootstrap.libp2p.io` (or the equivalent
DoH curl) to see the current entries.

## How NAT / firewall traversal works

Browsers can't listen on a TCP/UDP port, so the standard trick is:

1. On startup, libp2p connects to a set of **public WSS-listening relay
   nodes** (the Helia bootstrap list — Protocol Labs peers).
2. libp2p asks each relay for a **circuit-relay-v2 reservation**. Once
   granted, the relay announces our peer ID as reachable at
   `/dns4/<relay-host>/tcp/443/wss/p2p/<relay-peer>/p2p-circuit/p2p/<our-peer>`.
3. When another browser dials that multiaddr, the connection first flows
   through the relay. libp2p's **DCUtR** service then tries to hole-punch
   the two peers' NATs so the connection can be upgraded to **direct
   WebRTC** (`/webrtc`). If the punch fails the traffic keeps going through
   the relay — the connection still works, just with an extra hop.

The multiaddrs shown in the *dialable addresses* box are ranked so the
`/p2p-circuit/webrtc` entries — the ones that let two browsers behind
different NATs meet — appear first.

## Files

- `index.html` — HTML, CSS and all the JavaScript.
- `sw.js` — minimal service worker (app-shell cache only).
- `manifest.json` — PWA manifest, makes the page installable.
- `icon.svg` — inline app icon.

## Known limitations

- Bootstrap relay reservations can take 10–30 seconds after page load,
  and depend on public infrastructure being reachable. If the
  *dialable addresses* box stays empty for a while, that's what's happening.
- Because Helia is imported from a CDN, the first load needs network.
  Subsequent loads work offline as long as the browser's HTTP cache still
  has the modules.
- IndexedDB blockstore state persists across reloads and reflects the same
  data if you open the app in two tabs of the same browser — for a real
  swarm demo use two different browsers, profiles, or devices.
