# Maze Connect wire protocol

Status: implemented. This document is the contract that
`core/src/{discovery,pairing,transport,protocol}` and the Kotlin
equivalents in Maze-Connect-Mobile both satisfy; the two implementations
must stay in lock-step (see `mazeconnect::core::kProtocolVersion` /
mobile `Version.kt`).

## Discovery

Unauthenticated, LAN-only, informational only. Periodic (~5s) UDP multicast
`announce` datagram on `239.255.83.10:38271` (subnet broadcast fallback if
multicast is filtered):

```json
{ "protocolVersion": 2, "deviceIdHash": "...", "deviceName": "...", "deviceType": "desktop|mobile", "pairingPort": 0 }
```

Nothing from this beacon is trusted beyond populating a "nearby devices"
list — no capability, filename, or identity claim in it is acted on before
pairing completes.

## Transport

TCP + TLS 1.3 only, hard-pinned `min == max` version (no TLS 1.2 fallback,
ever, on either stack). Mutual auth via self-signed X.509 certs wrapping
each device's long-term EC P-256 identity keypair.

Confidentiality and integrity of the payload come from TLS 1.3 itself.
There is deliberately no second encryption layer inside it: the plan
sketched one, but rolling an additional hand-built AEAD layer over an
already-authenticated channel adds key-management and nonce-reuse failure
modes without addressing any threat TLS 1.3 leaves open here. What *is*
layered on top is an application-level replay window (below), because
message ordering across reconnects is a property TLS does not give us.

## Pairing (TOFU + SAS)

1. User selects a discovered device; a TLS 1.3 connection opens with the
   peer cert intentionally unverified. This insecure window exists *only*
   inside the explicit, user-initiated pairing flow — never reachable from
   the normal post-pairing connection path.
2. Three messages, not two. The nonce exchange is **committed**: the
   initiator sends only a hash of its nonce first, and opens it after the
   responder has already sent its own.

   ```
   initiator -> responder : PairRequest  { commitment = commit(N_i) }
   responder -> initiator : PairResponse { nonce = N_r }
   initiator -> responder : PairReveal   { nonce = N_i }
   ```

   `commit(n) = SHA-256("maze-connect/sas-commit/v1" || len‖n)` — its own
   context string, so it can never collide with the SAS hash over the same
   nonce. The responder checks `commit(N_i)` against what it was sent and
   aborts the link on a mismatch, in constant time.

   Without this round the SAS is worth nothing: a man-in-the-middle running
   both halves finishes the far side first, fixing that code, then searches
   its own nonce until the near side's code matches. Six digits is 10^6 —
   under a second — and then both users see the same number and confirm.
   The commitment is what stops either side moving its contribution after
   learning the other's.
3. Both derive a 6-digit Short Authentication String from
   `SHA-256("maze-connect/sas/v1" || len‖initiatorKey || len‖responderKey ||
   len‖initiatorNonce || len‖responderNonce)`, taking the leading 31 bits
   mod 10^6. Every field is length-prefixed so concatenation is
   unambiguous. See docs/THREAT_MODEL.md for why this is bound to public
   keys rather than the TLS exporter secret.
4. Both users confirm the codes match on-screen. Only then is the peer's
   public-key fingerprint persisted to the pinned-device store.
5. Every later connection must match the pinned fingerprint exactly; on
   mismatch, hard-fail with a visible warning — never silently re-prompt
   TOFU.

## Framing

4-byte big-endian length-prefixed frames. A frame is `[1 byte type][4 bytes big-endian length][payload]`, where type
is `0x01` control or `0x02` data. Control-plane payloads are JSON
with a receiver-enforced max size (rejected before the full frame is
buffered if the length prefix alone exceeds the cap). File-transfer chunks use data frames carrying
`[4 bytes big-endian transferId][chunk]` — no base64 wrapping, and several
transfers may interleave. Per-session
monotonic counters + a replay window sit on top of TLS 1.3's own record
protection. Per-peer token-bucket rate limiting applies to pairing attempts
and new connections.

## Control-message envelope

Every control frame carries the same three envelope keys, kept short because
they are on every message:

```json
{ "v": 2, "t": "hello", "c": 1, ... }
```

* `v` — protocol version. A mismatch is a **hard stop**: the message is
  refused and there is no negotiation down to an older dialect.
* `t` — message type. An unrecognised name is a protocol violation, never
  guessed at.
* `c` — per-session monotonic counter. `0` is reserved, so an absent or
  zeroed counter can never be a valid first message.

Receivers re-validate every field they read: strings are length-bounded and
refused if they carry control characters, integers are range-checked, and
base64 fields must decode to exactly the expected length. No accessor ever
returns an unchecked peer-supplied value. The single exception is named
`unvalidatedObject()`, so that it cannot be reached for by accident — it is
used for the dashboard snapshot, whose fields are then bounded individually
by the consumer.

## Capability negotiation

`hello` must be the first message on any link; everything downstream — the
device id a pairing is filed under, the capability set, the paired-id
consistency check — is established by it.

```json
{
  "v": 2, "t": "hello", "c": 1,
  "deviceId": "...",
  "deviceName": "...",
  "deviceType": "desktop|mobile",
  "capabilities": ["fileTransfer", "systemStatus"],
  "appVersion": "..."
}
```

`capabilities` is what this build **implements**, which is not the same as
what the peer may use. A capability only arms when both peers advertise it
*and* the local user has enabled it for that specific device. An
advertisement alone grants nothing, and pairing grants nothing beyond
identity.

| Capability | Default | Messages |
| --- | --- | --- |
| `fileTransfer` | on — every incoming file still prompts | `fileOffer` / `fileAccept` / `fileReject` / `fileComplete` / `fileCancel` |
| `systemStatus` | **off** | `statusRequest` → `statusReport` |

Planned, in order: `commands` (`commandList`/`commandCatalog`,
`commandRun`/`commandResult`), `guardControl` (`guardRequest`/`guardResult`)
and `ai` (`aiModels`/`aiModelList`, `aiPrompt` → `aiChunk`* → `aiDone`).

## systemStatus

`statusRequest` carries nothing but the envelope. That is the whole of the
requester's influence: it cannot name a probe, pass an argument, or make the
desktop's status helper run more often than that helper's own cache allows.

`statusReport` answers with **either** a snapshot or a reason there isn't
one, as one message type rather than two:

```json
{ "v": 2, "t": "statusReport", "c": 9, "status": { "hostname": "...", "metrics": [...], ... } }
{ "v": 2, "t": "statusReport", "c": 9, "error": "maze-tools is not installed" }
```

The snapshot is nested under `status` rather than flattened into the
envelope, so no future field of it can collide with `v`, `t` or `c`.

A report is only accepted from a device the receiver actually asked. Being
paired is not by itself a licence to put content on someone's screen.

## Scope

v2 dropped notification mirroring, clipboard sync and ping/find-my-device:
KDE Connect already does those. What remains is managing the machine itself,
plus file transfer. Deferred indefinitely: media/MPRIS control, battery
sharing, remote input, SMS.
