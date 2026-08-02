#include "mazeconnect/core/Server.h"

#include "mazeconnect/core/DeviceStore.h"
#include "mazeconnect/core/Limits.h"

#include <QSslSocket>

namespace mazeconnect::core {

Server::Server(const Identity &identity, const DeviceStore *store, QObject *parent)
    : QTcpServer(parent), m_identity(identity), m_store(store) {}

bool Server::start(quint16 preferredPort) {
    // AnyIPv4 rather than Any: this is a LAN service and should not be
    // reachable over anything we did not intend.
    if (listen(QHostAddress::AnyIPv4, preferredPort)) {
        return true;
    }
    // Fall back to an ephemeral port if the preferred one is taken.
    return preferredPort != 0 && listen(QHostAddress::AnyIPv4, 0);
}

quint16 Server::port() const {
    return serverPort();
}

void Server::setPairingWindowOpen(bool open) {
    m_pairingWindowOpen = open;
}

void Server::releaseSlot(const QString &host) {
    const auto it = m_activePerHost.find(host);
    if (it == m_activePerHost.end()) {
        return;
    }
    if (--it.value() <= 0) {
        m_activePerHost.erase(it);
    }
}

void Server::incomingConnection(qintptr socketDescriptor) {
    auto *socket = new QSslSocket(this);
    if (!socket->setSocketDescriptor(socketDescriptor)) {
        delete socket;
        return;
    }

    const QHostAddress peer = socket->peerAddress();
    const QString host = peer.toString();

    // Bound how many sockets a single peer can hold open, so one host cannot
    // exhaust our descriptors by connecting repeatedly.
    if (m_activePerHost.value(host) >= limits::kMaxConnectionsPerPeer) {
        socket->abort();
        socket->deleteLater();
        emit connectionRejected(peer, QStringLiteral("too many concurrent connections"));
        return;
    }
    m_activePerHost[host] += 1;

    const Connection::Mode mode =
        m_pairingWindowOpen ? Connection::Mode::Pairing : Connection::Mode::Paired;

    auto *connection = new Connection(socket, m_identity, m_store, mode, this);
    connection->setHandshakeTimeout(m_handshakeTimeoutMs);

    connect(connection, &Connection::established, this, [this, connection]() {
        emit connectionEstablished(connection);
    });
    connect(connection, &Connection::failed, this,
            [this, connection, peer, host](const QString &reason) {
                releaseSlot(host);
                emit connectionRejected(peer, reason);
                connection->deleteLater();
            });
    connect(connection, &Connection::disconnected, this, [this, connection, host]() {
        releaseSlot(host);
        connection->deleteLater();
    });

    connection->startServerHandshake();
}

} // namespace mazeconnect::core
