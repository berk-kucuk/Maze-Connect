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
| `systemStatus` | on | `statusRequest` → `statusReport` / `statusUnchanged` |
| `commands` | on | `commandList` → `commandCatalog`, `commandRun` → `commandResult` |
| `ai` | on | `aiModels` → `aiModelList`, `aiPrompt` → `aiChunk`* → `aiDone` |
| `guardControl` | on | `guardStatus` → `guardReport`, `guardRequest` → `guardResult` |
| `openOnPhone` | on | `openOnPhone` (computer → phone only) |
| `media` | on | `mediaRequest` → `mediaState`*, `mediaCommand` |
| `phoneStatus` | on — the phone owner can switch it off | `phoneStatusRequest` → `phoneStatus` (computer asks, phone answers) |
| `findPhone` | on — the phone owner can switch it off | `findPhone` → `findPhoneResult`* (computer → phone) |
| `shareText` | on | `shareText` (phone → computer only) |

Every capability is granted by pairing and revocable per device on the
computer. A message is only ever sent to a peer that advertised the
capability it belongs to, so a capability can be added without a protocol
version bump: an older peer never advertises it and is never sent it.

Transfer ids are scoped to the device that offered them. A `fileAccept`,
`fileCancel`, `fileComplete` or data frame from any other device for the
same id is ignored (or, for a data frame, drops that link).

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

## media

The computer's MPRIS players (anything in the desktop's own media controls)
and its default output volume.

`mediaRequest` carries `subscribe`. It is always answered with one
`mediaState`; with `subscribe: true` the computer also pushes a `mediaState`
whenever a player changes, until a request with `false` arrives or the link
drops. Only a device that asked may have a `mediaState` shown.

```json
{ "v": 4, "t": "mediaRequest", "c": 4, "subscribe": true }
{ "v": 4, "t": "mediaState", "c": 7, "media": {
    "players": [ { "id": "spotify", "name": "Spotify", "status": "playing",
                   "title": "…", "artist": "…", "album": "…",
                   "lengthMs": 231000, "positionMs": 12000,
                   "canPlay": true, "canPause": true, "canNext": true,
                   "canPrevious": false, "canSeek": true, "volume": 50 } ],
    "active": "spotify", "systemVolume": 40, "systemMuted": false,
    "notice": "Spotify has no previous track" } }
{ "v": 4, "t": "mediaState", "c": 7, "media": { "error": "no desktop session bus" } }
```

`positionMs` is the position when the state was sent; the phone moves it
forward itself while `status` is `playing`. `volume` is absent for a player
without its own volume; `systemVolume` is absent when the computer has no
volume tool.

`mediaCommand` carries `player` (an `id` from the last `mediaState`),
`action` and `value`:

| `action` | `value` |
| --- | --- |
| `play`, `pause`, `playPause`, `next`, `previous`, `stop` | ignored |
| `seek` | position in ms (clamped to the track) |
| `setVolume` | player volume, 0–100 |
| `systemVolume` | output volume, 0–100 (`player` ignored) |
| `systemMute` | 1 mute, 0 unmute (`player` ignored) |

Both strings are looked up on the computer — the action in a fixed table,
the player among the ones it found itself — and never used to build a bus
name, a method or an argv. A command that cannot be carried out is answered
with a `mediaState` whose `notice` says why; a command that worked is
answered by the change itself. More than a few commands a second from one
device are dropped unanswered.

## phoneStatus

The phone's own reading, for the computer's dashboard. The direction is the
reverse of `systemStatus`: the **computer** asks and the **phone** answers.

`phoneStatusRequest` carries nothing but the envelope. `phoneStatus` answers
with either a reading nested under `status`, or `error`:

```json
{ "v": 4, "t": "phoneStatus", "c": 9, "status": {
    "battery": { "level": 82, "charging": true, "plug": "ac",
                 "temperature": 314, "health": "good" },
    "storage": { "free": 49392123904, "total": 274877906944 },
    "memory":  { "available": 3221225472, "total": 8589934592 },
    "network": { "type": "wifi", "signal": 3, "metered": false },
    "ringer": "vibrate", "dnd": true, "powerSave": false, "screenOn": true,
    "model": "SM-S911B", "manufacturer": "samsung", "android": "15",
    "uptimeMs": 277200000 } }
{ "v": 4, "t": "phoneStatus", "c": 9, "error": "this phone has status sharing switched off" }
```

Every field is optional. `temperature` is tenths of a degree Celsius, as
Android reports it; sizes are bytes; `signal` is 0-4 bars and only sent for
Wi-Fi. The enumerations are closed:

| Field | Values |
| --- | --- |
| `battery.plug` | `ac`, `usb`, `wireless`, `dock`, `none` |
| `battery.health` | `good`, `overheat`, `dead`, `cold`, `overvoltage`, `failure`, `unknown` |
| `network.type` | `wifi`, `cellular`, `ethernet`, `vpn`, `none` |
| `ringer` | `normal`, `vibrate`, `silent` |

What is deliberately **not** in it: the Wi-Fi network name (it needs location
permission and says where the phone is), location, IMEI, phone number,
accounts, contacts, messages, installed apps.

The computer accepts a `phoneStatus` only from a device it asked, and passes
it through `phonestatus::sanitize()`, which copies a field only if it has the
right type, is in range and — for the enumerations — is one of the values
above. A field that fails is absent, never defaulted: a battery drawn as 0%
because the reading was malformed would be a false alarm.

The phone answers either way: a refusal carries `error`, so the dashboard can
say "sharing is off on the phone" rather than wait. A phone caches its
reading for two seconds, so a fast poll costs nothing.

## findPhone

Make a phone ring so it can be found — on the alarm stream at full volume,
which is audible in silent and vibrate mode, with vibration, until someone
taps "Found it", the computer stops it, or two minutes pass.

```json
{ "v": 4, "t": "findPhone", "c": 5, "ring": true }
{ "v": 4, "t": "findPhoneResult", "c": 6, "ringing": true }
{ "v": 4, "t": "findPhoneResult", "c": 7, "ringing": false }
{ "v": 4, "t": "findPhoneResult", "c": 8, "ringing": false, "error": "ringing is switched off on this phone" }
```

The phone answers every `findPhone`, and sends `ringing: false` again when
the ringing stops on its side — tapped, timed out, or unable to play — so
the computer never shows "Stop ringing" for a quiet phone. The computer
accepts a `findPhoneResult` only while it has a ring outstanding to that
device; after a `ringing: false`, nothing more is taken until it rings again.

## shareText

Text or a link from the phone — its clipboard, or anything shared to Maze
Connect from another app — for the computer's clipboard.

```json
{ "v": 4, "t": "shareText", "c": 11, "text": "https://example.com/article" }
```

Read with `Message::text()` rather than `string()`: tab, line feed and
carriage return are allowed, because a clipboard is rarely one line. Every
other control character is refused, and so are the bidirectional overrides
and isolates (U+202A-202E, U+2066-2069), which are how a link is made to
display as somewhere it does not go. At most 16 384 characters — longer is
refused, never truncated. At most five per device in ten seconds; the rest
are dropped, so a phone cannot keep overwriting the clipboard.

The computer puts the text on its clipboard and raises a notification. An
http(s) link is **opened only if the user clicks that notification** — a
paired phone can offer a link, never open one.

## Scope

v2 dropped notification mirroring, clipboard sync and ping/find-my-device:
KDE Connect already does those. What remains is managing the machine itself,
file transfer, and — since desktop 1.2.0 / mobile 0.14.0 — its media
players. Desktop 1.3.0 / mobile 0.15.0 added the phone's side: its status
on the computer's dashboard, find-my-phone, and sharing text to the
computer. Deferred: remote input, SMS.
