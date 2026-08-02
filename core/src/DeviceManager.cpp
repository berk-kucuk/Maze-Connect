#include "mazeconnect/core/DeviceManager.h"

#include "mazeconnect/core/Limits.h"
#include "mazeconnect/core/PathSanitizer.h"
#include "mazeconnect/core/Sas.h"
#include "mazeconnect/core/Version.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QUuid>

#include <utility>

namespace mazeconnect::core {

// Connection and pairing decisions are logged: on a security-sensitive
// service the operator needs to be able to answer "what did it refuse, and
// why" after the fact. Message *contents* are never logged.
Q_LOGGING_CATEGORY(lcLink, "maze.connect.link")

namespace {

constexpr QLatin1StringView kIdentityKeyFile("identity.key");
constexpr QLatin1StringView kIdentityCertFile("identity.crt");
constexpr QLatin1StringView kDeviceIdFile("device-id");
constexpr QLatin1StringView kDevicesFile("devices.json");
constexpr QLatin1StringView kInboxDir("inbox");

/// Chunk size for outgoing files — comfortably under the data-frame cap
/// once the 4-byte transfer id is added.
constexpr qint64 kSendChunkSize = 128 * 1024;

constexpr int kReconnectIntervalMs = 10000;

QString hexKey(const QByteArray &publicKey) {
    return QString::fromLatin1(Identity::fingerprintOf(publicKey).toHex());
}

} // namespace

DeviceManager::DeviceManager(QString dataDir, QObject *parent)
    : QObject(parent), m_dataDir(std::move(dataDir)),
      m_store(QDir(m_dataDir).filePath(kDevicesFile)) {
    qRegisterMetaType<PendingPairing>();
    qRegisterMetaType<PendingFileOffer>();
    qRegisterMetaType<DiscoveredDevice>();
    qRegisterMetaType<Message>();

    // One snapshot answers every device waiting for one, and the local UI
    // draws from the same cache — so a dashboard open on screen while a phone
    // polls still costs one helper run.
    connect(&m_status, &StatusProvider::snapshotReady, this,
            [this](const QJsonObject &snapshot) { deliverStatusReports(snapshot, QString()); });
    connect(&m_status, &StatusProvider::snapshotFailed, this,
            [this](const QString &reason) { deliverStatusReports(QJsonObject(), reason); });

    // A chunk goes only to the device that asked, and only while that device
    // is still allowed to ask. Streaming is ordinary control frames, so the
    // counter, replay window and size caps apply to each one unchanged.
    connect(&m_ai, &AiBridge::chunk, this, [this](quint32 requestId, const QString &text) {
        Connection *connection = nullptr;
        quint32 peerRequestId = 0;
        if (!routeAi(requestId, connection, peerRequestId)) {
            return;
        }
        connection->send(Message::aiChunk(connection->nextCounter(), peerRequestId, text));
    });
    connect(&m_ai, &AiBridge::done, this, [this](quint32 requestId, const QString &) {
        Connection *connection = nullptr;
        quint32 peerRequestId = 0;
        if (routeAi(requestId, connection, peerRequestId)) {
            connection->send(Message::aiDone(connection->nextCounter(), peerRequestId, QString()));
        }
        m_aiRequests.remove(requestId);
    });
    connect(&m_ai, &AiBridge::failed, this, [this](quint32 requestId, const QString &reason) {
        Connection *connection = nullptr;
        quint32 peerRequestId = 0;
        if (routeAi(requestId, connection, peerRequestId)) {
            connection->send(Message::aiDone(connection->nextCounter(), peerRequestId, reason));
        }
        m_aiRequests.remove(requestId);
    });

    connect(&m_ai, &AiBridge::modelsReady, this, [this](const QStringList &models) {
        deliverAiModels(models, QString());
    });
    connect(&m_ai, &AiBridge::modelsFailed, this, [this](const QString &reason) {
        deliverAiModels({}, reason);
    });

    connect(&m_guard, &GuardBridge::statusReady, this,
            [this](const QMap<GuardDevice, GuardState> &states) {
                deliverGuardReport(states, QString());
            });
    connect(&m_guard, &GuardBridge::statusFailed, this,
            [this](const QString &reason) { deliverGuardReport({}, reason); });

    connect(&m_guard, &GuardBridge::killApplied, this,
            [this](GuardDevice device, bool requestedEnabled, GuardState state,
                   const QString &error) {
                const QString requester = std::exchange(m_guardRequester, {});
                const QString name = guardDeviceName(device);

                // Announced before the answer is sent, and announced whether
                // or not the answer can still be delivered: the change already
                // happened to this machine, and the person sitting at it is
                // owed that regardless of what the phone hears back.
                if (!requester.isEmpty()) {
                    emit guardChangedRemotely(requester, name, requestedEnabled,
                                              guardStateName(state), error);
                }

                if (requester.isEmpty()) {
                    return;
                }
                Connection *connection = connectionForDevice(requester);
                if (!connection) {
                    return;
                }
                const auto link = linkFor(connection);
                if (!link || !capabilityAllowed(*link, Capability::GuardControl)) {
                    return;
                }
                connection->send(Message::guardResult(connection->nextCounter(), name,
                                                      guardStateName(state), error));
            });

    connect(&m_commands, &CommandRunner::finished, this,
            [this](quint32 requestId, const QString &id, int exitCode, const QString &output,
                   bool timedOut) {
                Q_UNUSED(timedOut);
                const auto request = m_commandRequests.take(requestId);
                if (request.deviceId.isEmpty()) {
                    return;
                }
                Connection *connection = connectionForDevice(request.deviceId);
                if (!connection) {
                    return;
                }
                // Re-checked at delivery: the user may have switched the
                // capability off while the command was running, and output
                // authorised when it started is not authorised now.
                const auto link = linkFor(connection);
                if (!link || !capabilityAllowed(*link, Capability::Commands)) {
                    return;
                }
                Message result = Message::commandResult(connection->nextCounter(),
                                                        request.peerRequestId, id, exitCode,
                                                        output);
                connection->send(result);
            });
}

DeviceManager::~DeviceManager() {
    stop();
}

QString DeviceManager::defaultDataDir() {
    // systemd sets STATE_DIRECTORY when the unit declares StateDirectory=,
    // and has already created it with the correct ownership.
    const QByteArray stateDir = qgetenv("STATE_DIRECTORY");
    if (!stateDir.isEmpty()) {
        return QString::fromLocal8Bit(stateDir);
    }

    QString base = QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME"));
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.local/share");
    }
    return base + QStringLiteral("/mazeconnect");
}

QString DeviceManager::inboxPath() const {
    // Under the user's Downloads, where a received file is where they would
    // look for it — not buried in the app's data directory.
    //
    // A subdirectory rather than Downloads itself: the receiver keeps its
    // inbox owner-only, and applying that to the whole of Downloads would
    // change a permission the user never asked us to touch. $XDG_DOWNLOAD_DIR
    // is honoured, so a localised or relocated Downloads still works.
    const QString downloads =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (!downloads.isEmpty()) {
        return QDir(downloads).filePath(QStringLiteral("Maze Connect"));
    }
    return QDir(m_dataDir).filePath(kInboxDir);
}

quint16 DeviceManager::listenPort() const {
    return m_server ? m_server->port() : 0;
}

bool DeviceManager::loadOrCreateIdentity(const QString &deviceName) {
    QDir dir;
    if (!dir.mkpath(m_dataDir)) {
        return false;
    }
    QFile::setPermissions(m_dataDir,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner
                              | QFileDevice::ExeOwner);

    const QString keyPath = QDir(m_dataDir).filePath(kIdentityKeyFile);
    const QString certPath = QDir(m_dataDir).filePath(kIdentityCertFile);
    const QString idPath = QDir(m_dataDir).filePath(kDeviceIdFile);

    QFile keyFile(keyPath);
    QFile certFile(certPath);
    if (keyFile.exists() && certFile.exists()) {
        if (keyFile.open(QIODevice::ReadOnly) && certFile.open(QIODevice::ReadOnly)) {
            m_identity = Identity::fromPem(keyFile.readAll(), certFile.readAll());
            keyFile.close();
            certFile.close();
        }
    }

    if (!m_identity.isValid()) {
        // Refuse to silently replace unreadable-but-present key material: a
        // corrupt or tampered identity file must be an explicit decision to
        // recover from, not something we quietly regenerate — regenerating
        // would invalidate every existing pairing without the user noticing.
        if (keyFile.exists() || certFile.exists()) {
            return false;
        }

        m_identity = Identity::generate(deviceName);
        if (!m_identity.isValid()) {
            return false;
        }
        if (!keyFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        keyFile.write(m_identity.privateKeyPem());
        keyFile.close();
        // The private key never needs to be readable by anyone else.
        if (!keyFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            return false;
        }

        if (!certFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        certFile.write(m_identity.certificatePem());
        certFile.close();
        certFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    QFile idFile(idPath);
    if (idFile.exists() && idFile.open(QIODevice::ReadOnly)) {
        m_deviceId = QString::fromLatin1(idFile.readAll()).trimmed();
        idFile.close();
    }
    if (m_deviceId.isEmpty()) {
        m_deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (idFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            idFile.write(m_deviceId.toLatin1());
            idFile.close();
            idFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
    }
    return true;
}

bool DeviceManager::start(const QString &deviceName, const QString &deviceType) {
    m_deviceName = deviceName.left(limits::kMaxDeviceNameChars);
    m_deviceType = deviceType;

    if (!loadOrCreateIdentity(m_deviceName)) {
        return false;
    }
    if (!m_store.load()) {
        return false;
    }

    m_receiver = new FileTransferReceiver(inboxPath(), this);
    connect(m_receiver, &FileTransferReceiver::progress, this,
            [this](quint32 id, qint64 received, qint64 total) {
                emit fileProgress(m_pendingOffers.value(id).deviceId, id, received, total);
            });

    m_server = new Server(m_identity, &m_store, this);
    connect(m_server, &Server::connectionEstablished, this, &DeviceManager::onIncomingConnection);
    connect(m_server, &Server::connectionRejected, this, &DeviceManager::onConnectionRejected);

    // Unpaired peers are admitted at the transport layer so that pairing is
    // possible without the user arranging a synchronised "pair mode" on both
    // devices. handleMessage() is what confines them: an untrusted link may
    // send nothing but pairing messages.
    m_server->setPairingWindowOpen(true);
    if (!m_server->start(Server::kDefaultPort)) {
        return false;
    }

    // Periodic sweep as well as beacon-triggered dialing: a device that was
    // already announcing before we started, or whose link dropped while it
    // stayed visible, would otherwise never be retried.
    m_reconnectTimer.setInterval(kReconnectIntervalMs);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &DeviceManager::reconnectPairedDevices);
    m_reconnectTimer.start();

    m_beacon = new Beacon(this);
    connect(m_beacon, &Beacon::deviceDiscovered, this, &DeviceManager::onDeviceDiscovered);
    connect(m_beacon, &Beacon::deviceLost, this,
            [this](const QString &) { emit deviceListChanged(); });
    m_beacon->start(m_deviceId, m_deviceName, m_deviceType, m_server->port());

    emit deviceListChanged();
    return true;
}

void DeviceManager::stop() {
    m_reconnectTimer.stop();
    m_outboundPending.clear();
    for (const auto &link : m_links) {
        if (link->connection) {
            link->connection->disconnectFromPeer();
        }
    }
    m_links.clear();

    if (m_beacon) {
        m_beacon->stop();
    }
    if (m_server) {
        m_server->close();
    }
    if (m_receiver) {
        m_receiver->cancelAll();
    }
}

QList<DiscoveredDevice> DeviceManager::discoveredDevices() const {
    return m_beacon ? m_beacon->devices() : QList<DiscoveredDevice>{};
}

bool DeviceManager::isConnected(const QString &deviceId) const {
    for (const auto &link : m_links) {
        if (link->deviceId == deviceId && link->trusted
            && link->connection->state() == Connection::State::Established) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<DeviceManager::Link> DeviceManager::linkFor(Connection *connection) {
    for (const auto &link : m_links) {
        if (link->connection == connection) {
            return link;
        }
    }
    return nullptr;
}

std::shared_ptr<DeviceManager::Link> DeviceManager::linkForDevice(const QString &deviceId) {
    for (const auto &link : m_links) {
        if (link->deviceId == deviceId) {
            return link;
        }
    }
    return nullptr;
}

Connection *DeviceManager::connectionForDevice(const QString &deviceId) {
    const auto link = linkForDevice(deviceId);
    return link ? link->connection : nullptr;
}

void DeviceManager::onConnectionRejected(const QHostAddress &address, const QString &reason) {
    // Surfaced rather than swallowed: a rejected connection on a home LAN is
    // worth the user seeing, since it is what an impersonation attempt looks
    // like from this side.
    emit securityAlert(QStringLiteral("Refused a connection from %1").arg(address.toString()),
                       reason);
}

void DeviceManager::onIncomingConnection(Connection *connection) {
    wireConnection(connection);
}

void DeviceManager::wireConnection(Connection *connection) {
    qCInfo(lcLink) << "link established with" << connection->peerAddress().toString()
                   << "trusted=" << m_store.isTrusted(connection->peerPublicKey())
                   << "fingerprint=" << connection->peerFingerprint().left(16);
    auto link = std::make_shared<Link>();
    link->connection = connection;
    link->trusted = m_store.isTrusted(connection->peerPublicKey());
    if (link->trusted) {
        const PairedDevice device = m_store.deviceForKey(connection->peerPublicKey());
        link->deviceId = device.deviceId;
        link->deviceName = device.deviceName;
        link->deviceType = device.deviceType;
    }
    m_links.append(link);

    connect(connection, &Connection::messageReceived, this,
            [this, connection](const Message &message) { handleMessage(connection, message); });
    connect(connection, &Connection::dataReceived, this,
            [this, connection](quint32 id, const QByteArray &chunk) {
                handleData(connection, id, chunk);
            });
    connect(connection, &Connection::disconnected, this,
            [this, connection]() { dropConnection(connection, QString()); });
    connect(connection, &Connection::failed, this,
            [this, connection](const QString &reason) { dropConnection(connection, reason); });

    sendHello(connection);

    if (link->trusted && !link->deviceId.isEmpty()) {
        emit deviceConnected(link->deviceId);
        emit deviceListChanged();
    }
}

void DeviceManager::dropConnection(Connection *connection, const QString &reason) {
    if (!reason.isEmpty()) {
        qCWarning(lcLink) << "dropping link:" << reason;
    }
    for (qsizetype i = 0; i < m_links.size(); ++i) {
        if (m_links[i]->connection != connection) {
            continue;
        }
        const QString deviceId = m_links[i]->deviceId;
        const bool wasTrusted = m_links[i]->trusted;
        m_links.removeAt(i);

        forgetPendingWork(deviceId);

        if (!reason.isEmpty() && !wasTrusted) {
            emit pairingFailed(deviceId, reason);
        }
        if (wasTrusted && !deviceId.isEmpty()) {
            emit deviceDisconnected(deviceId);
        }
        emit deviceListChanged();
        break;
    }
    connection->deleteLater();
}

void DeviceManager::forgetPendingWork(const QString &deviceId) {
    if (deviceId.isEmpty()) {
        return;
    }
    // Every one of these sets exists to refuse an answer nobody asked for.
    // An entry that outlives its link inverts that: the first genuine answer
    // after a reconnect looks unsolicited and is dropped.
    m_statusRequesters.remove(deviceId);
    // Forget the revision too: a reconnected device must be sent the whole
    // snapshot, not told that nothing changed since a link it no longer has.
    m_lastStatusSent.remove(deviceId);
    m_aiModelRequesters.remove(deviceId);
    m_guardStatusRequesters.remove(deviceId);

    // The privileged one matters most. m_guardRequester is a single slot, and
    // a device that vanished mid-toggle would hold it forever — after which
    // *every* killswitch request from *any* device is refused with "another
    // change is in progress", permanently, with no way back but a restart.
    if (m_guardRequester == deviceId) {
        m_guardRequester.clear();
    }

    // In-flight work keyed by our own handle: the answers have nowhere to go
    // now, and leaving them would deliver one device's output to whoever
    // reuses its id later.
    for (auto it = m_commandRequests.begin(); it != m_commandRequests.end();) {
        it = it->deviceId == deviceId ? m_commandRequests.erase(it) : std::next(it);
    }
    for (auto it = m_aiRequests.begin(); it != m_aiRequests.end();) {
        it = it->deviceId == deviceId ? m_aiRequests.erase(it) : std::next(it);
    }
}

void DeviceManager::sendHello(Connection *connection) {
    // What we implement, not what the peer is allowed to use: the peer's own
    // per-device switch decides the latter, and it decides it on its side.
    Message hello = Message::hello(connection->nextCounter(), m_deviceId, m_deviceName,
                                   m_deviceType, supportedCapabilities(),
                                   QString::number(kProtocolVersion));
    connection->send(hello);
}

bool DeviceManager::capabilityAllowed(const Link &link, Capability capability) const {
    if (!link.trusted) {
        return false;
    }
    // Both sides must offer it, and the local user must have enabled it for
    // this specific device. An advertisement alone grants nothing.
    if (!link.peerCapabilities.testFlag(capability)) {
        return false;
    }
    const PairedDevice device = m_store.deviceForKey(link.connection->peerPublicKey());
    return device.isValid() && device.enabledCapabilities.testFlag(capability);
}

bool DeviceManager::allowPairingAttempt(const QString &peerKeyHex) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QQueue<QDateTime> &attempts = m_pairingAttempts[peerKeyHex];
    while (!attempts.isEmpty() && attempts.head().secsTo(now) > 60) {
        attempts.dequeue();
    }
    if (attempts.size() >= limits::kPairingAttemptsPerMinute) {
        return false;
    }
    attempts.enqueue(now);
    return true;
}

void DeviceManager::handleMessage(Connection *connection, const Message &message) {
    // Held by shared_ptr for the whole call: dropConnection() below removes
    // the link from m_links, and this reference must stay valid until we
    // return.
    const auto link = linkFor(connection);
    if (!link) {
        return;
    }

    // ---- Gate: an untrusted peer may only pair ---------------------------
    // This is the other half of the transport's trust model. Connection lets
    // an unknown key complete TLS so pairing is possible at all; this is what
    // stops such a peer from doing anything else with that access.
    if (!link->trusted) {
        switch (message.type()) {
        case MessageType::Hello:
        case MessageType::PairRequest:
        case MessageType::PairResponse:
        case MessageType::PairResult:
            break;
        default:
            // A peer asking for anything but pairing believes it is paired
            // with us, and it is not — so it was removed here while it was
            // offline, and the unpair notice sent at the time went nowhere.
            // Say so now, or it keeps this computer in its list forever and
            // reconnects only to be refused again.
            //
            // Safe unconditionally: a peer genuinely mid-pairing only sends
            // the four types above and never reaches this branch.
            connection->send(Message::unpair(connection->nextCounter()));
            dropConnection(connection,
                           QStringLiteral("unpaired peer attempted %1")
                               .arg(Message::typeName(message.type())));
            return;
        }
    }

    // Hello must be the first message on any link. Everything downstream —
    // the device id a pairing is filed under, the capability set, the
    // paired-id consistency check — is established by it, so accepting
    // anything before it means acting on a peer we have not identified.
    // (This was not hypothetical: a client that sent hello and pairRequest
    // from two independent coroutines raced, and the pairing was filed
    // under an empty id.)
    if (!link->helloReceived && message.type() != MessageType::Hello) {
        dropConnection(connection,
                       QStringLiteral("first message was %1, expected hello")
                           .arg(Message::typeName(message.type())));
        return;
    }

    switch (message.type()) {
    case MessageType::Hello: {
        qCInfo(lcLink) << "hello received";
        if (link->helloReceived) {
            dropConnection(connection, QStringLiteral("duplicate hello"));
            return;
        }
        link->helloReceived = true;

        const QString deviceId = message.string(QLatin1StringView("deviceId"),
                                                limits::kMaxDeviceIdChars);
        const QString deviceName = message.string(QLatin1StringView("deviceName"),
                                                  limits::kMaxDeviceNameChars);
        if (deviceId.isEmpty() || deviceName.isEmpty()) {
            dropConnection(connection, QStringLiteral("malformed hello"));
            return;
        }

        if (link->trusted) {
            // For a paired device the id is already fixed by the pin. A peer
            // claiming a different id over an authenticated link is either a
            // bug or an attempt to be treated as another device.
            const PairedDevice device = m_store.deviceForKey(connection->peerPublicKey());
            if (device.deviceId != deviceId) {
                dropConnection(connection, QStringLiteral("paired device changed its id"));
                return;
            }
        } else {
            link->deviceId = deviceId;
        }
        link->deviceName = deviceName;
        link->deviceType = message.string(QLatin1StringView("deviceType"), 16);
        link->peerCapabilities = capabilitiesFromNames(
            message.stringList(QLatin1StringView("capabilities"), 32, 32));
        emit deviceListChanged();
        break;
    }

    case MessageType::PairRequest: {
        qCInfo(lcLink) << "pair request from" << link->deviceId;
        if (link->trusted) {
            dropConnection(connection, QStringLiteral("pair request on an already-paired link"));
            return;
        }
        if (!allowPairingAttempt(hexKey(connection->peerPublicKey()))) {
            dropConnection(connection, QStringLiteral("pairing attempts rate-limited"));
            return;
        }

        const QByteArray theirNonce = message.binary(QLatin1StringView("nonce"), Sas::kNonceSize);
        if (theirNonce.isEmpty()) {
            dropConnection(connection, QStringLiteral("malformed pairing nonce"));
            return;
        }
        link->theirNonce = theirNonce;
        link->ourNonce = Sas::generateNonce();
        if (link->ourNonce.isEmpty()) {
            dropConnection(connection, QStringLiteral("could not generate a pairing nonce"));
            return;
        }

        // We are the responder: the initiator's key comes first.
        const QString code = Sas::derive(connection->peerPublicKey(), m_identity.publicKey(),
                                         link->theirNonce, link->ourNonce);
        if (code.isEmpty()) {
            dropConnection(connection, QStringLiteral("could not derive a verification code"));
            return;
        }

        Message response = Message::pairResponse(connection->nextCounter(), link->ourNonce);
        connection->send(response);

        PendingPairing pairing;
        pairing.deviceId = link->deviceId;
        pairing.deviceName = link->deviceName;
        pairing.deviceType = link->deviceType;
        pairing.peerPublicKey = connection->peerPublicKey();
        pairing.verificationCode = code;
        pairing.weInitiated = false;
        m_pendingPairings.insert(link->deviceId, pairing);
        qCInfo(lcLink) << "showing verification code for" << link->deviceId;
        emit pairingRequested(pairing);
        break;
    }

    case MessageType::PairResponse: {
        auto it = m_pendingPairings.find(link->deviceId);
        if (it == m_pendingPairings.end() || !it->weInitiated) {
            dropConnection(connection, QStringLiteral("unexpected pair response"));
            return;
        }
        const QByteArray theirNonce = message.binary(QLatin1StringView("nonce"), Sas::kNonceSize);
        if (theirNonce.isEmpty()) {
            dropConnection(connection, QStringLiteral("malformed pairing nonce"));
            return;
        }
        link->theirNonce = theirNonce;

        // We initiated, so our key comes first.
        const QString code = Sas::derive(m_identity.publicKey(), connection->peerPublicKey(),
                                         link->ourNonce, link->theirNonce);
        if (code.isEmpty()) {
            dropConnection(connection, QStringLiteral("could not derive a verification code"));
            return;
        }
        it->verificationCode = code;
        it->peerPublicKey = connection->peerPublicKey();
        emit pairingRequested(*it);
        break;
    }

    case MessageType::PairResult: {
        auto it = m_pendingPairings.find(link->deviceId);
        if (it == m_pendingPairings.end()) {
            dropConnection(connection, QStringLiteral("unexpected pair result"));
            return;
        }
        it->remoteAccepted = message.boolean(QLatin1StringView("accepted"));
        if (!it->remoteAccepted) {
            m_pendingPairings.erase(it);
            emit pairingCompleted(link->deviceId, false);
            connection->disconnectFromPeer();
            return;
        }
        finalizePairing(link);
        break;
    }

    case MessageType::Unpair: {
        const QByteArray key = connection->peerPublicKey();
        const PairedDevice device = m_store.deviceForKey(key);
        m_store.remove(key);
        emit pairingCompleted(device.deviceId, false);
        emit deviceListChanged();
        connection->disconnectFromPeer();
        break;
    }

    case MessageType::FileOffer: {
        if (!capabilityAllowed(*link, Capability::FileTransfer)) {
            break;
        }
        const qint64 rawId = message.integer(QLatin1StringView("transferId"), 0xFFFFFFFFLL);
        const QString filename = message.string(QLatin1StringView("filename"),
                                                limits::kMaxFilenameChars);
        const qint64 size = message.integer(QLatin1StringView("size"), limits::kMaxFileBytes);
        if (rawId < 0 || size < 0 || filename.isEmpty()) {
            break;
        }
        const auto transferId = static_cast<quint32>(rawId);

        QString reason;
        if (!m_receiver->validateOffer(transferId, filename, size, reason)) {
            Message reject = Message::fileReject(connection->nextCounter(), transferId, reason);
            connection->send(reject);
            emit fileFailed(link->deviceId, transferId, reason);
            break;
        }

        PendingFileOffer offer;
        offer.deviceId = link->deviceId;
        offer.transferId = transferId;
        offer.filename = filename;
        offer.sizeBytes = size;
        m_pendingOffers.insert(transferId, offer);
        // Never auto-accept: an incoming file always waits for the user.
        emit fileOffered(offer);
        break;
    }

    case MessageType::FileAccept: {
        if (!capabilityAllowed(*link, Capability::FileTransfer)) {
            break;
        }
        const qint64 rawId = message.integer(QLatin1StringView("transferId"), 0xFFFFFFFFLL);
        if (rawId < 0) {
            break;
        }
        const auto transferId = static_cast<quint32>(rawId);
        const QString path = m_outgoingTransfers.value(transferId);
        if (path.isEmpty()) {
            break;
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            Message cancel = Message::fileCancel(connection->nextCounter(), transferId,
                                                 QStringLiteral("cannot read source file"));
            connection->send(cancel);
            m_outgoingTransfers.remove(transferId);
            break;
        }
        while (!file.atEnd()) {
            const QByteArray chunk = file.read(kSendChunkSize);
            if (chunk.isEmpty() || !connection->sendData(transferId, chunk)) {
                break;
            }
        }
        file.close();

        Message complete = Message::fileComplete(connection->nextCounter(), transferId);
        connection->send(complete);
        m_outgoingTransfers.remove(transferId);
        break;
    }

    case MessageType::FileReject:
    case MessageType::FileCancel: {
        const qint64 rawId = message.integer(QLatin1StringView("transferId"), 0xFFFFFFFFLL);
        if (rawId < 0) {
            break;
        }
        const auto transferId = static_cast<quint32>(rawId);
        m_outgoingTransfers.remove(transferId);
        m_receiver->cancel(transferId);
        m_pendingOffers.remove(transferId);
        emit fileFailed(link->deviceId, transferId,
                        message.string(QLatin1StringView("reason"), 256));
        break;
    }

    case MessageType::FileComplete: {
        if (!capabilityAllowed(*link, Capability::FileTransfer)) {
            break;
        }
        const qint64 rawId = message.integer(QLatin1StringView("transferId"), 0xFFFFFFFFLL);
        if (rawId < 0) {
            break;
        }
        const auto transferId = static_cast<quint32>(rawId);

        QString finalPath;
        QString reason;
        if (m_receiver->finish(transferId, finalPath, reason)) {
            emit fileReceived(link->deviceId, finalPath);
        } else {
            emit fileFailed(link->deviceId, transferId, reason);
        }
        m_pendingOffers.remove(transferId);
        break;
    }

    case MessageType::StatusRequest: {
        if (!capabilityAllowed(*link, Capability::SystemStatus)) {
            // Answered, not ignored. Silence here is indistinguishable from
            // a slow machine, and a phone that shows "asking…" forever is
            // how a revoked capability looks like a broken app.
            connection->send(
                Message::statusUnavailable(connection->nextCounter(), QStringLiteral("this computer has that switched off for your device")));
            break;
        }
        // Joining the queue is the whole of the peer's influence here: it
        // cannot pass an argument, choose a probe, or make the helper run
        // more often than StatusProvider's own cache allows.
        m_statusRequesters.insert(link->deviceId);
        m_status.request();
        break;
    }

    case MessageType::StatusReport: {
        // Having asked is the authorisation, not merely being paired: a
        // report nobody requested has no reason to reach the UI.
        if (!m_statusAwaiting.remove(link->deviceId)) {
            qCInfo(lcLink) << "ignoring unsolicited status report from" << link->deviceId;
            break;
        }
        const QString error = message.string(QLatin1StringView("error"), 200);
        emit statusReportReceived(link->deviceId,
                                  message.unvalidatedObject(QLatin1StringView("status")),
                                  error);
        break;
    }

    case MessageType::StatusUnchanged: {
        // The far side is telling us its snapshot is the one we already have.
        // Keep it, and report that we are still current rather than
        // pretending a fresh reading arrived.
        if (!m_statusAwaiting.remove(link->deviceId)) {
            break;
        }
        emit statusReportReceived(link->deviceId, QJsonObject(), QString());
        break;
    }

    case MessageType::CommandList: {
        if (!capabilityAllowed(*link, Capability::Commands)) {
            connection->send(Message::commandCatalog(
                connection->nextCounter(), QJsonArray(),
                QStringLiteral("this computer has that switched off for your device")));
            break;
        }
        QJsonArray entries;
        for (const Command &command : m_commands.catalog()) {
            QJsonObject entry;
            entry.insert(QLatin1StringView("id"), command.id);
            entry.insert(QLatin1StringView("label"), command.label);
            entry.insert(QLatin1StringView("confirm"), command.confirm);
            entry.insert(QLatin1StringView("pinned"), command.pinned);
            // argv is deliberately absent — see Message::commandCatalog.
            entries.append(entry);
        }
        Message catalog = Message::commandCatalog(connection->nextCounter(), entries);
        connection->send(catalog);
        break;
    }

    case MessageType::CommandRun: {
        if (!capabilityAllowed(*link, Capability::Commands)) {
            const qint64 refusedId = message.integer(QLatin1StringView("requestId"),
                                                     0xFFFFFFFFLL, 0);
            connection->send(Message::commandRefused(
                connection->nextCounter(), static_cast<quint32>(refusedId), QStringLiteral("this computer has that switched off for your device")));
            break;
        }
        const qint64 rawRequest = message.integer(QLatin1StringView("requestId"), 0xFFFFFFFFLL);
        // Bounded by the same length the catalogue enforces, so a string too
        // long to be any id is refused before it is looked up. It would be
        // refused by the lookup anyway; this just keeps the ceiling in one
        // place.
        const QString id = message.string(QLatin1StringView("id"), CommandRunner::kMaxIdChars);
        if (rawRequest < 0) {
            break;
        }
        const auto peerRequestId = static_cast<quint32>(rawRequest);

        // The id is matched exactly against the user's file. Nothing about
        // it is parsed, split or interpreted — a peer sending "rm -rf /" as
        // an id gets the same answer as one sending "nonsense": no such
        // entry. There is no shell for it to reach.
        const quint32 handle = id.isEmpty() ? 0u : m_commands.run(id);
        if (handle == 0) {
            Message refused = Message::commandRefused(
                connection->nextCounter(), peerRequestId,
                QStringLiteral("no such command, or too many already running"));
            connection->send(refused);
            break;
        }
        m_commandRequests.insert(handle, {link->deviceId, peerRequestId});
        break;
    }

    case MessageType::CommandCatalog:
    case MessageType::CommandResult:
        // The desktop answers these; it never asks. A phone has no command
        // list for us to run.
        qCInfo(lcLink) << "ignoring unsolicited" << Message::typeName(message.type())
                       << "from" << link->deviceId;
        break;

    case MessageType::AiModels: {
        if (!capabilityAllowed(*link, Capability::Ai)) {
            connection->send(
                Message::aiModelList(connection->nextCounter(), {}, QStringLiteral("this computer has that switched off for your device")));
            break;
        }
        m_aiModelRequesters.insert(link->deviceId);
        m_ai.listModels();
        break;
    }

    case MessageType::AiPrompt: {
        if (!capabilityAllowed(*link, Capability::Ai)) {
            const qint64 refusedId = message.integer(QLatin1StringView("requestId"),
                                                     0xFFFFFFFFLL, 0);
            connection->send(Message::aiDone(connection->nextCounter(),
                                             static_cast<quint32>(refusedId), QStringLiteral("this computer has that switched off for your device")));
            break;
        }
        const qint64 rawRequest = message.integer(QLatin1StringView("requestId"), 0xFFFFFFFFLL);
        const QString model = message.string(QLatin1StringView("model"), 128);
        const QString text = message.string(QLatin1StringView("text"),
                                            AiBridge::kMaxPromptChars);
        if (rawRequest < 0) {
            break;
        }
        const auto peerRequestId = static_cast<quint32>(rawRequest);

        // The conversation is keyed by device, so two phones paired to this
        // machine never read each other's history.
        const quint32 handle = m_ai.ask(link->deviceId, model, text);
        if (handle == 0) {
            connection->send(Message::aiDone(
                connection->nextCounter(), peerRequestId,
                QStringLiteral("that model is not available, or the message was empty")));
            break;
        }
        m_aiRequests.insert(handle, {link->deviceId, peerRequestId});
        break;
    }

    case MessageType::AiModelList:
    case MessageType::AiChunk:
    case MessageType::AiDone:
        // We answer these; we never ask. A phone has no Ollama for us.
        qCInfo(lcLink) << "ignoring unsolicited" << Message::typeName(message.type())
                       << "from" << link->deviceId;
        break;

    case MessageType::GuardStatus: {
        if (!capabilityAllowed(*link, Capability::GuardControl)) {
            connection->send(
                Message::guardReport(connection->nextCounter(), QJsonArray(), QStringLiteral("this computer has that switched off for your device")));
            break;
        }
        m_guardStatusRequesters.insert(link->deviceId);
        m_guard.requestStatus();
        break;
    }

    case MessageType::GuardRequest: {
        if (!capabilityAllowed(*link, Capability::GuardControl)) {
            connection->send(Message::guardResult(
                connection->nextCounter(),
                message.string(QLatin1StringView("device"), 32),
                QStringLiteral("unknown"), QStringLiteral("this computer has that switched off for your device")));
            break;
        }
        const QString deviceName = message.string(QLatin1StringView("device"), 32);

        // The name is looked up in a fixed table and becomes an enum. Nothing
        // the peer sent reaches the broker as text — a "device" of
        // "camera off\nPANIC" is simply not a device, and there is no code
        // path that could turn it into one.
        GuardDevice device{};
        if (!guardDeviceFromName(deviceName, device)) {
            connection->send(Message::guardResult(connection->nextCounter(), deviceName,
                                                  QStringLiteral("unknown"),
                                                  QStringLiteral("no such device")));
            break;
        }

        // One privileged change at a time. Overlapping toggles would make the
        // "state afterwards" ambiguous, and ambiguity is not something to
        // report about a security control.
        if (!m_guardRequester.isEmpty()) {
            connection->send(Message::guardResult(connection->nextCounter(), deviceName,
                                                  QStringLiteral("unknown"),
                                                  QStringLiteral("another change is in progress")));
            break;
        }

        m_guardRequester = link->deviceId;
        // "on" on the wire is maze-guardd's vocabulary: the *device* is
        // enabled. Blocking is `on: false`.
        m_guard.setDeviceEnabled(device, message.boolean(QLatin1StringView("on")));
        break;
    }

    case MessageType::GuardReport:
    case MessageType::GuardResult:
    case MessageType::OpenOnPhone:
        // We answer GuardReport/GuardResult; we never ask. OpenOnPhone is
        // computer -> phone only and a phone has no clipboard to push us.
        // Either way, a phone sending one of these believes something about
        // the direction of this protocol that is not true.
        qCInfo(lcLink) << "ignoring unsolicited" << Message::typeName(message.type())
                       << "from" << link->deviceId;
        break;

    case MessageType::Ping:
    case MessageType::Pong:
        // Never actually reached: Connection answers/consumes these itself
        // before messageReceived() is ever emitted. Listed only so this
        // switch stays exhaustive.
        break;

    case MessageType::Unknown:
        dropConnection(connection, QStringLiteral("unknown message type"));
        break;
    }
}

void DeviceManager::deliverGuardReport(const QMap<GuardDevice, GuardState> &states,
                                       const QString &error) {
    QJsonArray devices;
    for (auto it = states.cbegin(); it != states.cend(); ++it) {
        QJsonObject entry;
        entry.insert(QLatin1StringView("device"), guardDeviceName(it.key()));
        entry.insert(QLatin1StringView("state"), guardStateName(it.value()));
        devices.append(entry);
    }

    const QSet<QString> waiting = std::exchange(m_guardStatusRequesters, {});
    for (const QString &deviceId : waiting) {
        Connection *connection = connectionForDevice(deviceId);
        if (!connection) {
            continue;
        }
        const auto link = linkFor(connection);
        if (!link || !capabilityAllowed(*link, Capability::GuardControl)) {
            continue;
        }
        connection->send(Message::guardReport(connection->nextCounter(), devices, error));
    }
}

bool DeviceManager::routeAi(quint32 requestId, Connection *&connection, quint32 &peerRequestId) {
    const auto it = m_aiRequests.constFind(requestId);
    if (it == m_aiRequests.constEnd()) {
        return false;
    }
    connection = connectionForDevice(it->deviceId);
    if (!connection) {
        return false;
    }
    // Re-checked on every chunk, not only when the prompt arrived. A reply
    // can run for a minute; permission switched off halfway through must
    // stop the stream, not merely the next one.
    const auto link = linkFor(connection);
    if (!link || !capabilityAllowed(*link, Capability::Ai)) {
        return false;
    }
    peerRequestId = it->peerRequestId;
    return true;
}

void DeviceManager::deliverAiModels(const QStringList &models, const QString &error) {
    const QSet<QString> waiting = std::exchange(m_aiModelRequesters, {});
    for (const QString &deviceId : waiting) {
        Connection *connection = connectionForDevice(deviceId);
        if (!connection) {
            continue;
        }
        const auto link = linkFor(connection);
        if (!link || !capabilityAllowed(*link, Capability::Ai)) {
            continue;
        }
        connection->send(Message::aiModelList(connection->nextCounter(), models, error));
    }
}

void DeviceManager::deliverStatusReports(const QJsonObject &snapshot, const QString &error) {
    const QSet<QString> waiting = std::exchange(m_statusRequesters, {});
    for (const QString &deviceId : waiting) {
        Connection *connection = connectionForDevice(deviceId);
        if (!connection) {
            continue;
        }
        // Re-checked rather than assumed: the user may have switched the
        // capability off while the helper was running, and a snapshot that
        // was authorised when it was asked for is not authorised now.
        const auto link = linkFor(connection);
        if (!link || !capabilityAllowed(*link, Capability::SystemStatus)) {
            continue;
        }
        if (!error.isEmpty()) {
            connection->send(Message::statusUnavailable(connection->nextCounter(), error));
            m_lastStatusSent.remove(deviceId);
            continue;
        }

        // Hash the snapshot rather than diff it: the payload is small, the
        // comparison has to be exact, and a hash keeps no second copy of the
        // user's system information in memory.
        const QByteArray digest = QCryptographicHash::hash(
            QJsonDocument(snapshot).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);

        if (m_lastStatusSent.value(deviceId) == digest) {
            // Nothing has changed since this device's last answer. It gets a
            // few bytes instead of a fresh copy of what it already has.
            connection->send(Message::statusUnchanged(connection->nextCounter()));
            continue;
        }
        m_lastStatusSent.insert(deviceId, digest);
        connection->send(Message::statusReport(connection->nextCounter(), snapshot));
    }
}

void DeviceManager::handleData(Connection *connection, quint32 transferId,
                               const QByteArray &chunk) {
    const auto link = linkFor(connection);
    if (!link || !capabilityAllowed(*link, Capability::FileTransfer)) {
        dropConnection(connection, QStringLiteral("data frame without file-transfer capability"));
        return;
    }
    if (!m_receiver->hasTransfer(transferId)) {
        // Data for a transfer the user never accepted: refuse rather than
        // start writing on the strength of the data frame alone.
        dropConnection(connection, QStringLiteral("data for an unaccepted transfer"));
        return;
    }

    QString reason;
    if (!m_receiver->appendChunk(transferId, chunk, reason)) {
        Message cancel = Message::fileCancel(connection->nextCounter(), transferId, reason);
        connection->send(cancel);
        emit fileFailed(link->deviceId, transferId, reason);
    }
}

void DeviceManager::finalizePairing(const std::shared_ptr<Link> &link) {
    auto it = m_pendingPairings.find(link->deviceId);
    if (it == m_pendingPairings.end()) {
        return;
    }
    // Both sides must have said yes. A single-sided accept never pins.
    if (!it->localAccepted || !it->remoteAccepted) {
        return;
    }

    PairedDevice device;
    device.deviceId = it->deviceId;
    device.deviceName = it->deviceName;
    device.deviceType = it->deviceType;
    device.publicKey = it->peerPublicKey;
    device.pairedAt = QDateTime::currentDateTimeUtc();
    device.enabledCapabilities = defaultEnabledCapabilities();

    const QString deviceId = it->deviceId;
    m_pendingPairings.erase(it);

    if (!m_store.add(device)) {
        emit pairingFailed(deviceId, QStringLiteral("could not store the pairing"));
        link->connection->disconnectFromPeer();
        return;
    }

    link->trusted = true;
    emit pairingCompleted(deviceId, true);
    emit deviceConnected(deviceId);
    emit deviceListChanged();
}

DeviceManager::PairingStart DeviceManager::requestPairing(const QString &deviceId) {
    if (m_store.deviceForId(deviceId).isValid()) {
        return PairingStart::AlreadyPaired;
    }
    if (m_pendingPairings.contains(deviceId)) {
        return PairingStart::AlreadyPending;
    }

    DiscoveredDevice target;
    for (const DiscoveredDevice &device : discoveredDevices()) {
        if (device.deviceId == deviceId) {
            target = device;
            break;
        }
    }
    if (target.deviceId.isEmpty()) {
        return PairingStart::NotDiscovered;
    }
    return requestPairingAt(target.address, target.port, deviceId)
               ? PairingStart::Started
               : PairingStart::AlreadyPending;
}

bool DeviceManager::requestPairingAt(const QHostAddress &address, quint16 port,
                                     const QString &deviceId) {
    if (m_store.deviceForId(deviceId).isValid()) {
        return false; // already paired
    }
    if (m_pendingPairings.contains(deviceId)) {
        return false; // one pairing at a time per device
    }

    auto *connection = new Connection(m_identity, &m_store, Connection::Mode::Pairing, this);

    PendingPairing pairing;
    pairing.deviceId = deviceId;
    pairing.deviceName = deviceId;
    pairing.weInitiated = true;
    m_pendingPairings.insert(deviceId, pairing);

    connect(connection, &Connection::established, this, [this, connection, deviceId]() {
        wireConnection(connection);
        const auto link = linkFor(connection);
        if (!link) {
            return;
        }
        link->deviceId = deviceId;
        link->ourNonce = Sas::generateNonce();
        if (link->ourNonce.isEmpty()) {
            dropConnection(connection, QStringLiteral("could not generate a pairing nonce"));
            return;
        }
        Message request = Message::pairRequest(connection->nextCounter(), link->ourNonce);
        connection->send(request);
    });
    connect(connection, &Connection::failed, this, [this, deviceId](const QString &reason) {
        m_pendingPairings.remove(deviceId);
        emit pairingFailed(deviceId, reason);
    });

    connection->connectToPeer(address, port);
    return true;
}

void DeviceManager::respondToPairing(const QString &deviceId, bool accept) {
    auto it = m_pendingPairings.find(deviceId);
    if (it == m_pendingPairings.end()) {
        return;
    }
    const auto link = linkForDevice(deviceId);
    if (!link || link->connection == nullptr) {
        m_pendingPairings.erase(it);
        return;
    }

    it->localAccepted = accept;

    Message result = Message::pairResult(link->connection->nextCounter(), accept);
    link->connection->send(result);

    if (!accept) {
        m_pendingPairings.erase(it);
        emit pairingCompleted(deviceId, false);
        link->connection->disconnectFromPeer();
        return;
    }
    finalizePairing(link);
}

bool DeviceManager::unpair(const QString &deviceId) {
    const PairedDevice device = m_store.deviceForId(deviceId);
    if (!device.isValid()) {
        return false;
    }
    if (Connection *connection = connectionForDevice(deviceId)) {
        Message message = Message::unpair(connection->nextCounter());
        connection->send(message);
        connection->disconnectFromPeer();
    }
    const bool removed = m_store.remove(device.publicKey);
    emit deviceListChanged();
    return removed;
}

bool DeviceManager::setCapabilityEnabled(const QString &deviceId, Capability capability,
                                         bool enabled) {
    const PairedDevice device = m_store.deviceForId(deviceId);
    if (!device.isValid()) {
        return false;
    }
    Capabilities caps = device.enabledCapabilities;
    caps.setFlag(capability, enabled);
    const bool ok = m_store.update(device.publicKey, device.deviceName, caps);
    if (ok) {
        emit deviceListChanged();
    }
    return ok;
}

bool DeviceManager::sendFile(const QString &deviceId, const QString &localPath) {
    const auto link = linkForDevice(deviceId);
    if (!link || !capabilityAllowed(*link, Capability::FileTransfer)) {
        return false;
    }
    const QFileInfo info(localPath);
    if (!info.exists() || !info.isFile() || info.size() > limits::kMaxFileBytes) {
        return false;
    }

    const quint32 transferId = m_nextTransferId++;
    m_outgoingTransfers.insert(transferId, localPath);

    Message offer = Message::fileOffer(link->connection->nextCounter(), transferId,
                                       info.fileName(), info.size());
    return link->connection->send(offer);
}

bool DeviceManager::sendOpenOnPhone(const QString &text) {
    if (text.isEmpty() || text.size() > limits::kMaxOpenTextChars) {
        return false;
    }
    for (const auto &link : m_links) {
        if (!capabilityAllowed(*link, Capability::OpenOnPhone)) {
            continue;
        }
        const Message message = Message::openOnPhone(link->connection->nextCounter(), text);
        return link->connection->send(message);
    }
    return false;
}

bool DeviceManager::requestStatus(const QString &deviceId) {
    const auto link = linkForDevice(deviceId);
    // The peer must offer systemStatus and the local user must have enabled
    // it for this device — the same two conditions as any other capability.
    // Asking is harmless, but a request we know will be refused is noise.
    if (!link || !capabilityAllowed(*link, Capability::SystemStatus)) {
        return false;
    }
    Message request = Message::statusRequest(link->connection->nextCounter());
    if (!link->connection->send(request)) {
        return false;
    }
    m_statusAwaiting.insert(deviceId);
    return true;
}

void DeviceManager::respondToFileOffer(const QString &deviceId, quint32 transferId, bool accept) {
    const auto link = linkForDevice(deviceId);
    if (!link) {
        return;
    }
    const auto it = m_pendingOffers.find(transferId);
    if (it == m_pendingOffers.end()) {
        return;
    }

    if (!accept) {
        Message reject = Message::fileReject(link->connection->nextCounter(), transferId,
                                             QStringLiteral("declined"));
        link->connection->send(reject);
        m_pendingOffers.erase(it);
        return;
    }

    QString reason;
    if (!m_receiver->begin(transferId, it->filename, it->sizeBytes, reason)) {
        Message reject = Message::fileReject(link->connection->nextCounter(), transferId, reason);
        link->connection->send(reject);
        m_pendingOffers.erase(it);
        emit fileFailed(deviceId, transferId, reason);
        return;
    }

    Message accepted = Message::fileAccept(link->connection->nextCounter(), transferId);
    link->connection->send(accepted);
}

void DeviceManager::onDeviceDiscovered(const DiscoveredDevice &device) {
    maybeConnectTo(device);
    emit deviceListChanged();
}

void DeviceManager::reconnectPairedDevices() {
    for (const DiscoveredDevice &device : discoveredDevices()) {
        maybeConnectTo(device);
    }
}

void DeviceManager::maybeConnectTo(const DiscoveredDevice &device) {
    const PairedDevice paired = m_store.deviceForId(device.deviceId);
    if (!paired.isValid()) {
        return; // not paired: only an explicit user action may dial it
    }
    if (linkForDevice(device.deviceId) != nullptr) {
        return; // already linked
    }
    if (m_outboundPending.contains(device.deviceId)) {
        return; // a dial is already in flight
    }

    // Exactly one side must dial, or one pair ends up with two links. Which
    // side is not arbitrary:
    //
    // **A phone always dials the computer, never the other way round.** The
    // computer is the stable end — fixed address, fixed port, always
    // listening. A phone changes networks, changes address, and on Android
    // may not be accepting connections at all in the background. Dialing
    // *into* a phone is the fragile direction, so it is not used.
    //
    // This replaced a lexicographic tiebreak on device ids, which decided the
    // direction by coin flip: when the phone's id happened to sort first, the
    // desktop refused to dial and the phone had no reconnect logic of its
    // own, so a link lost to a network blip never came back. Half of all
    // pairings were quietly unable to recover.
    //
    // Same-type pairs (two desktops) have no stable/mobile asymmetry to
    // exploit, so they keep the tiebreak.
    if (device.deviceType == QLatin1StringView("mobile")) {
        return; // it dials us
    }
    if (m_deviceType == device.deviceType && m_deviceId > device.deviceId) {
        return;
    }

    connectToPairedAt(device.address, device.port, device.deviceId);
}

bool DeviceManager::connectToPairedAt(const QHostAddress &address, quint16 port,
                                      const QString &deviceId) {
    if (!m_store.deviceForId(deviceId).isValid()) {
        return false; // only ever dial a device we already trust
    }
    if (linkForDevice(deviceId) != nullptr) {
        return false; // already linked
    }
    m_outboundPending.insert(deviceId);

    // Mode::Paired: this is a reconnect to a device we already trust, so an
    // unknown key here must fail rather than open a pairing window.
    auto *connection = new Connection(m_identity, &m_store, Connection::Mode::Paired, this);

    // These two handlers cover only the dial itself. They are disconnected
    // the moment the link is established, because wireConnection() installs
    // its own failed/disconnected handling and both firing on one failure
    // would tear the same connection down twice.
    auto *dialGuard = new QObject(connection);

    connect(connection, &Connection::established, dialGuard,
            [this, connection, deviceId, dialGuard]() {
                m_outboundPending.remove(deviceId);
                dialGuard->deleteLater();
                wireConnection(connection);
            });
    connect(connection, &Connection::failed, dialGuard,
            [this, connection, deviceId](const QString &reason) {
                Q_UNUSED(reason);
                m_outboundPending.remove(deviceId);
                // Retried on the next sweep rather than immediately, so an
                // unreachable peer cannot become a tight reconnect loop.
                connection->deleteLater();
            });

    connection->connectToPeer(address, port);
    return true;
}

} // namespace mazeconnect::core
