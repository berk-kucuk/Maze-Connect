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
    Media = 0x40,        ///< see and control this computer's media players
    PhoneStatus = 0x80,  ///< read a paired phone's battery, storage and network
    FindPhone = 0x100,   ///< make a paired phone ring, even when it is silenced
    ShareText = 0x200,   ///< a phone sends text or a link to this computer's clipboard
    RemoteInput = 0x400, ///< a phone moves the pointer and types — **opt-in per device**
    Presenter = 0x800,   ///< a phone presses a fixed handful of slide keys
    ClipboardSync = 0x1000, ///< clipboards follow each other, when both owners switch it on
    SharedFolder = 0x2000,  ///< a phone browses and downloads from one shared folder
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
        | Capability::OpenOnPhone | Capability::Media | Capability::PhoneStatus
        | Capability::FindPhone | Capability::ShareText | Capability::RemoteInput
        | Capability::Presenter | Capability::ClipboardSync | Capability::SharedFolder;
}

/**
 * Capabilities that pairing does **not** grant: each has to be switched on
 * for a device, on this computer, by its owner.
 *
 * RemoteInput is the one: a phone that can type can open a terminal and run
 * anything as this user. Everything else a phone can do here is bounded by a
 * fixed table or a file the owner wrote; this is not, so it is not a default
 * — neither for a new pairing nor for an older pairing that has never heard
 * of it (see DeviceStore::load()).
 */
inline constexpr Capabilities optInCapabilities() {
    return Capabilities(Capability::RemoteInput);
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
    return supportedCapabilities() & ~optInCapabilities();
}

/**
 * Every capability that existed before a pairing record started saying which
 * capabilities it knew about (desktop 1.2.0). Used only to read such older
 * records — see DeviceStore::load().
 */
inline constexpr Capabilities legacyKnownCapabilities() {
    return Capabilities(Capability::FileTransfer) | Capability::SystemStatus
        | Capability::Commands | Capability::Ai | Capability::GuardControl
        | Capability::OpenOnPhone;
}

/// Wire names. Unknown names from a peer are ignored, never guessed at.
QString capabilityName(Capability capability);
Capability capabilityFromName(const QString &name);

QStringList capabilitiesToNames(Capabilities capabilities);
Capabilities capabilitiesFromNames(const QStringList &names);

} // namespace mazeconnect::core
