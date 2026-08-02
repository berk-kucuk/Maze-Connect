#pragma once

#include <QMap>
#include <QObject>
#include <QQueue>
#include <QString>

class QLocalSocket;
class QTimer;

namespace mazeconnect::core {

/// The killswitches maze-guardd knows about. A fixed enum, never a string.
enum class GuardDevice {
    Camera,
    Microphone,
    Bluetooth,
    Wifi,
    Usb,
};

/// Three real states plus "we could not tell". `Unavailable` is maze-guardd's
/// own `none` — the machine has no such device — and is not the same as off.
enum class GuardState {
    On,
    Off,
    Unavailable,
    Unknown,
};

/// Wire names, matched exactly. An unknown name is refused, never guessed at.
QString guardDeviceName(GuardDevice device);
bool guardDeviceFromName(const QString &name, GuardDevice &out);
QString guardStateName(GuardState state);

/**
 * A deliberately narrow bridge to maze-guardd.
 *
 * **This class is the most security-sensitive thing in the project**, because
 * of what maze-guardd says about itself:
 *
 *   > Callers are authorised by SO_PEERCRED: the peer must be a real user with
 *   > an ACTIVE local session (mirrors polkit allow_active) — not root, not
 *   > remote.
 *
 * Maze Connect toggling a killswitch makes it the **remote proxy for a broker
 * built specifically to refuse remote callers**. The user accepted that for
 * on/off. What makes it defensible is everything below, and none of it is
 * optional:
 *
 * 1. **`PANIC` and `RESTORE` are not gated — they are absent.** There is no
 *    code path here that can produce either word. They are destructive and
 *    should require being in front of the machine. `tst_guardbridge` asserts
 *    this against the bytes actually written to the socket.
 * 2. The verb table is fixed: `STATUS` and `KILL <device> <on|off>`, nothing
 *    else, ever.
 * 3. The device is a C++ enum. Nothing a peer sends is ever concatenated into
 *    the command line, so there is no newline to inject a second verb with and
 *    no argument to smuggle anything through.
 * 4. The capability is revocable per device from the computer's own Devices
 *    page, and a revocation is re-checked when an answer is delivered rather
 *    than only when it was asked for.
 * 5. Every privileged action is logged and announced on the desktop, so a
 *    remote toggle is never silent.
 *
 * Mirroring maze-guardd's own discipline is the point: a broker that refuses
 * to interpret caller strings should not be fronted by a proxy that does.
 */
class GuardBridge : public QObject {
    Q_OBJECT

public:
    explicit GuardBridge(QObject *parent = nullptr);
    ~GuardBridge() override;

    /// `/run/maze/guard.sock`. $MAZECONNECT_GUARD_SOCKET overrides it for
    /// tests; that is not a privilege boundary, since anyone able to set this
    /// process's environment can already run code as this user.
    static QString socketPath();

    /// False when maze-tools is not installed or maze-guardd is not running.
    /// The UI then says so rather than showing switches that do nothing.
    bool isAvailable() const;

    /// Ask for every killswitch's state. Answers with statusReady() or
    /// statusFailed().
    void requestStatus();

    /**
     * Allow or block one device.
     *
     * @p enabled is the **device's** state, not the killswitch's:
     *   * `true`  → the device works (`KILL <dev> on`, `rfkill unblock`)
     *   * `false` → the device is blocked (`KILL <dev> off`, `rfkill block`)
     *
     * This reads backwards if you think of "on" as "protection on", and that
     * is exactly the mistake this comment exists to prevent — it shipped
     * once. Pressing **Block** sent `KILL wifi on`, which *unblocked* wifi:
     * the Activity log dutifully recorded "asked for on — now on" while the
     * user watched the opposite of what they pressed.
     *
     * The parameter is named after the device rather than the switch because
     * that is the vocabulary maze-guardd and `rfkill` already use, and having
     * two vocabularies for one boolean is what caused the inversion.
     */
    void setDeviceEnabled(GuardDevice device, bool enabled);

    /// Longest we wait on the broker before giving up. maze-guardd shells out
    /// to rfkill/systemctl, so this is not instant.
    static constexpr int kTimeoutMs = 15000;

    /// Replies are one short line; anything larger is not maze-guardd.
    static constexpr int kMaxReplyBytes = 4096;

signals:
    void statusReady(const QMap<mazeconnect::core::GuardDevice,
                                mazeconnect::core::GuardState> &states);
    void statusFailed(const QString &reason);

    /// A change finished. @p requestedEnabled is what was asked for — the
    /// *device's* state, see setDeviceEnabled(). @p state is what the broker
    /// reports afterwards, which is not always the same thing.
    void killApplied(mazeconnect::core::GuardDevice device, bool requestedEnabled,
                     mazeconnect::core::GuardState state, const QString &error);

private:
    /**
     * One exchange with the broker: a line out, a line back.
     *
     * Asynchronous on purpose. maze-guardd shells out to rfkill, systemctl and
     * modprobe, and gives *itself* a 60-second budget for those; a blocking
     * read here would freeze the desktop for as long as the slowest of them
     * takes. Requests are queued and served one at a time, which also matches
     * the rule that there is never more than one privileged change in flight.
     */
    struct Pending {
        QString command;
        /// True when this exchange is the STATUS that follows a KILL, in
        /// which case the fields below say what the KILL asked for.
        bool isKillFollowUp = false;
        bool isKill = false;
        GuardDevice device{};
        bool requestedEnabled = false;
    };

    void enqueue(const Pending &pending);
    void startNext();
    void handleReply(const QString &reply);
    void failCurrent(const QString &error);
    void finishCurrent();

    QMap<GuardDevice, GuardState> parseStatus(const QString &reply) const;

    QQueue<Pending> m_queue;
    Pending m_current;
    QLocalSocket *m_socket = nullptr;
    QTimer *m_timeout = nullptr;
    QByteArray m_buffer;
};

} // namespace mazeconnect::core

Q_DECLARE_METATYPE(mazeconnect::core::GuardDevice)
Q_DECLARE_METATYPE(mazeconnect::core::GuardState)
