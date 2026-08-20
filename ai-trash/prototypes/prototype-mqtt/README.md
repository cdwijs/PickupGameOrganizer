# prototype-mqtt

End-to-end encrypted messaging between two peers via a public MQTT broker.
Both peers share an AES-GCM key out-of-band; the broker only sees ciphertext,
so it acts purely as a rendezvous point.

## Running

Serve the directory over HTTP (any static server works; a browser opening
`index.html` directly over `file://` will not be able to load the MQTT
library from unpkg via HTTPS in all cases). E.g.:

```
cd prototype-mqtt
python3 -m http.server 8080
# then open http://localhost:8080
```

## How to test with two peers

1. Open the page in two browser windows (or two devices).
2. Pick the same broker preset on both (defaults to freemqtt.com; if that's
   blocked on your network, try `broker.emqx.io`, `broker.hivemq.com`, or
   `test.mosquitto.org` from the dropdown — those three are anonymous, no
   credentials needed).
3. Press **Generate new key** on peer A, copy the key, paste into peer B.
4. On peer B, swap the *Publish* and *Subscribe* topics so the two peers'
   channels cross (A publishes to `msg-a`, subscribes to `msg-b`; B does the
   reverse).
5. Press **Connect** on both.
6. Type a message on either side and press **Send**.

## What's implemented

- WSS transport — plaintext MQTT and `ws://` are not used because browsers
  can only reach the broker over WebSocket, and TLS is the sensible default
  for anything crossing the internet.
- Broker preset dropdown (freemqtt, emqx, hivemq, mosquitto, or custom) so
  you can swap brokers without hand-editing URL/user/pass. Editing any of
  those fields flips the preset to *custom* so subsequent renders don't
  clobber your edits.
- Random client ID per session (prevents "same client ID" disconnect loops).
- QoS 1 by default, no retained messages, `dev/…` topic namespace.
- Last-Will (LWT) preconfigured with topic `dev/prototype/lwt` and payload
  `offline` so unexpected disconnects are visible to the peer.
- Exponential backoff (1 s → 30 s) with 30 % random jitter between
  reconnect attempts.
- 64 KB client-side payload cap.
- AES-GCM-256 encryption in the browser via Web Crypto; wire format is
  `[IV (12 B) ‖ ciphertext ‖ auth-tag (16 B)]` base64-encoded.

## What's not implemented

- No key exchange — the key must be moved between peers by hand (paste
  into the key field on both sides). See the parent repo for the pairing
  prototypes that would sit in front of this.
- No message authentication of the sender identity beyond the shared
  symmetric key (anyone with the key can publish as either peer).
- No offline queue: messages typed while disconnected aren't sent when
  the connection comes back.

## Files

- `index.html` — UI layout and styles
- `app.js` — MQTT client, crypto, event wiring
- `README.md` — this file
