#include <QtTest>

#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include "mazeconnect/core/Connection.h"
#include "mazeconnect/core/DeviceStore.h"
#include "mazeconnect/core/Identity.h"
#include "mazeconnect/core/Messages.h"
#include "mazeconnect/core/Server.h"

using namespace mazeconnect::core;

/**
 * End-to-end transport tests over a real TLS 1.3 socket pair on loopback.
 *
 * These are the tests that actually prove the trust model: that an unpaired
 * peer is refused, that a pinned one is admitted, and that the link really
 * negotiates TLS 1.3. Unit tests on the pieces cannot establish any of that.
 */
class TestTransport : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void rejectsUnpairedPeer();
    void clientRejectsUnpairedServer();
    void pairingModeAdmitsUnknownKey();
    void acceptsPairedPeer();
    void negotiatesTls13();
    void exchangesMessages();
    void rejectsReplayedCounter();
    void heartbeatPongsKeepAnIdleLinkAlive();
    void heartbeatDropsAnUnresponsivePeer();
    void bindsTheDefaultPortAndFallsBack();

private:
    /**
     * Short enough that a stalled handshake surfaces as a failure inside the
     * test's own wait, rather than the wait expiring first and the real
     * timeout firing seconds later — which read as a flaky test instead of
     * the timeout it was.
     */
    static constexpr int kTestHandshakeTimeoutMs = 3000;
    static constexpr int kWaitMs = 10000;

    struct Peer {
        Identity identity;
        std::unique_ptr<QTemporaryDir> dir;
        std::unique_ptr<DeviceStore> store;
    };

    static constexpr int kTestHeartbeatIntervalMs = 150;
    static constexpr int kTestHeartbeatTimeoutMs = 450;

    static Peer makePeer(const QString &name);
    static void pin(Peer &host, const Peer &guest, const QString &guestId);
};

/**
 * A bare TLS server socket standing in for a peer whose *application* has
 * stopped responding while its TCP connection is still technically up —
 * exactly what a half-open link looks like, and exactly what the heartbeat
 * exists to catch. Deliberately not wired to Connection: nothing here ever
 * reads a byte, so a Ping that lands is never answered with a Pong, no
 * matter how long the test waits.
 */
class UnresponsivePeer : public QTcpServer {
    Q_OBJECT
public:
    explicit UnresponsivePeer(const Identity &identity) : m_identity(identity) {}

    bool listen() { return QTcpServer::listen(QHostAddress::LocalHost, 0); }

protected:
    // Takes the raw descriptor directly, the same way Server::incomingConnection
    // does — going through QTcpServer::nextPendingConnection() first would
    // hand the fd to an intermediate QTcpSocket that fights the QSslSocket
    // below for ownership of it.
    void incomingConnection(qintptr socketDescriptor) override {
        auto *tls = new QSslSocket(this);
        if (!tls->setSocketDescriptor(socketDescriptor)) {
            tls->deleteLater();
            return;
        }

        QSslConfiguration config = QSslConfiguration::defaultConfiguration();
        config.setProtocol(QSsl::TlsV1_3);
        config.setLocalCertificate(QSslCertificate(m_identity.certificatePem(), QSsl::Pem));
        config.setPrivateKey(
            QSslKey(m_identity.privateKeyPem(), QSsl::Ec, QSsl::Pem, QSsl::PrivateKey));
        // Does not matter here: this stand-in never runs Connection's own
        // pinning check, so it has no reason to ask the client for a cert.
        config.setPeerVerifyMode(QSslSocket::VerifyNone);
        tls->setSslConfiguration(config);

        // No readyRead connection, intentionally: bytes arrive and sit
        // unread. That is the entire point.
        m_socket = tls;
        tls->startServerEncryption();
    }

private:
    Identity m_identity;
    QSslSocket *m_socket = nullptr;
};

void TestTransport::initTestCase() {
    QVERIFY2(QSslSocket::supportsSsl(), "TLS backend unavailable — transport tests cannot run");
}

TestTransport::Peer TestTransport::makePeer(const QString &name) {
    Peer peer;
    peer.identity = Identity::generate(name);
    peer.dir = std::make_unique<QTemporaryDir>();
    peer.store = std::make_unique<DeviceStore>(peer.dir->filePath(QStringLiteral("devices.json")));
    peer.store->load();
    return peer;
}

void TestTransport::pin(Peer &host, const Peer &guest, const QString &guestId) {
    PairedDevice device;
    device.deviceId = guestId;
    device.deviceName = guestId;
    device.deviceType = QStringLiteral("desktop");
    device.publicKey = guest.identity.publicKey();
    device.pairedAt = QDateTime::currentDateTimeUtc();
    QVERIFY(host.store->add(device));
}

void TestTransport::rejectsUnpairedPeer() {
    // Pin only in the client's direction, so the client is happy with the
    // server and the *server* is the side that has to refuse. Leaving both
    // stores empty would not isolate anything: whichever side aborted first
    // would end the connection, and the test would pass without ever
    // exercising the server's pinning check.
    Peer server = makePeer(QStringLiteral("server"));
    Peer client = makePeer(QStringLiteral("client"));
    pin(client, server, QStringLiteral("server-id"));

    Server listener(server.identity, server.store.get());
    listener.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QVERIFY(listener.start(0));

    QSignalSpy rejected(&listener, &Server::connectionRejected);
    QSignalSpy accepted(&listener, &Server::connectionEstablished);

    Connection client_(client.identity, client.store.get(), Connection::Mode::Paired);
    client_.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    client_.connectToPeer(QHostAddress::LocalHost, listener.port());

    QTRY_VERIFY_WITH_TIMEOUT(rejected.count() > 0, kWaitMs);
    QCOMPARE(accepted.count(), 0);
    QVERIFY2(client_.state() != Connection::State::Established,
             "client believed an unpaired connection was established");
}

void TestTransport::clientRejectsUnpairedServer() {
    // Mirror image: the server trusts the client, so the *client* is the
    // side that must refuse the server's unknown key.
    Peer server = makePeer(QStringLiteral("server"));
    Peer client = makePeer(QStringLiteral("client"));
    pin(server, client, QStringLiteral("client-id"));

    Server listener(server.identity, server.store.get());
    listener.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QVERIFY(listener.start(0));

    Connection client_(client.identity, client.store.get(), Connection::Mode::Paired);
    client_.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QSignalSpy clientFailed(&client_, &Connection::failed);
    QSignalSpy clientEstablished(&client_, &Connection::established);

    client_.connectToPeer(QHostAddress::LocalHost, listener.port());

    QTRY_VERIFY_WITH_TIMEOUT(clientFailed.count() > 0, kWaitMs);
    QCOMPARE(clientEstablished.count(), 0);
    QVERIFY(clientFailed.at(0).at(0).toString().contains(QStringLiteral("not paired")));
}

void TestTransport::pairingModeAdmitsUnknownKey() {
    // The one place an unknown key is allowed through — and only so the SAS
    // can be compared. Nothing is persisted by the transport itself.
    Peer server = makePeer(QStringLiteral("server"));
    Peer client = makePeer(QStringLiteral("client"));

    Server listener(server.identity, server.store.get());
    listener.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QVERIFY(listener.start(0));
    listener.setPairingWindowOpen(true);

    QSignalSpy accepted(&listener, &Server::connectionEstablished);

    Connection client_(client.identity, client.store.get(), Connection::Mode::Pairing);
    client_.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QSignalSpy clientEstablished(&client_, &Connection::established);
    client_.connectToPeer(QHostAddress::LocalHost, listener.port());

    QTRY_VERIFY_WITH_TIMEOUT(accepted.count() == 1, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(clientEstablished.count() == 1, kWaitMs);

    // Admitted, but still not trusted: the store must be untouched.
    QCOMPARE(server.store->count(), 0);
    QVERIFY(!server.store->isTrusted(client.identity.publicKey()));
}

void TestTransport::acceptsPairedPeer() {
    Peer server = makePeer(QStringLiteral("server"));
    Peer client = makePeer(QStringLiteral("client"));

    // Mutual pinning, exactly as completing a pairing flow would leave it.
    pin(server, client, QStringLiteral("client-id"));
    pin(client, server, QStringLiteral("server-id"));

    Server listener(server.identity, server.store.get());
    listener.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QVERIFY(listener.start(0));

    QSignalSpy accepted(&listener, &Server::connectionEstablished);
    QSignalSpy rejected(&listener, &Server::connectionRejected);

    Connection client_(client.identity, client.store.get(), Connection::Mode::Paired);
    client_.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QSignalSpy clientEstablished(&client_, &Connection::established);

    client_.connectToPeer(QHostAddress::LocalHost, listener.port());

    QTRY_VERIFY_WITH_TIMEOUT(accepted.count() == 1, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(clientEstablished.count() == 1, kWaitMs);
    QCOMPARE(rejected.count(), 0);

    // Each side must have observed the other's actual key.
    QCOMPARE(client_.peerPublicKey(), server.identity.publicKey());
    QCOMPARE(client_.peerFingerprint(), server.identity.fingerprint());
}

void TestTransport::negotiatesTls13() {
    Peer server = makePeer(QStringLiteral("server"));
    Peer client = makePeer(QStringLiteral("client"));
    pin(server, client, QStringLiteral("client-id"));
    pin(client, server, QStringLiteral("server-id"));

    Server listener(server.identity, server.store.get());
    listener.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QVERIFY(listener.start(0));

    Connection client_(client.identity, client.store.get(), Connection::Mode::Paired);
    client_.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QSignalSpy established(&client_, &Connection::established);
    client_.connectToPeer(QHostAddress::LocalHost, listener.port());

    // Connection::verifyPeerIdentity() fails the link if the negotiated
    // protocol is anything other than TLS 1.3, so reaching Established at
    // all is the assertion here.
    QTRY_VERIFY_WITH_TIMEOUT(established.count() == 1, kWaitMs);
    QCOMPARE(client_.state(), Connection::State::Established);
}

void TestTransport::exchangesMessages() {
    Peer server = makePeer(QStringLiteral("server"));
    Peer client = makePeer(QStringLiteral("client"));
    pin(server, client, QStringLiteral("client-id"));
    pin(client, server, QStringLiteral("server-id"));

    Server listener(server.identity, server.store.get());
    listener.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QVERIFY(listener.start(0));

    Connection *serverSide = nullptr;
    connect(&listener, &Server::connectionEstablished, this,
            [&serverSide](Connection *c) { serverSide = c; });

    Connection client_(client.identity, client.store.get(), Connection::Mode::Paired);
    client_.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QSignalSpy clientEstablished(&client_, &Connection::established);
    client_.connectToPeer(QHostAddress::LocalHost, listener.port());

    QTRY_VERIFY_WITH_TIMEOUT(clientEstablished.count() == 1, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(serverSide != nullptr, kWaitMs);

    QSignalSpy serverReceived(serverSide, &Connection::messageReceived);

    Message ping = Message::unpair(client_.nextCounter());
    QVERIFY(client_.send(ping));

    QTRY_VERIFY_WITH_TIMEOUT(serverReceived.count() == 1, kWaitMs);
    const auto received = qvariant_cast<Message>(serverReceived.at(0).at(0));
    QCOMPARE(received.type(), MessageType::Unpair);
    QCOMPARE(received.counter(), quint64(1));

    // And a data frame over the binary sub-channel.
    QSignalSpy serverData(serverSide, &Connection::dataReceived);
    const QByteArray chunk(4096, 'z');
    QVERIFY(client_.sendData(7, chunk));

    QTRY_VERIFY_WITH_TIMEOUT(serverData.count() == 1, kWaitMs);
    QCOMPARE(serverData.at(0).at(0).toUInt(), 7u);
    QCOMPARE(serverData.at(0).at(1).toByteArray(), chunk);
}

void TestTransport::rejectsReplayedCounter() {
    Peer server = makePeer(QStringLiteral("server"));
    Peer client = makePeer(QStringLiteral("client"));
    pin(server, client, QStringLiteral("client-id"));
    pin(client, server, QStringLiteral("server-id"));

    Server listener(server.identity, server.store.get());
    listener.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QVERIFY(listener.start(0));

    Connection *serverSide = nullptr;
    connect(&listener, &Server::connectionEstablished, this,
            [&serverSide](Connection *c) { serverSide = c; });

    Connection client_(client.identity, client.store.get(), Connection::Mode::Paired);
    client_.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QSignalSpy clientEstablished(&client_, &Connection::established);
    client_.connectToPeer(QHostAddress::LocalHost, listener.port());

    QTRY_VERIFY_WITH_TIMEOUT(clientEstablished.count() == 1, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(serverSide != nullptr, kWaitMs);

    QSignalSpy serverFailed(serverSide, &Connection::failed);
    QSignalSpy serverReceived(serverSide, &Connection::messageReceived);

    // Send counter 1 twice: the second is a replay and must kill the link
    // rather than be delivered to any handler.
    Message first = Message::unpair(1);
    QVERIFY(client_.send(first));
    QTRY_VERIFY_WITH_TIMEOUT(serverReceived.count() == 1, kWaitMs);

    Message replayed = Message::unpair(1);
    QVERIFY(client_.send(replayed));

    QTRY_VERIFY_WITH_TIMEOUT(serverFailed.count() == 1, kWaitMs);
    QCOMPARE(serverReceived.count(), 1); // never delivered a second time
}

void TestTransport::heartbeatPongsKeepAnIdleLinkAlive() {
    // Nothing application-level is ever sent by the test itself: everything
    // that happens here is the Ping/Pong traffic Connection generates and
    // answers on its own. Establishing that a healthy, silent link survives
    // several heartbeat cycles is as important as establishing that a dead
    // one does not.
    Peer server = makePeer(QStringLiteral("server"));
    Peer client = makePeer(QStringLiteral("client"));
    pin(server, client, QStringLiteral("client-id"));
    pin(client, server, QStringLiteral("server-id"));

    Server listener(server.identity, server.store.get());
    listener.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QVERIFY(listener.start(0));

    Connection *serverSide = nullptr;
    connect(&listener, &Server::connectionEstablished, this,
            [&serverSide](Connection *c) { serverSide = c; });

    Connection client_(client.identity, client.store.get(), Connection::Mode::Paired);
    client_.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QSignalSpy clientEstablished(&client_, &Connection::established);
    QSignalSpy clientFailed(&client_, &Connection::failed);
    client_.connectToPeer(QHostAddress::LocalHost, listener.port());

    QTRY_VERIFY_WITH_TIMEOUT(clientEstablished.count() == 1, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(serverSide != nullptr, kWaitMs);

    client_.setHeartbeatIntervals(kTestHeartbeatIntervalMs, kTestHeartbeatTimeoutMs);
    serverSide->setHeartbeatIntervals(kTestHeartbeatIntervalMs, kTestHeartbeatTimeoutMs);

    QSignalSpy serverReceived(serverSide, &Connection::messageReceived);
    QSignalSpy clientReceived(&client_, &Connection::messageReceived);

    // Well past what would have been the timeout with no traffic at all —
    // several ping/pong round trips must have happened by now.
    QTest::qWait(kTestHeartbeatTimeoutMs * 3);

    QCOMPARE(clientFailed.count(), 0);
    QCOMPARE(client_.state(), Connection::State::Established);
    QCOMPARE(serverSide->state(), Connection::State::Established);
    // Neither side's application layer ever sees Ping/Pong.
    QCOMPARE(serverReceived.count(), 0);
    QCOMPARE(clientReceived.count(), 0);
}

void TestTransport::heartbeatDropsAnUnresponsivePeer() {
    Peer client = makePeer(QStringLiteral("client"));
    Peer rawServer = makePeer(QStringLiteral("unresponsive-server"));
    pin(client, rawServer, QStringLiteral("server-id"));

    UnresponsivePeer peer(rawServer.identity);
    QVERIFY(peer.listen());

    Connection client_(client.identity, client.store.get(), Connection::Mode::Paired);
    client_.setHandshakeTimeout(kTestHandshakeTimeoutMs);
    QSignalSpy clientEstablished(&client_, &Connection::established);
    QSignalSpy clientFailed(&client_, &Connection::failed);
    client_.connectToPeer(QHostAddress::LocalHost, peer.serverPort());

    QTRY_VERIFY_WITH_TIMEOUT(clientEstablished.count() == 1, kWaitMs);
    client_.setHeartbeatIntervals(kTestHeartbeatIntervalMs, kTestHeartbeatTimeoutMs);

    // The peer never reads, so it never Pongs: this proves the timeout path
    // itself, not merely that a clean disconnect is noticed (already covered
    // elsewhere, and unrelated to this change).
    QTRY_VERIFY_WITH_TIMEOUT(clientFailed.count() == 1, kWaitMs);
    QVERIFY(clientFailed.at(0).at(0).toString().contains(QStringLiteral("heartbeat")));
    QCOMPARE(client_.state(), Connection::State::Disconnected);
}

void TestTransport::bindsTheDefaultPortAndFallsBack() {
    // The shipped firewalld rule names a specific port, so "which port does
    // the listener use" stopped being an implementation detail the moment
    // that rule existed. If this drifts, the rule opens the wrong hole and
    // every link fails on a firewalled machine with no visible reason.
    Peer first = makePeer(QStringLiteral("first"));
    Server a(first.identity, first.store.get());
    QVERIFY(a.start(Server::kDefaultPort));

    // Not asserted as equality: a developer machine may already be running
    // the real app on that port, and the fallback below is the behaviour
    // that covers exactly that case.
    const bool gotPreferred = a.port() == Server::kDefaultPort;

    Peer second = makePeer(QStringLiteral("second"));
    Server b(second.identity, second.store.get());
    QVERIFY2(b.start(Server::kDefaultPort),
             "a second instance must still come up on an ephemeral port");

    QVERIFY(a.port() != 0);
    QVERIFY(b.port() != 0);
    QVERIFY2(a.port() != b.port(), "two listeners ended up on one port");
    if (gotPreferred) {
        QCOMPARE(a.port(), Server::kDefaultPort);
        QVERIFY2(b.port() != Server::kDefaultPort,
                 "the second listener did not fall back off the taken port");
    }
}

QTEST_MAIN(TestTransport)
#include "tst_transport.moc"
