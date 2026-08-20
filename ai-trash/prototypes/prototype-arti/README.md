# Arti P2P Prototype

A single native (egui) program that demonstrates **serverless peer-to-peer
connectivity over Tor** using [Arti](https://arti.torproject.org/) (Tor in Rust)
and its onion services. This is the "how would PickupGameOrganizer connect peers
without a central server" spike.

## What it does

- **Connect** with both fields empty → bootstraps Tor, launches an **onion
  service**, and fills the fields with this node's **onion address** and a
  **libp2p-style multiaddr** (`/onion3/<b32>:9999`).
- **Connect** with the onion field (or a `/onion3/…` multiaddr) filled → **dials
  that peer** over Tor.
- **Status area** → live log of status messages *and* data received from the peer.
- **Send increasing number every second** → once connected, streams `1, 2, 3, …`
  to the peer every second; the peer displays them in its status area.

The data channel is a raw length-prefixed byte stream over the onion service
(`u32` little-endian length + UTF-8 payload). No rust-libp2p yet — the "libp2p
address" field is purely the dialable multiaddr form of the onion address.

> ⚠️ Arti onion services are officially **experimental** — fine for a prototype,
> not for sensitive production traffic.

## Web version

A browser port of this demo lives in [`web/`](web/) — same connect + counter
(start/pause/resume) flow, but over a **WebRTC data channel** with copy/paste
signaling instead of Tor (browsers can't run onion services). Works in Firefox,
Chromium, and Safari. See [`web/README.md`](web/README.md).

## Build & run

Requires a Rust toolchain and (for the GUI) system libraries — see
[System dependencies](#system-dependencies).

```bash
cd prototype-arti
cargo run --release
```

First launch bootstraps Tor and publishes the onion descriptor, which goes over
the **real Tor network** — expect **30–120 s** before the service is reachable.
There is no loopback shortcut for onion services.

### Trying it with two nodes on one machine

Onion services always route through the real Tor network, so you can run both
peers on the same host — just give them separate state directories:

```bash
# Terminal 1 — the host. Leave fields empty, press Connect, copy its onion address.
ARTI_PROFILE=a cargo run --release

# Terminal 2 — the dialer. Paste node A's onion address, press Connect.
ARTI_PROFILE=b cargo run --release
```

Then press **Send increasing number every second** in either window and watch the
numbers arrive in the other.

## System dependencies

egui/eframe needs native windowing + GL libs. On Debian/Ubuntu:

```bash
apt-get install -y \
  libgl1-mesa-dev libxkbcommon-dev libwayland-dev \
  libxcb1-dev libxrandr-dev libxi-dev libx11-dev pkg-config
```

## Adding Rust to the container image

The dev container currently has **no Rust toolchain**. Add this to the relevant
`Dockerfile` so every session ships with it (and the egui system libs above):

```dockerfile
# Rust toolchain
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
      | sh -s -- -y --default-toolchain stable
ENV PATH="/root/.cargo/bin:${PATH}"

# Native libs for egui/eframe
RUN apt-get update && apt-get install -y --no-install-recommends \
      libgl1-mesa-dev libxkbcommon-dev libwayland-dev \
      libxcb1-dev libxrandr-dev libxi-dev libx11-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*
```

Rebuild the image afterwards. Note the GUI still needs a display to *run*
(a desktop session, or `xvfb-run` for headless smoke tests); it compiles fine
headless.

## Known limitations

- Single active peer connection (in listen mode, the most recent connection wins).
- No persistent onion identity yet — each run generates a new address unless
  Arti's keystore for `ARTI_PROFILE` is reused.
- Prototype-grade error handling; the status log is the source of truth.

## Layout

- `src/main.rs` — everything: egui UI thread + tokio backend (Tor bootstrap,
  onion service accept loop, dialer, framed read/write, counter).
- `Cargo.toml` — arti-family crates pinned to `0.37`, `eframe` `0.29`.
