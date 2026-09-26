#include "mazeconnect/core/Connection.h"

#include "mazeconnect/core/DeviceStore.h"
#include "mazeconnect/core/Limits.h"

#include <QDateTime>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>

namespace mazeconnect::core {
namespace {

QSslConfiguration buildConfiguration(const Identity &identity, bool isServer) {
    QSslConfiguration config = QSslConfiguration::defaultConfiguration();

    // TLS 1.3 only. Pinning min == max removes the downgrade question
    // entirely rather than relying on a "prefer newest" heuristic.
    config.setProtocol(QSsl::TlsV1_3);

    const QSslCertificate certificate(identity.certificatePem(), QSsl::Pem);
    const QSslKey key(identity.privateKeyPem(), QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
    config.setLocalCertificate(certificate);
    config.setPrivateKey(key);

    // Both sides must present a certificate: this is mutual auth, and a
    // client that declines to identify itself is refused at the TLS layer.
    config.setPeerVerifyMode(QSslSocket::VerifyPeer);
    // Depth 1: a self-signed leaf is the only shape we ever expect.
    config.setPeerVerifyDepth(1);

    if (isServer) {
        // No session resumption: every connection re-proves possession of
        // the pinned key rather than riding on a earlier handshake.
        config.setSslOption(QSsl::SslOptionDisableSessionTickets, true);
    }
    return config;
}

} // namespace

Connection::Connection(QSslSocket *socket, const Identity &identity, const DeviceStore *store,
                       Mode mode, QObject *parent)
    : QObject(parent), m_socket(socket), m_identity(identity), m_store(store), m_mode(mode),
      m_isServer(true) {
    m_socket->setParent(this);
    configureSocket();
}

Connection::Connection(const Identity &identity, const DeviceStore *store, Mode mode,
                       QObject *parent)
    : QObject(parent), m_socket(new QSslSocket(this)), m_identity(identity), m_store(store),
      m_mode(mode), m_isServer(false) {
    configureSocket();
}

Connection::~Connection() = default;

void Connection::configureSocket() {
    m_socket->setSslConfiguration(buildConfiguration(m_identity, m_isServer));

    connect(m_socket, &QSslSocket::encrypted, this, &Connection::onEncrypted);
    connect(m_socket, &QSslSocket::readyRead, this, &Connection::onReadyRead);
    connect(m_socket, &QSslSocket::disconnected, this, &Connection::onDisconnected);
    connect(m_socket, &QSslSocket::sslErrors, this, &Connection::onSslErrors);
    connect(m_socket, &QAbstractSocket::errorOccurred, this, &Connection::onSocketError);
    connect(m_socket, &QSslSocket::encryptedBytesWritten, this, [this]() { emit bytesWritten(); });

    // A peer must not be able to hold handshake state open indefinitely.
    m_handshakeTimer.setSingleShot(true);
    m_handshakeTimer.setInterval(limits::kHandshakeTimeoutMs);
    connect(&m_handshakeTimer, &QTimer::timeout, this, &Connection::onHandshakeTimeout);

    m_heartbeatIntervalMs = limits::kHeartbeatIntervalMs;
    m_heartbeatTimeoutMs = limits::kHeartbeatTimeoutMs;
    // Ticks at the interval, not the timeout: onHeartbeatTick() itself
    // decides whether that's "send a Ping" or "we're past the timeout,
    // drop it", so a single recurring timer covers both.
    m_heartbeatTimer.setInterval(m_heartbeatIntervalMs);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &Connection::onHeartbeatTick);
}

void Connection::setHandshakeTimeout(int milliseconds) {
    m_handshakeTimer.setInterval(milliseconds);
}

void Connection::setHeartbeatIntervals(int intervalMilliseconds, int timeoutMilliseconds) {
    m_heartbeatIntervalMs = intervalMilliseconds;
    m_heartbeatTimeoutMs = timeoutMilliseconds;
    m_heartbeatTimer.setInterval(intervalMilliseconds);
}

void Connection::connectToPeer(const QHostAddress &address, quint16 port) {
    m_state = State::Connecting;
    m_handshakeTimer.start();
    m_socket->connectToHostEncrypted(address.toString(), port);
    m_state = State::Handshaking;
}

void Connection::startServerHandshake() {
    m_state = State::Handshaking;
    m_handshakeTimer.start();
    m_socket->startServerEncryption();
}

void Connection::disconnectFromPeer() {
    m_handshakeTimer.stop();
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
    }
}

void Connection::abortLink(const QString &reason) {
    fail(reason);
}

QHostAddress Connection::peerAddress() const {
    return m_socket->peerAddress();
}

QString Connection::peerFingerprint() const {
    if (m_peerPublicKey.isEmpty()) {
        return {};
    }
    return QString::fromLatin1(Identity::fingerprintOf(m_peerPublicKey).toHex());
}

void Connection::onSslErrors(const QList<QSslError> &errors) {
    // Our certificates are self-signed by design, so chain/hostname errors
    // are expected and meaningless here. They are ignored ONLY because
    // verifyPeerIdentity() below performs the real check against the pinned
    // key — that check is what authenticates the peer, not this.
    //
    // Anything outside this allow-list is still fatal: an unexpected error
    // class means something we have not reasoned about, and guessing is not
    // an option in this code path.
    for (const QSslError &error : errors) {
        switch (error.error()) {
        case QSslError::SelfSignedCertificate:
        case QSslError::SelfSignedCertificateInChain:
        case QSslError::HostNameMismatch:
        case QSslError::CertificateUntrusted:
        case QSslError::UnableToGetLocalIssuerCertificate:
        case QSslError::UnableToVerifyFirstCertificate:
            break;
        default:
            fail(QStringLiteral("TLS error: %1").arg(error.errorString()));
            return;
        }
    }
    m_socket->ignoreSslErrors(errors);
}

void Connection::onEncrypted() {
    m_handshakeTimer.stop();

    if (!verifyPeerIdentity()) {
        return; // verifyPeerIdentity() already failed the connection
    }

    m_state = State::Established;
    m_lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    m_heartbeatTimer.start();
    emit established();
}

bool Connection::verifyPeerIdentity() {
    const QSslCertificate peerCertificate = m_socket->peerCertificate();
    if (peerCertificate.isNull()) {
        fail(QStringLiteral("peer presented no certificate"));
        return false;
    }

    // Sanity-check the negotiated version rather than trusting that the
    // configuration was applied — this is cheap and catches a mis-set
    // configuration turning into a silent downgrade.
    if (m_socket->sessionProtocol() != QSsl::TlsV1_3) {
        fail(QStringLiteral("negotiated protocol is not TLS 1.3"));
        return false;
    }

    const QByteArray publicKey = Identity::publicKeyFromCertificateDer(peerCertificate.toDer());
    if (!Identity::isPlausiblePublicKey(publicKey)) {
        fail(QStringLiteral("peer certificate does not carry a P-256 key"));
        return false;
    }
    m_peerPublicKey = publicKey;

    // Presenting our own key back would let a peer "pair with us as us".
    if (publicKey == m_identity.publicKey()) {
        fail(QStringLiteral("peer presented our own identity key"));
        return false;
    }

    if (m_mode == Mode::Paired) {
        if (m_store == nullptr || !m_store->isTrusted(publicKey)) {
            // Hard fail. Never fall back to prompting the user here: a
            // mismatch on an established pairing is exactly what an attacker
            // looks like, and re-prompting would train the habit that makes
            // pinning worthless.
            fail(QStringLiteral("peer key is not paired (fingerprint %1)").arg(peerFingerprint()));
            return false;
        }
    }
    // In Pairing mode an unknown key is expected — that is the whole point.
    // Nothing is persisted until the user confirms the SAS.

    return true;
}

void Connection::onReadyRead() {
    // Any bytes at all prove the peer is alive, independent of what they
    // turn out to decode to.
    m_lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    m_parser.append(m_socket->readAll());

    for (;;) {
        FrameType type{};
        QByteArray payload;
        const FrameParser::Status status = m_parser.next(type, payload);

        if (status == FrameParser::Status::Incomplete) {
            return;
        }
        if (status == FrameParser::Status::Error) {
            fail(QStringLiteral("framing violation: %1").arg(m_parser.errorString()));
            return;
        }

        if (type == FrameType::Control) {
            const Message message = Message::parse(payload);
            if (!message.isValid()) {
                fail(QStringLiteral("malformed control message"));
                return;
            }
            // Replay/ordering check before the message reaches any handler,
            // so no side effect can happen on a replayed message.
            if (!m_replay.accept(message.counter())) {
                fail(QStringLiteral("replayed or out-of-window counter %1").arg(message.counter()));
                return;
            }
            // Heartbeat traffic is a transport concern, not an application
            // one: answered (or simply absorbed) here, never handed to
            // DeviceManager. The activity stamp above already covers "the
            // peer is alive" for both.
            if (message.type() == MessageType::Ping) {
                send(Message::pong(nextCounter()));
                continue;
            }
            if (message.type() == MessageType::Pong) {
                continue;
            }
            emit messageReceived(message);
        } else {
            quint32 transferId = 0;
            QByteArray chunk;
            if (!datachunk::decode(payload, transferId, chunk)) {
                fail(QStringLiteral("malformed data frame"));
                return;
            }
            emit dataReceived(transferId, chunk);
        }
    }
}

bool Connection::send(const Message &message) {
    if (m_state != State::Established) {
        return false;
    }
    const QByteArray frame = FrameParser::encode(FrameType::Control, message.toJson());
    if (frame.isEmpty()) {
        return false; // over cap — refuse rather than truncate
    }
    return m_socket->write(frame) == frame.size();
}

bool Connection::sendData(quint32 transferId, const QByteArray &chunk) {
    if (m_state != State::Established) {
        return false;
    }
    const QByteArray frame = FrameParser::encode(FrameType::Data,
                                                 datachunk::encode(transferId, chunk));
    if (frame.isEmpty()) {
        return false;
    }
    return m_socket->write(frame) == frame.size();
}

qint64 Connection::pendingWriteBytes() const {
    return m_socket->bytesToWrite() + m_socket->encryptedBytesToWrite();
}

void Connection::onHandshakeTimeout() {
    fail(QStringLiteral("handshake timed out"));
}

void Connection::onSocketError() {
    if (m_state == State::Established) {
        return; // a normal close surfaces via disconnected()
    }
    fail(m_socket->errorString());
}

void Connection::fail(const QString &reason) {
    m_handshakeTimer.stop();
    m_heartbeatTimer.stop();
    if (m_state == State::Disconnected) {
        return;
    }
    m_state = State::Disconnected;
    m_socket->abort();
    emit failed(reason);
}

void Connection::onDisconnected() {
    m_handshakeTimer.stop();
    m_heartbeatTimer.stop();
    m_state = State::Disconnected;
    emit disconnected();
}

void Connection::onHeartbeatTick() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 idleMs = now - m_lastActivityMs;
    if (idleMs > m_heartbeatTimeoutMs) {
        fail(QStringLiteral("heartbeat timeout: peer unresponsive"));
        return;
    }
    if (idleMs >= m_heartbeatIntervalMs) {
        send(Message::ping(nextCounter()));
    }
}

} // namespace mazeconnect::core
