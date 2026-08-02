#pragma once

#include <QHash>
#include <QHostAddress>
#include <QTcpServer>

#include "mazeconnect/core/Connection.h"
#include "mazeconnect/core/Identity.h"
#include "mazeconnect/core/Limits.h"

namespace mazeconnect::core {

class DeviceStore;

/**
 * Listener for inbound peer connections.
 *
 * Binds to the LAN only. Every accepted socket is immediately wrapped in a
 * Connection, which performs the mutual-TLS and pinning checks before the
 * link is handed upward — nothing above this layer ever sees an
 * unauthenticated peer.
 */
class Server : public QTcpServer {
    Q_OBJECT

public:
    /**
     * Default listening port — the same number the discovery beacon uses on
     * UDP, so there is one to remember rather than two.
     *
     * The port used to be ephemeral. That made the listener impossible to
     * write a firewall rule for, and it changed the "pair by address" string
     * on every restart, which is exactly the case where discovery is already
     * failing and the user is typing the address by hand. A fixed port costs
     * nothing: it is unprivileged, and the transport's security comes from
     * pinning and SAS, never from being hard to find.
     */
    static constexpr quint16 kDefaultPort = 38271;

    Server(const Identity &identity, const DeviceStore *store, QObject *parent = nullptr);

    /// Falls back to an ephemeral port when @p preferredPort is taken, so a
    /// second instance (or a test) still comes up.
    bool start(quint16 preferredPort = 0);
    quint16 port() const;

    /**
     * While pairing mode is on, one inbound connection carrying an unknown
     * key is permitted so the SAS can be compared. It is deliberately a
     * narrow, user-initiated, single-shot window rather than a mode the
     * daemon can sit in.
     */
    void setPairingWindowOpen(bool open);
    bool isPairingWindowOpen() const { return m_pairingWindowOpen; }

    /// Applied to every connection this server accepts. See
    /// Connection::setHandshakeTimeout.
    void setHandshakeTimeout(int milliseconds) { m_handshakeTimeoutMs = milliseconds; }

signals:
    void connectionEstablished(mazeconnect::core::Connection *connection);
    void connectionRejected(const QHostAddress &address, const QString &reason);

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private:
    void releaseSlot(const QString &host);

    Identity m_identity;
    const DeviceStore *m_store = nullptr;
    bool m_pairingWindowOpen = false;
    int m_handshakeTimeoutMs = limits::kHandshakeTimeoutMs;

    /// In-flight connections per source host, to bound what one peer can
    /// occupy.
    QHash<QString, int> m_activePerHost;
};

} // namespace mazeconnect::core
