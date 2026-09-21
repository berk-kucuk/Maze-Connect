#pragma once

namespace mazeconnect::core {

// Wire protocol version. Bumped on breaking changes to framing or to the
// message set; must stay in lock-step with Maze-Connect-Mobile's Version.kt.
//
// v2 replaced the KDE-Connect-style capabilities (clipboard, notifications,
// ping) with the Maze management ones. A v1 peer has nothing useful to say
// to a v2 peer, and the version check refuses the link outright rather than
// negotiating down.
//
// v4 added the pairing commitment round: PairRequest now carries commit(nonce)
// and a third message, PairReveal, opens it. A v3 peer sends its nonce in the
// clear in PairRequest, which is what let a man-in-the-middle grind its own
// nonce until both screens showed the same code (see Sas.h). Refusing the link
// is the point: negotiating down to v3 would restore the very weakness, so an
// old peer must be updated rather than accommodated.
constexpr int kProtocolVersion = 4;

int protocolVersion();

} // namespace mazeconnect::core
