#pragma once

#include <QFlags>
#include <QString>
#include <QStringList>

namespace mazeconnect::core {

/**
 * Optional features a device can offer. A capability only becomes active
 * when *both* peers advertise it and the local user has enabled it — an
 * advertisement alone never grants anything.
 */
enum class Capability : quint32 {
    None = 0x00,
    FileTransfer = 0x01,
    SystemStatus = 0x02, ///< read-only dashboard snapshot of this machine
    Commands = 0x04,     ///< run entries from the user's own allow-list
    Ai = 0x08,           ///< chat with Maze AI through the local Ollama
    GuardControl = 0x10, ///< toggle maze-guard killswitches (privileged)
    OpenOnPhone = 0x20,  ///< push the desktop's clipboard to the phone to open
};
Q_DECLARE_FLAGS(Capabilities, Capability)
Q_DECLARE_OPERATORS_FOR_FLAGS(Capabilities)

/**
 * What this build can do at all — the set advertised in `hello`.
 *
 * Distinct from defaultEnabledCapabilities() below, and the distinction is
 * load-bearing: advertising says "I implement this", enabling says "you may
 * use it". They were the same set while FileTransfer was the only capability,
 * which made it easy to miss that they answer different questions.
 */
inline constexpr Capabilities supportedCapabilities() {
    return Capabilities(Capability::FileTransfer) | Capability::SystemStatus
        | Capability::Commands | Capability::Ai | Capability::GuardControl
        | Capability::OpenOnPhone;
}

/**
 * What a newly paired device may do: **everything this build implements.**
 *
 * These used to default off, one switch per capability per device. That was
 * the wrong trade. Pairing already requires a person to compare a six-digit
 * code on two screens and agree on both — that is the deliberate act, and it
 * is where the trust decision belongs. Requiring every feature to be opted
 * into afterwards did not add a decision; it added a wall. A freshly paired
 * phone showed "asking the computer…" on four screens indefinitely, with
 * nothing on either device explaining why.
 *
 * The switches still exist and still work: the desktop's Devices page revokes
 * any of them per device, and a revocation takes effect immediately — it is
 * re-checked when an answer is delivered, not only when it was asked for.
 * What changed is the starting position, not the control.
 *
 * **This includes GuardControl, which is the part that matters.** A paired
 * phone can now toggle killswitches with no further step. What still holds it
 * in: PANIC and RESTORE cannot be expressed in the protocol at all, the
 * device table is a fixed enum, the phone confirms in both directions, every
 * change is written to the Activity log with the device that asked, and the
 * desktop raises a banner that does not fade.
 */
inline constexpr Capabilities defaultEnabledCapabilities() {
    return supportedCapabilities();
}

/// Wire names. Unknown names from a peer are ignored, never guessed at.
QString capabilityName(Capability capability);
Capability capabilityFromName(const QString &name);

QStringList capabilitiesToNames(Capabilities capabilities);
Capabilities capabilitiesFromNames(const QStringList &names);

} // namespace mazeconnect::core
