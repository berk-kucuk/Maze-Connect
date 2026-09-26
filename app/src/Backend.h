#pragma once

#include <QHash>
#include <QTimer>
#include <QUrl>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>
#include <QVariantMap>

#include "ActivityLog.h"
#include "DeviceListModel.h"
#include "TransferModel.h"
#include "mazeconnect/core/DeviceManager.h"

/**
 * The QML-facing surface of the application.
 *
 * Deliberately narrow: QML can list devices, start and answer a pairing,
 * and invoke capabilities. It cannot reach key material, cannot mark a
 * device trusted, and cannot bypass a prompt — every decision that grants
 * trust goes through DeviceManager and requires the user to have answered a
 * prompt first.
 */
class Backend : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString deviceName READ deviceName CONSTANT)
    Q_PROPERTY(QString fingerprint READ fingerprint CONSTANT)
    Q_PROPERTY(QString inboxPath READ inboxPath CONSTANT)
    Q_PROPERTY(QString listenAddress READ listenAddress NOTIFY listenAddressChanged)
    Q_PROPERTY(DeviceListModel *devices READ devices CONSTANT)
    Q_PROPERTY(TransferModel *transfers READ transfers CONSTANT)
    Q_PROPERTY(ActivityLog *activity READ activity CONSTANT)
    Q_PROPERTY(int connectedCount READ connectedCount NOTIFY devicesChanged)
    Q_PROPERTY(int pairedCount READ pairedCount NOTIFY devicesChanged)

    // The pairing prompt currently on screen, if any.
    Q_PROPERTY(bool pairingActive READ pairingActive NOTIFY pairingChanged)
    Q_PROPERTY(QString pairingDeviceId READ pairingDeviceId NOTIFY pairingChanged)
    Q_PROPERTY(QString pairingDeviceName READ pairingDeviceName NOTIFY pairingChanged)
    Q_PROPERTY(QString pairingCode READ pairingCode NOTIFY pairingChanged)
    Q_PROPERTY(bool pairingWeInitiated READ pairingWeInitiated NOTIFY pairingChanged)
    /// True once this side has answered and is waiting on the other. The
    /// buttons gave no sign they had been pressed, so "Codes match" read as a
    /// dead control while the other device took its time.
    Q_PROPERTY(bool pairingAnswered READ pairingAnswered NOTIFY pairingChanged)

    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

    /**
     * Every paired phone, for the dashboard: identity, whether it is linked,
     * its last validated reading (battery, storage, memory, network, ringer)
     * and whether it is ringing.
     *
     * The dashboard used to show *this* computer's CPU and disk — a reading
     * of the machine the user was already sitting at. What the desktop cannot
     * see without help is the phone, so that is what it shows now. This
     * machine's own snapshot still goes out to phones that ask; it simply is
     * not drawn here.
     *
     * Each entry is a map: deviceId, name, connected (bool), status (map, see
     * phonestatus::sanitize), error, updatedMs, ringing (bool), ringNotice,
     * canStatus / canRing / canOpen (bool, what the link allows right now).
     */
    Q_PROPERTY(QVariantList phones READ phones NOTIFY phonesChanged)

    /// Set by the dashboard while it is on screen, so the phones are read
    /// every few seconds then and once a minute otherwise.
    Q_PROPERTY(bool dashboardVisible READ dashboardVisible WRITE setDashboardVisible
                   NOTIFY dashboardVisibleChanged)

    // No Maze AI surface here on purpose. Maze AI has its own desktop
    // application; what this app adds is reaching that Ollama *from the
    // phone*, so DeviceManager keeps the bridge and this window does not
    // duplicate a program the user already has installed.

    // maze-guard killswitches. The one privileged capability, so the state
    // shown is always what the broker last reported — never what was asked.
    Q_PROPERTY(QVariantList guardDevices READ guardDevices NOTIFY guardChanged)
    Q_PROPERTY(QString guardError READ guardError NOTIFY guardChanged)
    Q_PROPERTY(bool guardAvailable READ guardAvailable NOTIFY guardChanged)
    Q_PROPERTY(QString guardBanner READ guardBanner NOTIFY guardBannerChanged)

    // The user's own command allow-list. Shown here so the file can be
    // checked and tried out on the machine that will actually run it,
    // rather than debugged through a phone.
    Q_PROPERTY(QVariantList commands READ commands NOTIFY commandsChanged)
    Q_PROPERTY(QString commandsError READ commandsError NOTIFY commandsChanged)
    Q_PROPERTY(QString commandsPath READ commandsPath CONSTANT)

    // --- Startup ---------------------------------------------------------
    Q_PROPERTY(bool autostartEnabled READ autostartEnabled WRITE setAutostartEnabled
                   NOTIFY autostartChanged)

public:
    explicit Backend(QObject *parent = nullptr);

    bool start();

    QString deviceName() const;
    QString fingerprint() const;
    QString inboxPath() const;

    /// "host:port" for this machine's listener, for pairing on networks
    /// where multicast discovery is filtered.
    QString listenAddress() const;
    DeviceListModel *devices() { return &m_devices; }
    TransferModel *transfers() { return &m_transfers; }
    ActivityLog *activity() { return &m_activity; }

    int connectedCount() const;
    int pairedCount() const;

    bool pairingActive() const { return m_pairingActive; }
    bool pairingAnswered() const { return m_pairingAnswered; }
    QString pairingDeviceId() const { return m_pairing.deviceId; }
    QString pairingDeviceName() const { return m_pairing.deviceName; }
    QString pairingCode() const { return m_pairing.verificationCode; }
    bool pairingWeInitiated() const { return m_pairing.weInitiated; }

    QString statusMessage() const { return m_statusMessage; }

    QVariantList phones() const;

    bool dashboardVisible() const { return m_dashboardVisible; }
    void setDashboardVisible(bool visible);

    /// One line for the tray tooltip: the linked phones and their battery.
    QString phoneSummary() const;

    QVariantList guardDevices() const { return m_guardDevices; }
    QString guardError() const { return m_guardError; }
    bool guardAvailable() const;
    QString guardBanner() const { return m_guardBanner; }

    QVariantList commands() const { return m_commands; }
    QString commandsError() const { return m_commandsError; }
    QString commandsPath() const;

    bool autostartEnabled() const;
    void setAutostartEnabled(bool enabled);

public slots:
    void requestPairing(const QString &deviceId);
    void acceptPairing();
    void rejectPairing();
    void unpair(const QString &deviceId);

    void sendFile(const QString &deviceId, const QString &fileUrl);

    void setCapability(const QString &deviceId, const QString &capability, bool enabled);
    void respondToFileOffer(const QString &deviceId, int transferId, bool accept);

    /// Whether a paired device has a capability enabled, for the UI toggles.
    bool hasCapability(const QString &deviceId, const QString &capability) const;

    /// Ask every linked phone for a fresh reading now.
    void refreshPhones();

    /// Make a phone ring — loudly, even on silent — so it can be found.
    void ringPhone(const QString &deviceId);

    /// Stop it again from here.
    void stopRinging(const QString &deviceId);

    /// Ring every linked phone that allows it, for the tray's "Find my
    /// phone". Returns how many were asked.
    int ringAllPhones();

    /// Send this computer's clipboard to one particular phone.
    bool sendClipboardTo(const QString &deviceId);

    /// Re-read every killswitch from the broker.
    void refreshGuard();

    /**
     * Re-run LAN discovery.
     *
     * Discovery is passive, so an empty device list is ambiguous — nothing
     * out there, or we stopped listening properly? This makes the second
     * case pressable. See DeviceManager::rescan().
     */
    void rescanDevices();

    /// Allow or block one device from this window. @p enabled is the
    /// *device's* state — see GuardBridge::setDeviceEnabled(), and note that
    /// getting this backwards once made Block unblock.
    void setGuardKill(const QString &device, bool enabled);

    /// Re-read the command file. Cheap, and the file is meant to be edited
    /// while the app is open.
    void refreshCommands();

    /// Run one entry here, to check it does what its label claims before
    /// trusting it from a phone.
    void runCommand(const QString &id);

    /**
     * Write a starter command file, if there is not one already.
     *
     * Never overwrites: the file is the user's, and silently replacing a
     * list they wrote would be the worst possible outcome of pressing a
     * button labelled "create".
     */
    bool createExampleCommands();

    /**
     * Add a new entry to the command file, from this window rather than a
     * text editor.
     *
     * @p argv is one space-separated line — the same shape the list already
     * shows an entry's argv in — split on whitespace into the program and
     * its arguments. There is still no shell anywhere: splitting on spaces
     * is the only interpretation applied, so there is no quoting, no `$`,
     * no `;` to have a meaning. An argument that itself needs a literal
     * space is not expressible from this field; the file can still be
     * edited by hand for that.
     *
     * Returns false and sets commandsError on failure — an id already
     * taken, a bound exceeded, or an existing file that does not parse.
     */
    /// @p pinned shows the entry on the phone's home-screen widget. Set
    /// only here; a phone can run a pinned entry but never change which
    /// ones are pinned.
    bool addCommand(const QString &id, const QString &label, const QString &argv, bool confirm,
                    bool pinned);

    /// Replace the entry with this id. Same argv shape as addCommand().
    bool updateCommand(const QString &id, const QString &newId, const QString &label,
                       const QString &argv, bool confirm, bool pinned);

    /// Remove the entry with this id.
    bool removeCommand(const QString &id);

    /**
     * Push the system clipboard's text to whichever paired phone is
     * reachable, for the tray's "Send clipboard to phone" action.
     *
     * No device to pick from here — see
     * DeviceManager::sendOpenOnPhone() for the auto-pick this backs onto.
     */
    bool sendClipboardToPhone();

    /// Short, readable form of a fingerprint for display.
    QString shortFingerprint(const QString &fingerprint) const;

    /// Friendly name for a paired device, falling back to its id.
    QString deviceName(const QString &deviceId) const;

signals:
    void listenAddressChanged();
    void devicesChanged();
    void pairingChanged();
    void statusMessageChanged();
    void phonesChanged();
    void dashboardVisibleChanged();

    /**
     * Something worth a desktop notification: a phone's battery is low or
     * full, a phone shared a link. @p url is set only for an http(s) link,
     * and is what a click on the notification opens — never opened unasked.
     */
    void notificationRequested(const QString &title, const QString &body, const QUrl &url);
    void guardChanged();
    void guardBannerChanged();
    void commandsChanged();
    void autostartChanged();
    void commandFinished(const QString &id, int exitCode, const QString &output);
    void securityAlert(const QString &summary, const QString &detail);
    void fileOffered(const QString &deviceId, int transferId, const QString &filename,
                     qint64 sizeBytes);

private:
    void setStatus(const QString &message);


    mazeconnect::core::DeviceManager m_manager;
    DeviceListModel m_devices;
    TransferModel m_transfers;
    ActivityLog m_activity;

    mazeconnect::core::PendingPairing m_pairing;
    bool m_pairingActive = false;
    bool m_pairingAnswered = false;
    QString m_statusMessage;

    struct PhoneState {
        QVariantMap status;
        QString error;
        qint64 updatedMs = 0;
        bool ringing = false;
        QString ringNotice;
        bool lowBatteryNotified = false;
        bool fullNotified = false;
        /// The previous reading's level, -1 before the first. "Fully
        /// charged" is announced on the climb to 100, never merely because
        /// the first reading after a restart found the phone already full.
        int lastLevel = -1;
    };
    QHash<QString, PhoneState> m_phoneStates;
    QTimer m_phonePoll;
    bool m_dashboardVisible = false;

    void onPhoneStatus(const QString &deviceId, const QVariantMap &status, const QString &error);
    void onTextShared(const QString &deviceId, const QString &text);
    void updatePhonePollInterval();

    QVariantList m_guardDevices;
    QString m_guardError;
    /// The last remote killswitch change, kept until the next one. A banner
    /// that faded would be missable, which defeats the point of having one.
    QString m_guardBanner;

    QVariantList m_commands;
    QString m_commandsError;

    /// Ids for runs started from this window, so their output is shown here
    /// and not confused with a run a phone asked for.
    QHash<quint32, QString> m_localRuns;
    quint32 m_nextLocalRun = 1;
};
