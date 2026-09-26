# Maze Connect threat model

Status: design outline, tracked alongside `docs/PROTOCOL.md`. This is the
checklist every capability plugin (`core/src/plugins/*`) and transport
change must be reviewed against before it ships.

| Threat | Mitigation |
|---|---|
| MITM during pairing | public-key-bound Short Authentication String, mandatory human comparison, no auto-accept (see "Design decisions" below) |
| MITM/rogue-AP after pairing | P-256 public-key pinning, hard-fail on mismatch, no silent re-TOFU |
| Replay | monotonic per-session counters + replay window on top of TLS 1.3 |
| Malicious/oversized payloads | per-message-type frame caps; reject before full buffering once the length prefix exceeds the cap |
| Path traversal / zip-slip in file transfer | reject `..`, path separators, NUL/control chars in filenames; Unicode-normalize; confine all writes under one per-device subdirectory; write to a random temp name then atomic-rename; resolve realpath post-write and verify it is still inside the confinement root; never follow symlinks |
| DoS from a rogue LAN peer | rate-limited discovery/pairing endpoints, per-peer connection limits, timeouts on unauthenticated handshake state, backpressure on file transfer |
| Downgrade | TLS 1.3 hard-pinned min=max, no cleartext socket ever, no protocol-version negotiation to a weaker dialect |
| Secure key storage (desktop) | long-term P-256 identity key written owner-only (0600) under the app data dir; a corrupt or unreadable key fails startup loudly rather than being silently regenerated, which would invalidate every pairing unnoticed |
| systemd sandboxing (daemon) | `systemd --user` unit — see `daemon/systemd/mazeconnectd.service` — `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome=read-only` with a single scoped `ReadWritePaths`, `PrivateTmp`, `PrivateDevices`, kernel/namespace/SUID restrictions, `MemoryDenyWriteExecute`, `SystemCallFilter=@system-service`, empty `CapabilityBoundingSet` |
| Cert pinning enforcement | custom `QSslSocket` peer-cert verification callback checking the pinned fingerprint — never rely on system CA trust for paired-peer connections |
| Reading this machine (`systemStatus`) | capability defaults **off** and is enabled per device, never globally; the snapshot comes from an unprivileged, read-only helper invoked with no arguments and no stdin, so nothing a peer sends reaches it; a report is only produced for a device that asked, and the capability is re-checked when the answer is sent, not only when it was requested |
| Feature surface | notification mirroring stays **removed**. Find-my-phone and phone → computer text came back in 1.3.0, each as its own capability with the limits below, not as a sync that runs by itself |
| Reading the phone (`phoneStatus`) | the phone decides: a per-device capability *and* an owner switch on the phone, refused out loud when off. Only battery, storage, memory, network type, ringer and model — no location, Wi-Fi name, identifiers, accounts or apps. Accepted here only from a device that was asked; every field type-checked, range-checked and matched against a closed enumeration (`phonestatus::sanitize`), dropped rather than defaulted when it fails |
| Text from a phone (`shareText`) | read with `Message::text()`: C0/C1 controls other than tab/LF/CR, and the bidi overrides/isolates that disguise a link, reject the whole field; 16 384 characters at most, refused rather than truncated; five per device per ten seconds. Placed on the clipboard with a notification and an Activity entry every time; an http(s) link is opened **only** by a click on that notification, never by the phone |
| Ringing a phone (`findPhone`) | per-device capability plus an owner switch on the phone; the ring stops by itself after two minutes and restores the alarm volume; the computer takes a `findPhoneResult` only while a ring it sent is outstanding |
| Pairing answers from the wrong link | a `pairResult`/`pairResponse` is honoured only from the link of the device being paired. (Desktop always keyed pending pairings by link; mobile 0.15.0 fixes the phone, where any unpaired peer could answer for a pairing in progress and have its own link marked trusted) |

Mirrors `Maze-Connect-Mobile`'s equivalent threat model; mobile-specific
items (AndroidKeyStore, BiometricPrompt gating, manifest permission
minimization, network security config) are tracked in that repo's own
`docs/THREAT_MODEL.md` and must stay consistent with this one wherever the
threat is shared (protocol-level threats above apply identically to both
clients).


## Design decisions that differ from the original plan

Both were forced by what the platforms actually expose, and both were
verified rather than assumed.

### The SAS is bound to public keys, not the TLS exporter secret

The plan called for binding the pairing code to the RFC 5705
`tls-exporter` secret. Neither Qt's `QSslSocket` nor Android's
`javax.net.ssl` exposes `SSL_export_keying_material`, so that binding is
unreachable without replacing the whole TLS stack on both clients.

Binding to both peers' long-term public keys preserves the property that
matters. A man-in-the-middle cannot forge either key, so it must terminate
TLS twice and present its own key to each side:

    Alice sees (A, M)  ->  code_A = H(A, M, nonces)
    Bob   sees (M, B)  ->  code_B = H(M, B, nonces)

The two codes differ, the on-screen comparison fails, and pairing aborts.
This is the same construction KDE Connect and Signal safety numbers use.
Covered by `TestCrypto::sasDetectsManInTheMiddle` and
`SasTest.detectsManInTheMiddle`.

**That argument is only true with the commitment round, and for a while it
was not there.** The nonce exchange used to be one round — the initiator
sent its nonce, the responder answered with its own — which let the
man-in-the-middle above choose its nonce *after* seeing the other side's.
It would complete the Bob half first, fixing `code_B`, then search its own
nonce until `code_A` came out equal: a 10^6 space, well under a second,
after which both users see the same six digits and confirm. Key binding
does not prevent this, because Mallory is not forging a key — it is
steering the only input it controls.

Pairing is therefore three messages, with the initiator committing to its
nonce before the responder picks one (see docs/PROTOCOL.md §Pairing).
Neither half of a man-in-the-middle can move its contribution after
learning the other's. `TestCrypto::commitmentStopsAGrindingManInTheMiddle`
and `SasTest.commitmentStopsAGrindingManInTheMiddle` perform the grind for
real and then assert the commitment refuses the result — the passive-relay
tests above would have stayed green throughout the weakness, which is why
they are not enough on their own.

### Device identity is EC P-256, not Ed25519

Three findings changed this, and the second is the security-relevant one:

1. Qt 6's `QSslKey` has no Ed25519 algorithm (`QSsl::KeyAlgorithm` is
   Rsa/Dsa/Ec/Dh/MlDsa), so an Ed25519 key cannot be handed to
   `QSslSocket` without an opaque-handle escape hatch.
2. Android's Keystore hardware-backs EC P-256 on essentially every
   shipping device; Ed25519 is not hardware-backed. Choosing P-256 is what
   actually lets the mobile private key live in the TEE/StrongBox and never
   enter app memory — worth far more here than any difference in the
   curves' own margins.
3. SubjectPublicKeyInfo DER is byte-identical between OpenSSL's
   `i2d_PUBKEY()` and Java's `PublicKey.getEncoded()`, so both clients
   derive identical fingerprints and identical pairing codes with no custom
   encoding to keep in sync.

The cross-platform agreement is pinned by a known-answer test asserted on
both sides: `TestCrypto::matchesCrossPlatformKnownAnswer` and
`SasTest.matchesKnownAnswerFromSharedConstruction` both require the code
`876154` for the same fixed input. If either derivation drifts, those fail
rather than the two clients silently failing to pair in the field.
