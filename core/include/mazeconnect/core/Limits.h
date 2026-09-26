#pragma once

#include <QtGlobal>

namespace mazeconnect::core {

// Every limit here is enforced by the *receiver* on untrusted input. None of
// them are ever taken from a value the peer sent — a peer-supplied "size"
// field is only ever checked against these, never trusted in place of them.
// Kept in lock-step with the mobile client's Limits.kt.
namespace limits {

// Largest control-plane (JSON) frame. Anything bigger is rejected from the
// length prefix alone, before a single payload byte is buffered.
inline constexpr quint32 kMaxControlFrame = 512 * 1024; // 512 KiB

// Largest single file-transfer data frame.
inline constexpr quint32 kMaxDataFrame = 256 * 1024; // 256 KiB

// Absolute frame ceiling used by the framing layer itself.
inline constexpr quint32 kMaxFrame = kMaxControlFrame;

// Largest accepted incoming file. A sender advertising more is refused up
// front rather than part-way through.
inline constexpr qint64 kMaxFileBytes = 2LL * 1024 * 1024 * 1024; // 2 GiB

// Discovery beacon datagrams are tiny; anything larger is not ours.
inline constexpr int kMaxBeaconDatagram = 2048;

// Bounds on peer-supplied display strings, applied after UTF-8 validation.
inline constexpr int kMaxDeviceNameChars = 64;
inline constexpr int kMaxDeviceIdChars = 64;
inline constexpr int kMaxFilenameChars = 255;

// Longest clipboard text "open on phone" will carry. Generous next to a
// real URL, small enough that a giant clipboard blob cannot turn into an
// outsized frame — refused outright rather than truncated, since a cut-off
// URL is broken rather than merely shorter.
inline constexpr int kMaxOpenTextChars = 4096;

// Longest text a phone may share to this computer's clipboard. Larger than
// the link-sized bound above because a shared note is a real use, still far
// under the control-frame cap. Refused, not truncated, for the same reason.
inline constexpr int kMaxShareTextChars = 16384;

// At most this many shared texts per device inside kShareTextWindowMs. A
// clipboard that a phone can overwrite in a loop is a clipboard the user
// cannot use.
inline constexpr int kMaxShareTextsPerWindow = 5;
inline constexpr int kShareTextWindowMs = 10000;

// Handshake must complete within this window or the socket is dropped, so a
// peer cannot pin resources by opening connections and stalling.
inline constexpr int kHandshakeTimeoutMs = 15000;

// An established link with nothing to say sends a Ping after this much
// idle time, and is dropped if nothing — not even a Pong — has arrived
// within the timeout (three missed pings). TCP alone does not catch a
// half-open connection: a link can survive a Wi-Fi reassociation, a NAT
// table eviction, or a DHCP lease change with no FIN/RST ever arriving on
// either side, and without this it can sit "connected" and dead until the
// app is restarted.
inline constexpr int kHeartbeatIntervalMs = 15000;
inline constexpr int kHeartbeatTimeoutMs = 45000;

// Replay window depth: how many out-of-order counters we tolerate.
inline constexpr int kReplayWindowSize = 512;

// Per-peer rate limiting on connection attempts.
inline constexpr int kMaxConnectionsPerPeer = 4;
inline constexpr int kPairingAttemptsPerMinute = 5;

} // namespace limits
} // namespace mazeconnect::core
