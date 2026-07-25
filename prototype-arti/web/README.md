# Arti P2P Prototype — Web

A browser version of the native Arti prototype. It reproduces the same demo —
connect two peers, then stream an increasing number every second with a
**start / pause / resume** button — but in the browser.

Browsers can't run Tor onion services, so instead of exchanging onion addresses
this uses a **WebRTC data channel** with **manual copy/paste signaling** (no
signaling server, so it stays serverless — the peers exchange the connection
"signal" by hand, just like copying an onion address). Works in **Firefox,
Chromium, and Safari**.

## Run

It's a single self-contained `index.html` — no build step, no dependencies.

```bash
cd prototype-arti/web
python3 -m http.server 8000     # or any static file server
# then open http://localhost:8000 in each peer's browser
```

Opening the file directly (`file://…/index.html`) also works, but serving over
`http(s)` is more reliable (some browsers restrict the clipboard and WebRTC on
`file://`). Cross-machine use needs the pages served over **HTTPS**.

## Connecting two peers

1. **Host** presses **Host (create offer)**, copies *Your signal*, and sends it
   to the peer (chat, email, etc.).
2. **Joiner** pastes that into *Peer's signal*, presses **Join (create answer)**,
   and sends their generated signal back.
3. **Host** pastes the joiner's answer and presses **Finish connect (apply
   answer)**.

When the status shows **Connected**, press **Send increasing number every
second** in either window. Press it again to **pause**, again to **resume** —
the count continues from where it left off. Received numbers appear in the peer's
status log as `⟵ received: N`.

## How it maps to the native prototype

| Native (`src/main.rs`)                    | Web (`web/index.html`)                     |
|-------------------------------------------|--------------------------------------------|
| Tor onion service + dial                  | WebRTC data channel (copy/paste signaling) |
| Onion address / `/onion3` multiaddr       | JSON offer/answer "signal" blob            |
| Length-prefixed byte stream               | `RTCDataChannel.send()` string messages    |
| Counter start → pause → resume            | Same toggle button                         |
| Status log + received data                | Status log + received data                 |

## Notes

- Uses a public Google STUN server for NAT traversal. Peers behind strict
  (symmetric) NATs may fail to connect without a TURN relay — out of scope for a
  prototype.
- Single peer connection at a time; hosting or joining again resets the previous
  one.
