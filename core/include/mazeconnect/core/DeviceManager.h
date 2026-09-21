#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QTimer>

#include <memory>

#include "mazeconnect/core/Beacon.h"
#include "mazeconnect/core/Connection.h"
#include "mazeconnect/core/DeviceStore.h"
#include "mazeconnect/core/FileTransfer.h"
#include "mazeconnect/core/AiBridge.h"
#include "mazeconnect/core/CommandRunner.h"
#include "mazeconnect/core/GuardBridge.h"
#include "mazeconnect/core/Identity.h"
#include "mazeconnect/core/Server.h"
#include "mazeconnect/core/StatusProvider.h"

namespace mazeconnect::core {

/// A pairing awaiting the user's decision on both ends.
struct PendingPairing {
    QString deviceId;
    QString deviceName;
    QString deviceType;
    QByteArray peerPublicKey;
    QString verificationCode; ///< the 6-digit SAS to show the user
    bool weInitiated = false;
    bool localAccepted = false;
    bool remoteAccepted = false;
};

/// An offer of an incoming file, awaiting the user's decision.
struct PendingFileOffer {
    QString deviceId;
    quint32 transferId = 0;
    QString filename;
    qint64 sizeBytes = 0;
};

/**
 * Owns everything: identity, pinned devices, discovery, the listener, live
 * connections, the pairing state machine, and file transfers.
 *
 * Message-level authorisation lives here, and it is the second half of the
 * transport's trust model. Connection admits an unpaired peer so pairing is
 * possible at all; this class is what ensures such a peer can do *nothing*
 * except pair, and that every other capability additionally requires the
 * user to have enabled it.
 */
class DeviceManager : public QObject {
    Q_OBJECT

public:
    /**
     * @param dataDir  where identity, pins, and the file inbox live.
     *                 Created with owner-only permissions if absent.
     */
    explicit DeviceManager(QString dataDir, QObject *parent = nullptr);
    ~DeviceManager() override;

    /**
     * Where identity, pins and the inbox live.
     *
     * Honours $STATE_DIRECTORY when systemd provides it (the daemon runs
     * with StateDirectory=, so systemd creates the directory with the right
     * ownership before we start), and otherwise falls back to
     * $XDG_DATA_HOME/mazeconnect.
     *
     * Deliberately NOT QStandardPaths::AppDataLocation: that resolves to
     * "~/.local/share/Maze Linux/Maze Connect", whose spaces make it awkward
     * to name in systemd directives — and a mismatch there means the daemon
     * silently cannot write its identity.
     */
    static QString defaultDataDir();

    /// Load or generate the identity, load pins, and start networking.
    bool start(const QString &deviceName, const QString &deviceType);
    void stop();

    QString deviceId() const { return m_deviceId; }
    QString deviceName() const { return m_deviceName; }
    QString fingerprint() const { return m_identity.fingerprint(); }
    QString inboxPath() const;

    /// TCP port the listener bound to (0 before start()).
    quint16 listenPort() const;

    const DeviceStore &store() const { return m_store; }
    Beacon *beacon() { return m_beacon; }

    /// The dashboard snapshot source, shared with the local UI so a desktop
    /// refresh and a phone's request are answered from one cache.
    StatusProvider *statusProvider() { return &m_status; }

    /// The user's command allow-list, shared with the local UI so both ends
    /// read the same file.
    CommandRunner *commandRunner() { return &m_commands; }

    /// Maze AI over the local Ollama, shared with the local window so both
    /// ends talk to one bridge.
    AiBridge *aiBridge() { return &m_ai; }

    /// The killswitch broker. Shared with the local window so the desktop
    /// sees the same states a phone does.
    GuardBridge *guardBridge() { return &m_guard; }
    const GuardBridge *guardBridge() const { return &m_guard; }

    QList<DiscoveredDevice> discoveredDevices() const;

    /**
     * Re-run discovery, because the user asked.
     *
     * Rebinds the beacon (see Beacon::refresh() for why that is not merely
     * a re-announce), sends an announcement immediately rather than at the
     * next tick, and redials anything paired. Distinct from the automatic
     * sweep only in that a person pressed it: discovery is passive, so an
     * empty device list cannot be told apart from a broken listener, and
     * this is what makes that case actionable instead of a restart.
     */
    void rescan();
    QList<PairedDevice> pairedDevices() const { return m_store.devices(); }
    bool isConnected(const QString &deviceId) const;

    // ---- Pairing --------------------------------------------------------

    /// Begin pairing with a device seen on the LAN.
    /**
     * Why a pairing could not be started, so the UI can say the true thing.
     *
     * These were all reported as "could not reach that device", which sent
     * people looking at their network for a problem that was not there — none
     * of the refusals below involve reaching anything.
     */
    enum class PairingStart {
        Started,        ///< dialling; success or failure arrives by signal
        NotDiscovered,  ///< no beacon from that id, so no address to dial
        AlreadyPaired,
        AlreadyPending, ///< one pairing at a time per device
    };
    Q_ENUM(PairingStart)

    PairingStart requestPairing(const QString &deviceId);

    /**
     * Begin pairing with an explicit address, for networks where multicast
     * discovery is filtered and the user types the address in by hand.
     * Carries no less weight than discovery-based pairing: the address only
     * decides who we dial, and the SAS comparison is still what authorises.
     */
    bool requestPairingAt(const QHostAddress &address, quint16 port, const QString &deviceId);
    /// Answer a pairing prompt (ours or theirs).
    void respondToPairing(const QString &deviceId, bool accept);
    /// Forget a paired device and drop its connection.
    bool unpair(const QString &deviceId);

    /**
     * Dial a device we have already paired with, at an explicit address.
     *
     * Used by the reconnect sweep, and by the UI on a network where
     * multicast is filtered. Always a Paired-mode connection: an unknown key
     * fails here rather than opening a pairing window.
     */
    bool connectToPairedAt(const QHostAddress &address, quint16 port, const QString &deviceId);

    // ---- Capabilities ---------------------------------------------------

    bool sendFile(const QString &deviceId, const QString &localPath);

    /**
     * Ask a paired device for a dashboard snapshot.
     *
     * Only a report from a device we actually asked is accepted, so an
     * unsolicited statusReport cannot push anything into the UI.
     */
    bool requestStatus(const QString &deviceId);

    /// Accept or refuse an offered incoming file.
    void respondToFileOffer(const QString &deviceId, quint32 transferId, bool accept);

    /// Enable/disable a capability for one paired device.
    bool setCapabilityEnabled(const QString &deviceId, Capability capability, bool enabled);

    /**
     * Push clipboard text to whichever paired phone is reachable right now.
     *
     * No specific target: the tray action this backs has no per-device UI
     * to pick one from, so — same auto-pick idiom the mobile client already
     * uses for its own outbound sends — this takes the first connected link
     * that actually allows OpenOnPhone. False if none does, or the text is
     * over the bound; refused outright rather than truncated, since a
     * cut-off URL is broken rather than merely shorter.
     */
    bool sendOpenOnPhone(const QString &text);

signals:
    void deviceListChanged();
    void deviceConnected(const QString &deviceId);
    void deviceDisconnected(const QString &deviceId);

    /// A pairing needs the user's decision; show the code on screen.
    void pairingRequested(const mazeconnect::core::PendingPairing &pairing);
    void pairingCompleted(const QString &deviceId, bool accepted);
    void pairingFailed(const QString &deviceId, const QString &reason);

    void fileOffered(const mazeconnect::core::PendingFileOffer &offer);
    void fileProgress(const QString &deviceId, quint32 transferId, qint64 received, qint64 total);
    void fileReceived(const QString &deviceId, const QString &path);
    void fileFailed(const QString &deviceId, quint32 transferId, const QString &reason);

    /**
     * A snapshot arrived from @p deviceId in answer to requestStatus().
     *
     * @p snapshot is peer-supplied and unvalidated beyond being an object;
     * @p error is non-empty when that device has no snapshot to give.
     */
    void statusReportReceived(const QString &deviceId, const QJsonObject &snapshot,
                              const QString &error);

    /**
     * A paired device changed a killswitch.
     *
     * Emitted for the UI to log and announce. A privileged change made from
     * somewhere else in the house must never be silent on the machine it
     * actually happened to.
     */
    void guardChangedRemotely(const QString &deviceId, const QString &device,
                              bool requestedEnabled, const QString &state,
                              const QString &error);

    void securityAlert(const QString &summary, const QString &detail);

private slots:
    void onIncomingConnection(mazeconnect::core::Connection *connection);
    void onConnectionRejected(const QHostAddress &address, const QString &reason);
    void onDeviceDiscovered(const mazeconnect::core::DiscoveredDevice &device);

private:
    struct Link {
        Connection *connection = nullptr;
        QString deviceId;
        QString deviceName;
        QString deviceType;
        bool trusted = false;
        bool helloReceived = false;
        Capabilities peerCapabilities = Capability::None;
        QByteArray ourNonce;
        QByteArray theirNonce;
        /// What the initiator committed to, until PairReveal opens it.
        QByteArray theirCommitment;
    };

    bool loadOrCreateIdentity(const QString &deviceName);
    void wireConnection(Connection *connection);
    void handleMessage(Connection *connection, const Message &message);
    void handleData(Connection *connection, quint32 transferId, const QByteArray &chunk);
    void dropConnection(Connection *connection, const QString &reason);

    // Links are held by shared_ptr, not stored by value: handleMessage()
    // takes a reference to one and can then reach code that appends to or
    // removes from m_links (wireConnection/dropConnection). With Link* into
    // a QList that is a use-after-free — reallocation or removal invalidates
    // the pointer while it is still being written through.
    std::shared_ptr<Link> linkFor(Connection *connection);
    std::shared_ptr<Link> linkForDevice(const QString &deviceId);
    Connection *connectionForDevice(const QString &deviceId);

    bool capabilityAllowed(const Link &link, Capability capability) const;
    bool allowPairingAttempt(const QString &peerKeyHex);

    /**
     * Answer everyone waiting on a snapshot.
     *
     * One snapshot serves every pending requester, so a device sending
     * statusRequest in a loop cannot multiply the work — it only re-joins a
     * queue that is drained once. Pass an empty @p error for success.
     */
    void deliverStatusReports(const QJsonObject &snapshot, const QString &error);

    /// Resolve an in-flight AI reply to the device that asked, re-checking
    /// the capability. False means the chunk goes nowhere.
    bool routeAi(quint32 requestId, Connection *&connection, quint32 &peerRequestId);

    void deliverAiModels(const QStringList &models, const QString &error);

    /// Drop everything we were waiting on from a device whose link just
    /// closed. See the implementation for why the guard slot in particular
    /// must not be allowed to leak.
    void forgetPendingWork(const QString &deviceId);

    void deliverGuardReport(const QMap<GuardDevice, GuardState> &states, const QString &error);

    void sendHello(Connection *connection);
    void finalizePairing(const std::shared_ptr<Link> &link);

    /**
     * Dial a paired device we can see but are not linked to.
     *
     * Both devices see each other's beacons, so both would dial and we would
     * end up with two links for one pair. The lexicographically smaller
     * deviceId is the one that initiates — a deterministic rule that needs no
     * negotiation and cannot deadlock, since the ids are distinct by
     * construction.
     */
    void maybeConnectTo(const DiscoveredDevice &device);
    void reconnectPairedDevices();

    /// Re-join the discovery group if the interfaces moved under it.
    /// Runs on the reconnect sweep; see the definition for why this is
    /// polled rather than subscribed to.
    void rejoinIfInterfacesChanged();

    /// Interface+address fingerprint from the last sweep. Null until the
    /// first one, which must not trigger a refresh.
    QString m_interfaceSignature;

    QString m_dataDir;
    QString m_deviceId;
    QString m_deviceName;
    QString m_deviceType;

    Identity m_identity;
    DeviceStore m_store;
    Beacon *m_beacon = nullptr;
    Server *m_server = nullptr;
    FileTransferReceiver *m_receiver = nullptr;
    StatusProvider m_status;
    CommandRunner m_commands;
    AiBridge m_ai;
    GuardBridge m_guard;

    /// Devices whose statusRequest has not been answered yet.
    QSet<QString> m_statusRequesters;

    /**
     * Fingerprint of the last snapshot sent to each device.
     *
     * A dashboard polls every few seconds, and most of those answers are
     * byte-identical to the one before — an idle machine's reading does not
     * change. Sending the same ~2 KB over and over costs radio time on the
     * phone for no new information, so an unchanged snapshot is answered with
     * a few bytes instead.
     *
     * Per device, not global: two phones may be at different revisions, and a
     * device that has just reconnected must get the full snapshot even if
     * another already has it.
     */
    QHash<QString, QByteArray> m_lastStatusSent;

    /**
     * In-flight command runs.
     *
     * Keyed by an id *we* allocate, not by the one the peer sent. Two devices
     * are free to pick the same requestId, and using theirs as the key would
     * let one device's result be delivered against the other's run. Their id
     * is carried alongside purely so the answer can be matched up on their
     * side.
     */
    struct CommandRequest {
        QString deviceId;
        quint32 peerRequestId = 0;
    };
    QHash<quint32, CommandRequest> m_commandRequests;

    /// In-flight AI replies, keyed by our own handle for the same reason as
    /// commands: two devices may pick the same requestId.
    struct AiRequest {
        QString deviceId;
        quint32 peerRequestId = 0;
    };
    QHash<quint32, AiRequest> m_aiRequests;

    /// Devices waiting on a model list.
    QSet<QString> m_aiModelRequesters;

    /// Devices waiting on a killswitch report.
    QSet<QString> m_guardStatusRequesters;

    /// The device whose guardRequest is in flight, so the result goes back to
    /// the one that asked. One at a time: a privileged change is not
    /// something to have several of in the air.
    QString m_guardRequester;

    /// Devices we have asked for a snapshot. A statusReport from anyone else
    /// is unsolicited and dropped — being paired is not by itself a licence
    /// to put content in front of the user.
    QSet<QString> m_statusAwaiting;

    QList<std::shared_ptr<Link>> m_links;
    QHash<QString, PendingPairing> m_pendingPairings;
    QHash<quint32, PendingFileOffer> m_pendingOffers;

    /// Timestamps of recent pairing attempts per peer key, for rate limiting.
    QHash<QString, QQueue<QDateTime>> m_pairingAttempts;

    /// Devices we are currently dialing, so a burst of beacons cannot open
    /// several sockets to the same peer.
    QSet<QString> m_outboundPending;
    QTimer m_reconnectTimer;

    /// Outgoing transfers still streaming, keyed by transfer id.
    QHash<quint32, QString> m_outgoingTransfers;
    quint32 m_nextTransferId = 1;
};

} // namespace mazeconnect::core

Q_DECLARE_METATYPE(mazeconnect::core::PendingPairing)
Q_DECLARE_METATYPE(mazeconnect::core::PendingFileOffer)
