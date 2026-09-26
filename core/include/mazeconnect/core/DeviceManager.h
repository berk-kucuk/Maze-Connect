#pragma once

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include <functional>
#include <memory>

#include "mazeconnect/core/Beacon.h"
#include "mazeconnect/core/Connection.h"
#include "mazeconnect/core/DeviceStore.h"
#include "mazeconnect/core/FileTransfer.h"
#include "mazeconnect/core/AiBridge.h"
#include "mazeconnect/core/CommandRunner.h"
#include "mazeconnect/core/GuardBridge.h"
#include "mazeconnect/core/Identity.h"
#include "mazeconnect/core/MediaBridge.h"
#include "mazeconnect/core/RemoteInput.h"
#include "mazeconnect/core/SharedFolder.h"
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

    /// This machine's media players, as a phone sees them.
    MediaBridge *mediaBridge() { return &m_media; }

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

    /// The same, to one named device rather than the first that will take it.
    bool sendOpenOnPhone(const QString &deviceId, const QString &text);

    /**
     * Ask a paired phone for its battery/storage/network reading.
     *
     * The answer arrives as phoneStatusReceived(). As with a dashboard
     * snapshot, only a reading this side asked for is accepted: being paired
     * does not by itself entitle a phone to put content on this screen.
     */
    bool requestPhoneStatus(const QString &deviceId);

    /**
     * Start (@p ring true) or stop ringing a paired phone.
     *
     * The phone answers with findPhoneAnswered(), and says so again when the
     * person holding it silences it — which is how the ringing state here
     * stays honest without polling.
     */
    bool ringPhone(const QString &deviceId, bool ring);

    /// Whether a connected device may use @p capability right now.
    bool allows(const QString &deviceId, Capability capability) const;

    /// Send this computer's clipboard to every linked device that syncs.
    int sendClipboard(const QString &text);

    /// The one folder phones may browse. Shared with the UI.
    const SharedFolder &sharedFolder() const { return m_shared; }

    /// Remote input — who is driving, and a way to end it from here.
    RemoteInput *remoteInput() { return m_input.get(); }
    void stopRemoteInput();

    /// How the computer's owner answered a phone asking for full control.
    enum class InputApproval { Deny, Once, Always };
    Q_ENUM(InputApproval)

    /**
     * Answer the pending request from @p deviceId. Once grants this one
     * session and nothing after it; Always switches the capability on for
     * that device. Either starts the session straight away, so the phone does
     * not have to ask again.
     */
    void answerRemoteInput(const QString &deviceId, InputApproval answer);

    /**
     * For tests: replace the input backend (takes ownership). The portal one
     * needs a desktop session, which a test run does not have.
     */
    void setInputBackendForTesting(InputBackend *backend);

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
    /// Bytes of an outgoing file handed to the socket so far.
    void fileSendProgress(const QString &deviceId, quint32 transferId, qint64 sent, qint64 total);
    void fileSent(const QString &deviceId, quint32 transferId);
    void fileReceived(const QString &deviceId, quint32 transferId, const QString &path);
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

    /**
     * A paired device said hello over a trusted link, so its capabilities are
     * now known. deviceConnected() fires before that — asking a device for
     * anything capability-gated has to wait for this one.
     */
    void deviceReady(const QString &deviceId);

    /**
     * A phone answered requestPhoneStatus(). @p status is already validated
     * (see phonestatus::sanitize); @p error is non-empty when the phone had
     * no reading to give, in which case @p status is empty.
     */
    void phoneStatusReceived(const QString &deviceId, const QVariantMap &status,
                             const QString &error);

    /// Whether @p deviceId is ringing now, and why not if it refused.
    void findPhoneAnswered(const QString &deviceId, bool ringing, const QString &error);

    /**
     * A phone sent text for this computer's clipboard. Bounded and free of
     * disallowed control characters; what to do with it is the UI's call.
     */
    void textShared(const QString &deviceId, const QString &text);

    /// A device is (or is no longer) controlling this computer's input.
    /// @p mode is "presenter" or "full".
    void remoteInputChanged(const QString &deviceId, bool active, const QString &mode,
                            const QString &error);

    /// A phone asked for full control and is not allowed it: ask the owner.
    void remoteInputApprovalRequested(const QString &deviceId, const QString &deviceName);
    /// That question is no longer open — answered, timed out, or gone.
    void remoteInputApprovalCleared();

    /// A device's clipboard changed; bounded and cleaned like shareText.
    void clipboardReceived(const QString &deviceId, const QString &text);

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

    /// Send the players to everyone who asked and everyone subscribed.
    void deliverMediaState();
    void sendMediaState(Connection *connection, const QString &notice);
    /// At most a handful of media commands per device per second: a held
    /// volume slider must not become a process spawn per touch event.
    bool allowMediaCommand(const QString &deviceId);

    /// Move an outgoing file forward as far as the socket's buffer allows.
    void pumpOutgoing(quint32 transferId);

    /// Retire an older link to the same device when a new one is
    /// established — see wireConnection().
    void retireOlderLinks(const std::shared_ptr<Link> &current);

    /// Fail every transfer, in either direction, with @p deviceId.
    void abandonTransfers(const QString &deviceId, const QString &reason);

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
    MediaBridge m_media;

    /// Devices that asked for the players and have not had an answer yet.
    QSet<QString> m_mediaWaiting;
    /// Devices that want every change pushed, until they say otherwise or
    /// their link drops.
    QSet<QString> m_mediaSubscribers;
    /// Hash of the last state pushed to each subscriber, so an unchanged
    /// state is not sent again.
    QHash<QString, QByteArray> m_lastMediaSent;
    struct RateWindow {
        qint64 startedMs = 0;
        int count = 0;
    };
    QHash<QString, RateWindow> m_mediaCommandRate;

    /// Shared texts per device, for the clipboard flood limit.
    QHash<QString, RateWindow> m_shareTextRate;
    QHash<QString, RateWindow> m_clipboardRate;
    QHash<QString, RateWindow> m_folderRate;
    QHash<QString, RateWindow> m_previewRate;
    /// Previews being decoded per device, so one phone cannot fill the pool.
    QHash<QString, int> m_previewsInFlight;
    bool allowWindow(QHash<QString, RateWindow> &table, const QString &deviceId, int max,
                     int windowMs);

    std::unique_ptr<RemoteInput> m_input;

    /// The one device waiting for the owner's answer, and for how long.
    QString m_inputApprovalPending;
    QTimer m_inputApprovalTimer;
    /// A device allowed for the current session only ("Allow once").
    QString m_inputOnceGrant;
    bool fullInputAllowed(const Link &link) const;
    void clearInputApproval();
    SharedFolder m_shared;

    /// Queue a file offer; @p beforeOffer runs with the id before the offer
    /// is sent. 0 when refused.
    quint32 offerFile(const QString &deviceId, const QString &localPath,
                      const std::function<void(quint32)> &beforeOffer = {});
    void wireRemoteInput();
    bool allowShareText(const QString &deviceId);

    /// Phones we asked for a reading and have not heard back from.
    QSet<QString> m_phoneStatusAwaiting;

    /// Phones we asked to ring (or to stop) whose answers we will take. Kept
    /// while a phone rings, so its "silenced by hand" notice is accepted.
    QSet<QString> m_findPhoneAwaiting;

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

    /**
     * Outgoing files, keyed by transfer id, **with the device they were
     * offered to.** The id alone used to be the key, and ids are small
     * sequential numbers: any other paired device could send fileAccept for
     * a transfer meant for someone else and have the file streamed to itself.
     */
    struct OutgoingTransfer {
        QString deviceId;
        QString path;
        std::shared_ptr<QFile> file; ///< open once accepted
        qint64 sent = 0;
        qint64 size = 0;
    };
    QHash<quint32, OutgoingTransfer> m_outgoingTransfers;
    quint32 m_nextTransferId = 1;
};

} // namespace mazeconnect::core

Q_DECLARE_METATYPE(mazeconnect::core::PendingPairing)
Q_DECLARE_METATYPE(mazeconnect::core::PendingFileOffer)
