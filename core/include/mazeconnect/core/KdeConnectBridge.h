#pragma once

#include <QDBusConnection>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

class QDBusMessage;
class QDBusServiceWatcher;

namespace mazeconnect::core {

/// One device as KDE Connect's daemon reports it. Only what the UI shows and
/// what decides which actions are offered — no key material, no addresses.
struct KdeConnectDevice {
    QString id;
    QString name;
    QString type; ///< "smartphone", "tablet", "desktop", "laptop", "tv" — KDE's words
    bool reachable = false;
    bool paired = false;
    bool pairRequested = false;       ///< we asked, waiting for the other side
    bool pairRequestedByPeer = false; ///< they asked, waiting for an answer here
    QString verificationKey;          ///< shown on both screens while pairing
    int battery = -1;                 ///< -1: no reading
    bool charging = false;
    bool canPing = false;
    bool canRing = false;
    bool canShare = false;
    bool canClipboard = false;
};

/**
 * KDE Connect compatibility: the system's own kdeconnectd, switched on or off
 * from Maze Connect and driven over its D-Bus interface.
 *
 * **Why not speak the protocol here.** KDE Connect clients exist for Android,
 * iOS, Windows and macOS, and the protocol, its pairing and its TLS are
 * maintained by KDE. Re-implementing them would add a second, younger copy of
 * all that network and crypto code to this process. Driving the real daemon
 * gives full compatibility with every client and keeps Maze Connect's own
 * link — and its guarantees — exactly as it is.
 *
 * **On and off are files, not a setting.** "Off" writes two per-user
 * overrides, both marked as written by Maze Connect:
 *   - `~/.config/autostart/org.kde.kdeconnect.daemon.desktop` with
 *     `Hidden=true`, so the session no longer starts the daemon;
 *   - `~/.local/share/dbus-1/services/org.kde.kdeconnect.service` with
 *     `Exec=/bin/false`, so nothing — the Plasma applet, Dolphin's share menu —
 *     can bring it back by D-Bus activation. The user's directory comes before
 *     /usr/share in the bus's search path, so this one wins.
 * "On" removes them again. The state shown is therefore what is on disk, not
 * what this process remembers, and a machine where KDE Connect was simply
 * installed shows it as on — because it is.
 *
 * **Nothing here starts KDE Connect by accident.** Every call is sent with
 * auto-start off and only while the daemon is running, so reading the device
 * list of a switched-off KDE Connect never switches it on.
 *
 * **Nothing here blocks**: every bus call is asynchronous, as in MediaBridge.
 *
 * **The firewall is not touched, or even read.** Asking firewalld about its
 * configuration is polkit's `config.info`, which is auth_admin: a password
 * prompt every time the window opened. The package scriptlet opens the
 * kdeconnect service once, at install, beside Maze Connect's own port.
 */
class KdeConnectBridge : public QObject {
    Q_OBJECT

public:
    static constexpr int kCallTimeoutMs = 1500;
    static constexpr int kMaxDevices = 32;
    static constexpr int kMaxIdChars = 128;
    static constexpr int kMaxNameChars = 64;
    static constexpr int kMaxTextChars = 16 * 1024;

    /// The session bus, connected on first use.
    explicit KdeConnectBridge(QObject *parent = nullptr);

    /// A given bus — the tests run a private one.
    KdeConnectBridge(const QDBusConnection &bus, QObject *parent = nullptr);

    ~KdeConnectBridge() override;

    /// The daemon binary whose presence means "installed". /usr/bin/kdeconnectd
    /// by default.
    void setDaemonPath(const QString &path);

    /// Where the two overrides live, honouring $XDG_CONFIG_HOME/$XDG_DATA_HOME.
    static QString autostartOverridePath();
    static QString activationOverridePath();

    /// Connect to the bus and start watching the daemon. Idempotent. Returns
    /// false when there is no session bus.
    bool ensureStarted();

    bool isInstalled() const;

    /// An override is in place: one of ours, or an autostart entry someone
    /// else hid (Plasma's own Autostart page writes the same file).
    bool isBlocked() const;

    bool isEnabled() const { return isInstalled() && !isBlocked(); }
    bool isRunning() const { return m_running; }

    /**
     * Switch KDE Connect on or off for this user. Returns false, with @p error
     * set, when it cannot: not installed, or a file could not be written.
     * Starting and stopping the daemon follow asynchronously; changed() reports
     * the result.
     */
    bool setEnabled(bool enabled, QString &error);

    /// Re-read the devices. changed() follows.
    void refresh();

    QList<KdeConnectDevice> devices() const { return m_devices; }

    // ---- actions — true means the call was sent ---------------------------
    bool ping(const QString &deviceId, QString &error);
    bool ring(const QString &deviceId, QString &error);
    bool shareFile(const QString &deviceId, const QString &localPath, QString &error);
    bool shareText(const QString &deviceId, const QString &text, QString &error);
    bool sendClipboard(const QString &deviceId, QString &error);
    bool requestPairing(const QString &deviceId, QString &error);
    bool acceptPairing(const QString &deviceId, QString &error);
    bool rejectPairing(const QString &deviceId, QString &error);
    bool unpair(const QString &deviceId, QString &error);

    // ---- exposed for tests ------------------------------------------------

    /// What KDE Connect uses as an id and as an object path element:
    /// [A-Za-z0-9_], bounded. Anything else is never put into a path.
    static bool isValidDeviceId(const QString &id);

    /// The marker line both overrides carry, so only our own files are removed.
    static QString overrideMarker();

signals:
    /// Something changed: installed/enabled/running, a device.
    /// Coalesced.
    void changed();

private Q_SLOTS:
    void onServiceRegistered();
    void onServiceUnregistered();
    void onKdeSignal(const QDBusMessage &message);

private:
    const KdeConnectDevice *deviceById(const QString &id) const;
    bool checkPaired(const QString &deviceId, QString &error) const;
    bool sendCall(const QString &path, const QString &interface, const QString &method,
                  const QList<QVariant> &arguments, QString &error);
    void listDevices();
    void readDevice(const QString &id, quint64 generation);
    void readPlugins(const QString &id, quint64 generation);
    void readBattery(const QString &id, quint64 generation);
    void reloadBusConfig(bool startAfter);
    void quitDaemon();
    void scheduleChanged();
    void scheduleRefresh();

    QDBusConnection m_bus;
    bool m_useSessionBus = false;
    bool m_started = false;
    bool m_available = false;
    bool m_running = false;

    QString m_daemonPath;

    QDBusServiceWatcher *m_watcher = nullptr;
    QList<KdeConnectDevice> m_devices;
    /// Bumped on every full re-read; answers from an older read are dropped,
    /// so a device that went away cannot be put back by a late reply.
    quint64 m_generation = 0;

    QTimer m_changedDebounce;
    QTimer m_refreshDebounce;
};

} // namespace mazeconnect::core
