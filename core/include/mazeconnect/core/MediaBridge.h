#pragma once

#include <QDBusConnection>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>

class QDBusMessage;
class QProcess;

namespace mazeconnect::core {

/// The whole vocabulary a phone has for this machine's media. Anything not
/// in this table cannot be asked for — there is no free-form method name,
/// no argv and no bus name anywhere in a media command.
enum class MediaAction {
    Play,
    Pause,
    PlayPause,
    Next,
    Previous,
    Stop,
    Seek,         ///< value = position in milliseconds
    SetVolume,    ///< value = player volume, 0..100
    SystemVolume, ///< value = default output volume, 0..100
    SystemMute,   ///< value = 1 to mute the default output, 0 to unmute
};

QString mediaActionName(MediaAction action);
bool mediaActionFromName(const QString &name, MediaAction &action);

/**
 * The computer's media players, over MPRIS, plus the default output volume.
 *
 * MPRIS is the freedesktop interface every Linux player that shows up in the
 * Plasma media applet speaks — Spotify, Firefox and Chromium tabs, VLC, mpv
 * with its plugin, Elisa, Strawberry — so one implementation covers all of
 * them without knowing about any of them.
 *
 * **Nothing here blocks.** Every bus call is asynchronous with a short
 * timeout. A player that hangs (a browser busy on its main thread is the
 * common case) would otherwise freeze the whole link for the D-Bus default
 * of 25 seconds — heartbeats included — for the sake of reading a song title.
 *
 * **Started lazily**, on the first request from a paired device. Merely
 * constructing the bridge touches no bus, so a machine with no session bus
 * (a build chroot, the test suite) never tries to find or launch one.
 *
 * What a phone can reach through this class is deliberately small: the
 * players this class itself found, identified by an id it derived from their
 * bus name, and the ten actions in MediaAction. The volume tool is run with
 * a fixed argv and a clamped number; there is no shell.
 */
class MediaBridge : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxPlayers = 12;
    static constexpr int kMaxTextChars = 256;
    static constexpr int kMaxIdChars = 64;
    static constexpr int kCallTimeoutMs = 1500;

    /// The session bus, connected on first use.
    explicit MediaBridge(QObject *parent = nullptr);

    /// A given bus — the tests run a private one.
    MediaBridge(const QDBusConnection &bus, QObject *parent = nullptr);

    ~MediaBridge() override;

    /// Program used for the default output volume, found on PATH by default.
    /// An empty string disables system volume (tests, machines without it).
    void setVolumeTool(const QString &program);

    /// Connect to the bus and start watching players. Idempotent. Returns
    /// false when there is no session bus to watch.
    bool ensureStarted();

    bool isAvailable() const { return m_available; }
    QString unavailableReason() const { return m_unavailableReason; }

    /// Someone is subscribed: poll what has no change signal (the output
    /// volume) so a change made on the computer still reaches the phone.
    void setWatching(bool watching);

    /// Re-read every player and the volume. changed() follows once the
    /// answers are in (or have timed out).
    void refresh();

    /// The current state, in the shape mediaState carries.
    QJsonObject snapshot() const;

    /**
     * Carry out one action. Returns false, with @p error set, when it cannot
     * even be attempted: unknown player, a capability the player does not
     * have, a value out of range. A true return means the call was sent —
     * the resulting state arrives through changed(), like any other change.
     */
    bool command(const QString &playerId, MediaAction action, qint64 value, QString &error);

    // ---- exposed for tests ------------------------------------------------

    /// "org.mpris.MediaPlayer2.spotify" -> "spotify"; empty if the name is
    /// not an MPRIS name or its suffix is not a plain id.
    static QString playerIdFromBusName(const QString &busName);

    /// Control characters removed, bounded to kMaxTextChars.
    static QString cleanText(const QString &text);

    /// Parse `wpctl get-volume` output: "Volume: 0.45 [MUTED]".
    static bool parseWpctlVolume(const QByteArray &output, int &percent, bool &muted);

signals:
    /// The state changed; read snapshot(). Coalesced — a burst of property
    /// changes from one track skip becomes one signal.
    void changed();

private Q_SLOTS:
    void onNameOwnerChanged(const QString &name, const QString &oldOwner,
                            const QString &newOwner);
    void onPropertiesChanged(const QDBusMessage &message);
    void onSeeked(const QDBusMessage &message);

private:
    struct Player {
        QString busName;
        QString id;
        QString owner; ///< unique name — what signals arrive from
        QString identity;
        QString status; ///< "playing" | "paused" | "stopped"
        QString title;
        QString artist;
        QString album;
        QString trackId;
        qint64 lengthUs = 0;
        qint64 positionUs = 0;
        double volume = -1.0; ///< -1: the player does not expose one
        bool canPlay = false;
        bool canPause = false;
        bool canNext = false;
        bool canPrevious = false;
        bool canSeek = false;
        bool canControl = false;
        qint64 lastActivity = 0; ///< for choosing which player the phone shows
        bool refreshing = false;
        bool dirty = false;
    };

    void listPlayers();
    void addPlayer(const QString &busName, const QString &owner);
    void refreshPlayer(const QString &busName);
    void applyPlayerProperties(Player &player, const QVariantMap &properties);
    void refreshSystemVolume();
    void runVolumeTool(const QStringList &arguments, bool readBack);
    void callPlayer(const Player &player, const QString &method,
                    const QList<QVariant> &arguments = {});
    void scheduleChanged();
    Player *playerById(const QString &id);
    QString busNameForOwner(const QString &owner) const;

    QDBusConnection m_bus;
    bool m_useSessionBus = false;
    bool m_started = false;
    bool m_available = false;
    QString m_unavailableReason;

    QMap<QString, Player> m_players; ///< by bus name
    qint64 m_activityCounter = 0;

    QString m_volumeTool;
    bool m_volumeToolSet = false;
    int m_systemVolume = -1; ///< -1: unknown / no tool
    bool m_systemMuted = false;
    QProcess *m_volumeRead = nullptr;

    QTimer m_changedDebounce;
    QTimer m_volumePoll;
};

} // namespace mazeconnect::core
