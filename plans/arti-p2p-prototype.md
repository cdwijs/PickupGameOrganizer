# Arti P2P Prototype — Plan

## Goal
A single native GUI program (Rust + egui) that uses **Arti** (Tor in Rust) to
demonstrate serverless peer-to-peer connectivity over onion services, mirroring
the model PickupGameOrganizer wants (no central server).

## Decisions (confirmed with user)
- **Build env:** Rust added to the container Dockerfile (currently missing). Until
  the image is rebuilt, the user can also build on their host.
- **GUI:** native egui/eframe window.
- **Networking depth:** *onion stream only* — raw length-prefixed stream over the
  Arti onion service. The "libp2p address" field just shows the node's dialable
  multiaddr (`/onion3/<b32>:<port>`); no rust-libp2p yet.

## UX spec
- Two text fields: **Onion address** and **libp2p multiaddr**.
- **Connect** button:
  - Both fields empty → bootstrap Tor, launch an onion service, and fill the two
    fields with this node's own onion address + multiaddr.
  - A field filled → dial that peer over Tor.
- **Status** area: scrolling log of status messages *and* received data.
- **Send increasing number every second** button: once connected, streams
  1, 2, 3, … to the peer every second; the peer shows them in its status area.

## Architecture
```
egui UI thread  --Command(mpsc)-->  tokio backend thread
     ^                                     |
     |  UiEvent(mpsc) + ctx.request_repaint()
     +-------------------------------------+
```
- Backend owns the `TorClient`. Listen mode runs an accept loop
  (RendRequest → StreamRequest → DataStream); dial mode calls `client.connect`.
- The active `DataStream` is split: read half → reader task (forwards received
  text to the UI); write half → shared `Mutex`, used by the counter task.
- Framing: `u32` little-endian length prefix + UTF-8 payload.

## Crate versions (verified compatible)
arti-client / tor-rtcompat / tor-hsservice / tor-hscrypto / tor-cell all `0.37.0`;
eframe `0.29`; tokio `1` (full); futures `0.3`; sha3 `0.10`; dirs `5`.

## Known limitations (prototype)
- Onion publish + first dial go through the real Tor network → 30–120 s the first
  time. There is no loopback shortcut for onion services.
- Single active peer connection (last connection wins in listen mode).
- Arti onion services are officially **experimental** — fine for a prototype.
- Two local instances must use different `ARTI_PROFILE` values (separate state dirs).

## Follow-ups (not in this prototype)
- Real rust-libp2p over an Arti transport.
- Persistent onion identity (`HIDDEN_SERVICE_SECRET` equivalent / keystore reuse).
- Multi-peer handling.
