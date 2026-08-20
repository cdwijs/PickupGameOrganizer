# Administrator / User Authentication System

A cryptographic design for a group with **3 administrators** and **10 regular
users** that satisfies the following properties:

1. Every message can be attributed to exactly one sender — the recipient can
   verify who sent it.
2. Only administrators can grant administrative access to another participant.
3. Only an administrator can remove an administrator.
4. Only administrators can change the list of administrators.
5. Being an administrator means holding a specific private key whose
   corresponding public key appears in the current admin roster.

The design is intentionally decentralized: it does not rely on a trusted
server. All authority is expressed as signatures over well-defined records.

---

## 1. Participants

Each participant owns an **Ed25519** identity key pair. The public key is the
participant's stable identifier; the private key is kept secret on the
participant's device.

The identifier of a participant is the SHA-256 of its public key, rendered as
hex (`fingerprint`). Names below are for readability only — the fingerprint is
the ground truth.

### Administrators (3)

| Name    | Role  | Public key (fingerprint, illustrative)                             |
|---------|-------|---------------------------------------------------------------------|
| Alice   | admin | `a1c3e0…9f21` |
| Bob     | admin | `b7d240…4e88` |
| Carol   | admin | `c0aa11…7712` |

### Users (10)

| Name    | Role | Public key (fingerprint, illustrative)                             |
|---------|------|---------------------------------------------------------------------|
| Dave    | user | `d0e1a4…5501` |
| Eve     | user | `e2b8c6…9930` |
| Frank   | user | `f11a03…7c4d` |
| Grace   | user | `03bb2e…2244` |
| Heidi   | user | `144d5a…88a1` |
| Ivan    | user | `25e6b7…c013` |
| Judy    | user | `36f809…d724` |
| Karl    | user | `47091b…e835` |
| Laura   | user | `58122c…f946` |
| Mallory | user | `69233d…0a57` |

---

## 2. Cryptographic primitives

| Purpose                              | Algorithm                    |
|--------------------------------------|------------------------------|
| Identity / signatures                | Ed25519                      |
| Fingerprints / content hashes        | SHA-256                      |
| Session key exchange (encryption)    | X25519                       |
| Symmetric message encryption         | ChaCha20-Poly1305 (AEAD)     |

Every participant therefore holds:

- An **Ed25519** signing key pair — long-term identity.
- An **X25519** key-exchange key pair — used to derive per-conversation
  symmetric keys via ECDH + HKDF.

Both public keys are bound together in the participant's signed identity
record so that a recipient who trusts the Ed25519 identity automatically
trusts the X25519 key.

---

## 3. Message format — property (1)

Every message on the wire is a signed envelope:

```
Message {
  sender_pubkey : 32 bytes      // Ed25519 public key of the sender
  timestamp     : u64           // milliseconds since epoch
  nonce         : 16 bytes      // random, prevents replay
  payload       : bytes         // plaintext, or AEAD ciphertext
  signature     : 64 bytes      // Ed25519(sender_priv, H(all fields above))
}
```

Verification:

1. Recompute `H = SHA-256(sender_pubkey ‖ timestamp ‖ nonce ‖ payload)`.
2. Check `Ed25519.verify(sender_pubkey, H, signature)`.
3. Reject if the timestamp is outside the accepted window, or if the
   `(sender_pubkey, nonce)` pair has already been seen (replay protection).

Because Ed25519 signatures are unforgeable without the private key, a valid
signature is proof that the holder of `sender_priv` produced this exact
payload — this satisfies **property (1)**.

Confidentiality is optional and orthogonal: when needed, `payload` is
`ChaCha20-Poly1305` ciphertext under a key derived from
`X25519(sender_x25519_priv, recipient_x25519_pub)` via HKDF. Signing happens
over the ciphertext (encrypt-then-sign at the transport layer), so
authenticity holds whether or not the message is encrypted.

---

## 4. The admin roster — properties (2)–(5)

The set of administrators is expressed as a signed record called the
**admin roster**. It is the single source of truth for "who is an admin
right now".

```
AdminRoster {
  version    : u64                  // monotonically increasing
  admins     : [pubkey]             // Ed25519 public keys of current admins
  prev_hash  : 32 bytes             // SHA-256 of the previous roster (or zero for genesis)
  timestamp  : u64
  signatures : [ (pubkey, sig) ]    // signatures by admins of the *previous* roster
}
```

### Genesis roster (version 0)

The initial roster lists Alice, Bob and Carol. It is bootstrapped
out-of-band — for example, it ships with the application, is fixed at group
creation time, or is signed by a group-founder key. Every participant pins
this genesis roster; it is the trust anchor.

### Updating the roster (add / remove an admin)

Any change is a new `AdminRoster` record with `version = prev.version + 1`
and `prev_hash = SHA-256(prev)`. It is accepted by a participant only if:

1. Its `signatures` field contains at least **M valid signatures from
   distinct admins listed in the previous roster** (`prev`), where `M` is
   the group's admin-threshold policy — e.g. `M = 1` (any single admin) or
   `M = 2` (a majority of 3).
2. `version` is exactly `prev.version + 1` and `prev_hash` matches.
3. Timestamp is not in the future beyond a small tolerance.

Because the *previous* roster is what authorizes the *next* one, and the
previous roster only contains admin public keys, **only administrators can
produce a valid roster update**. This covers:

- **Property (2)** — adding an admin requires publishing a new roster whose
  `admins` list contains the new key; that roster must be signed by
  existing admins. ⇒ only admins can grant admin access.
- **Property (3)** — removing an admin is the same operation, with the key
  omitted from `admins`. Signed by existing admins. ⇒ only an admin can
  remove an admin.
- **Property (4)** — any change to the admin list, additions or removals,
  goes through this signed-roster mechanism, so nobody else can change it.

### Being an admin — property (5)

A participant is an administrator **if and only if** their Ed25519 public
key appears in the current (highest-version, valid) `AdminRoster`. The
"key that gives them that right" is the Ed25519 private key corresponding
to that public key: without it, they cannot sign a roster update, so they
cannot exercise admin authority even if their name still appears.
Conversely, once the roster no longer lists their key, any roster update
they try to sign is rejected — the right vanishes with the key's removal
from the roster.

### Recommended admin-threshold policy

For 3 admins, `M = 2` (two-of-three) is a good default:

- No single compromised admin key can add attacker-controlled admins or
  remove honest admins.
- The group still functions if one admin is unavailable.

`M = 1` is simpler but strictly weaker: a single stolen admin key
compromises the whole group. `M = 3` is safest but blocks progress when
any admin is offline. This document assumes `M = 2` unless stated
otherwise.

---

## 5. End-to-end verification of an authoritative action

Suppose Bob (admin) wants to promote Dave to admin.

1. Bob constructs `AdminRoster { version = N+1, admins = [Alice, Bob, Carol, Dave], prev_hash = H(prev), … }`.
2. Bob signs it, then forwards it to Alice or Carol.
3. Alice reviews the change, signs it, attaches her signature.
4. The record now has 2 valid signatures from admins in roster `N` → it
   meets the `M = 2` threshold.
5. Bob (or anyone) broadcasts the new roster.

Every recipient:

- Loads their currently pinned roster (version `N`).
- Verifies each signature against a distinct admin pubkey in roster `N`.
- Verifies `version` and `prev_hash`.
- Accepts and pins roster `N+1`.

From this moment on, messages signed by Dave's key are accepted as
"messages from an admin" by anyone who has processed roster `N+1`.

---

## 6. Threat model & non-goals

**In scope**

- Forgery of messages (defeated by Ed25519 signatures).
- Unauthorized escalation to admin (defeated by the signed roster).
- Replay of messages (defeated by nonce + timestamp window).
- Silent rollback to an older roster (defeated by monotonic `version` and
  `prev_hash` chaining — each participant only accepts strictly higher
  versions that chain from the one they hold).

**Out of scope**

- Loss of an admin's private key. Recovery requires the remaining admins
  (still meeting the threshold) to publish a new roster that removes the
  lost key and, if desired, adds a replacement.
- Denial of service / network-level censorship of roster updates.
- Deniability of messages. Ed25519 signatures are non-repudiable by
  design; if deniability is a requirement, a separate MAC-based scheme
  would be needed.
