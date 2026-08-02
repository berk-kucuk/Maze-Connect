#pragma once

#include <QObject>
#include <QSslSocket>
#include <QTimer>

#include "mazeconnect/core/Framing.h"
#include "mazeconnect/core/Identity.h"
#include "mazeconnect/core/Messages.h"
#include "mazeconnect/core/ReplayWindow.h"

namespace mazeconnect::core {

class DeviceStore;

/**
 * One mutually-authenticated TLS 1.3 link to a peer.
 *
 * Trust model, in order:
 *  1. TLS 1.3 is pinned as both minimum and maximum protocol version. There
 *     is no negotiation path to TLS 1.2 or below, so there is no downgrade
 *     to defend against later.
 *  2. Qt's own chain verification is deliberately bypassed — the
 *     certificates are self-signed and would never pass it — and replaced
 *     with an explicit pinned-key check. Bypassing verification is only safe
 *     *because* that check is unconditional; the two must always be changed
 *     together.
 *  3. In Paired mode the peer's Ed25519 key must already be in the
 *     DeviceStore. In Pairing mode an unknown key is allowed through, but
 *     only so the user can compare the SAS — nothing is persisted until the
 *     user confirms.
 */
class Connection : public QObject {
    Q_OBJECT

public:
    enum class Mode {
        Paired, ///< peer key must already be pinned; unknown keys are refused
        Pairing ///< unknown key permitted solely for SAS comparison
    };

    enum class State {
        Disconnected,
        Connecting,
        Handshaking,
        Established,
    };

    /// Wrap an already-accepted inbound socket.
    Connection(QSslSocket *socket, const Identity &identity, const DeviceStore *store,
               Mode mode, QObject *parent = nullptr);
    /// Create an outbound connection.
    Connection(const Identity &identity, const DeviceStore *store, Mode mode,
               QObject *parent = nullptr);

    /**
     * How long a handshake may take before the link is dropped.
     *
     * Injectable so tests can fail a stalled handshake in a bounded time.
     * With the production value a machine under load can take longer to
     * complete TLS than a test is willing to wait, which showed up as an
     * intermittent failure rather than as the timeout it actually was.
     */
    void setHandshakeTimeout(int milliseconds);

    /**
     * Override the heartbeat interval/timeout. Production values come from
     * Limits.h; tests inject short ones so a dead link surfaces inside the
     * test's own wait rather than seconds later, the same reasoning as
     * setHandshakeTimeout().
     */
    void setHeartbeatIntervals(int intervalMilliseconds, int timeoutMilliseconds);

    ~Connection() override;

    void connectToPeer(const QHostAddress &address, quint16 port);
    void startServerHandshake();
    void disconnectFromPeer();

    State state() const { return m_state; }
    Mode mode() const { return m_mode; }

    /// Peer's SPKI DER public key; empty until the TLS handshake completes.
    QByteArray peerPublicKey() const { return m_peerPublicKey; }
    QString peerFingerprint() const;

    QHostAddress peerAddress() const;

    /// Send a control message. Assigns the next outgoing counter.
    /// Const because it does not mutate: taking a non-const reference only
    /// forced every caller to name a temporary first.
    bool send(const Message &message);
    bool sendData(quint32 transferId, const QByteArray &chunk);

    /// Next counter to use for an outgoing message.
    quint64 nextCounter() { return ++m_outgoingCounter; }

signals:
    void established();
    void messageReceived(const mazeconnect::core::Message &message);
    void dataReceived(quint32 transferId, const QByteArray &chunk);
    void disconnected();
    /// Fatal protocol/trust failure; the connection is already closing.
    void failed(const QString &reason);

private slots:
    void onEncrypted();
    void onReadyRead();
    void onDisconnected();
    void onSslErrors(const QList<QSslError> &errors);
    void onSocketError();
    void onHandshakeTimeout();
    void onHeartbeatTick();

private:
    void configureSocket();
    bool verifyPeerIdentity();
    void fail(const QString &reason);

    QSslSocket *m_socket = nullptr;
    Identity m_identity;
    const DeviceStore *m_store = nullptr;
    Mode m_mode = Mode::Paired;
    State m_state = State::Disconnected;
    bool m_isServer = false;

    QByteArray m_peerPublicKey;
    FrameParser m_parser;
    ReplayWindow m_replay;
    quint64 m_outgoingCounter = 0;
    QTimer m_handshakeTimer;

    QTimer m_heartbeatTimer;
    int m_heartbeatIntervalMs = 0;
    int m_heartbeatTimeoutMs = 0;
    qint64 m_lastActivityMs = 0;
};

} // namespace mazeconnect::core
