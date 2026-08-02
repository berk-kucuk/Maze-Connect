#pragma once

namespace mazeconnect::core {

// Wire protocol version. Bumped on breaking changes to framing or to the
// message set; must stay in lock-step with Maze-Connect-Mobile's Version.kt.
//
// v2 replaced the KDE-Connect-style capabilities (clipboard, notifications,
// ping) with the Maze management ones. A v1 peer has nothing useful to say
// to a v2 peer, and the version check refuses the link outright rather than
// negotiating down.
constexpr int kProtocolVersion = 3;

int protocolVersion();

} // namespace mazeconnect::core
