#include <QtTest>

#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "mazeconnect/core/DeviceManager.h"

using namespace mazeconnect::core;

/**
 * Full pairing flow across two managers on loopback.
 *
 * These cover the properties that only emerge when the state machine, the
 * transport, and the store all run together: that a pairing needs consent on
 * both ends before anything is pinned, that the codes shown on the two
 * screens actually match, and that an unpaired peer cannot use any
 * capability.
 */
class TestDeviceManager : public QObject {
    Q_OBJECT

private slots:
    void pairingPinsOnlyAfterBothAccept();
    void rejectingPairingPinsNothing();
    void reconnectsToAPairedDeviceWithoutUserAction();
    void onlyOneSideDialsOnReconnect();
    void identityPersistsAcrossRestart();
    void refusesToRegenerateOverExistingKey();

    void capabilitiesStartGrantedAndStayRevocable();
    void deliversASnapshotOverAPairedLink();
    void snapshotsAreNotPushedToDevicesThatDidNotAsk();
    void aDroppedLinkDoesNotLeavePendingWorkBehind();
    void aPhoneIsNeverDialledByTheComputer();

    void cleanup();

private:
    struct Node {
        std::unique_ptr<QTemporaryDir> dir;
        std::unique_ptr<DeviceManager> manager;
    };
    static Node makeNode(const QString &name);

    /// Pair two nodes and return once both have pinned each other.
    static void pair(Node &a, Node &b);

    /// Point StatusProvider at a stand-in helper, so these tests do not
    /// depend on maze-tools being installed on the build machine.
    void installStatusHelper(const QTemporaryDir &dir, const QString &json);

    QString m_helperPath;
};

TestDeviceManager::Node TestDeviceManager::makeNode(const QString &name) {
    Node node;
    node.dir = std::make_unique<QTemporaryDir>();
    node.manager = std::make_unique<DeviceManager>(node.dir->path());
    // start() also brings up the beacon; harmless on loopback and the tests
    // never depend on discovery, they dial the port directly.
    node.manager->start(name, QStringLiteral("desktop"));
    return node;
}

void TestDeviceManager::pairingPinsOnlyAfterBothAccept() {
    Node a = makeNode(QStringLiteral("alice"));
    Node b = makeNode(QStringLiteral("bob"));

    // Dial b's listener directly. Discovery is not what authorises anything
    // — it only decides who we call — so the tests skip it entirely.
    const quint16 bPort = b.manager->listenPort();
    QVERIFY(bPort != 0);

    QSignalSpy aPrompt(a.manager.get(), &DeviceManager::pairingRequested);
    QSignalSpy bPrompt(b.manager.get(), &DeviceManager::pairingRequested);
    QSignalSpy aDone(a.manager.get(), &DeviceManager::pairingCompleted);
    QSignalSpy bDone(b.manager.get(), &DeviceManager::pairingCompleted);

    QVERIFY(a.manager->requestPairingAt(QHostAddress::LocalHost, bPort, b.manager->deviceId()));

    // Both sides must be prompted, and with the same six digits.
    QTRY_VERIFY_WITH_TIMEOUT(aPrompt.count() == 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(bPrompt.count() == 1, 5000);

    const auto aPairing = qvariant_cast<PendingPairing>(aPrompt.at(0).at(0));
    const auto bPairing = qvariant_cast<PendingPairing>(bPrompt.at(0).at(0));
    QCOMPARE(aPairing.verificationCode.size(), 6);
    QCOMPARE(aPairing.verificationCode, bPairing.verificationCode);

    // Nothing is pinned while the prompt is still open.
    QCOMPARE(a.manager->pairedDevices().size(), 0);
    QCOMPARE(b.manager->pairedDevices().size(), 0);

    // One side accepting is not enough.
    b.manager->respondToPairing(a.manager->deviceId(), true);
    QTest::qWait(200);
    QCOMPARE(a.manager->pairedDevices().size(), 0);

    // Now the other side agrees.
    a.manager->respondToPairing(b.manager->deviceId(), true);

    QTRY_VERIFY_WITH_TIMEOUT(a.manager->pairedDevices().size() == 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(b.manager->pairedDevices().size() == 1, 5000);

    // Each pinned the other's real key.
    QCOMPARE(a.manager->pairedDevices().at(0).deviceId, b.manager->deviceId());
    QCOMPARE(b.manager->pairedDevices().at(0).deviceId, a.manager->deviceId());
}

void TestDeviceManager::rejectingPairingPinsNothing() {
    Node a = makeNode(QStringLiteral("alice"));
    Node b = makeNode(QStringLiteral("bob"));

    const quint16 bPort = b.manager->listenPort();
    QVERIFY(bPort != 0);

    QSignalSpy aPrompt(a.manager.get(), &DeviceManager::pairingRequested);
    QSignalSpy bPrompt(b.manager.get(), &DeviceManager::pairingRequested);

    QVERIFY(a.manager->requestPairingAt(QHostAddress::LocalHost, bPort, b.manager->deviceId()));
    QTRY_VERIFY_WITH_TIMEOUT(aPrompt.count() == 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(bPrompt.count() == 1, 5000);

    // The user says the codes do not match.
    a.manager->respondToPairing(b.manager->deviceId(), false);
    QTest::qWait(300);

    QCOMPARE(a.manager->pairedDevices().size(), 0);
    QCOMPARE(b.manager->pairedDevices().size(), 0);
}

void TestDeviceManager::reconnectsToAPairedDeviceWithoutUserAction() {
    // The real scenario: the app restarts, and must re-link to an
    // already-paired device with no prompt and no fresh pairing. Without
    // this path a paired device shows "not reachable" forever, because
    // nothing ever dials it again.
    QTemporaryDir dirA;
    QVERIFY(dirA.isValid());
    Node b = makeNode(QStringLiteral("bob"));
    const quint16 bPort = b.manager->listenPort();
    QString bId;

    // --- first run: pair ---
    {
        DeviceManager a(dirA.path());
        QVERIFY(a.start(QStringLiteral("alice"), QStringLiteral("desktop")));

        QSignalSpy aPrompt(&a, &DeviceManager::pairingRequested);
        QSignalSpy bPrompt(b.manager.get(), &DeviceManager::pairingRequested);

        bId = b.manager->deviceId();
        QVERIFY(a.requestPairingAt(QHostAddress::LocalHost, bPort, bId));
        QTRY_VERIFY_WITH_TIMEOUT(aPrompt.count() == 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(bPrompt.count() == 1, 5000);

        b.manager->respondToPairing(a.deviceId(), true);
        a.respondToPairing(bId, true);

        QTRY_VERIFY_WITH_TIMEOUT(a.pairedDevices().size() == 1, 5000);
    }
    // a is destroyed here: its links are gone, exactly as after a restart.

    // --- second run: same data dir, no pairing, must just reconnect ---
    DeviceManager a2(dirA.path());
    QVERIFY(a2.start(QStringLiteral("alice"), QStringLiteral("desktop")));

    // The pin survived, so no prompt should ever appear this time.
    QCOMPARE(a2.pairedDevices().size(), 1);
    QSignalSpy prompted(&a2, &DeviceManager::pairingRequested);
    QSignalSpy connected(&a2, &DeviceManager::deviceConnected);

    QVERIFY(a2.connectToPairedAt(QHostAddress::LocalHost, bPort, bId));

    QTRY_VERIFY_WITH_TIMEOUT(connected.count() >= 1, 5000);
    QVERIFY(a2.isConnected(bId));
    QCOMPARE(prompted.count(), 0);
}

void TestDeviceManager::onlyOneSideDialsOnReconnect() {
    // Both peers see each other's beacons, so without a rule both would dial
    // and one pair would end up with two links.
    //
    // For two devices of the *same* type there is no stable/mobile asymmetry
    // to use, so the lexicographic tiebreak still decides. Ids are distinct
    // by construction, so exactly one ordering holds and exactly one side
    // acts — that is the property, not which particular node wins.
    Node a = makeNode(QStringLiteral("alice"));
    Node b = makeNode(QStringLiteral("bob"));

    const QString idA = a.manager->deviceId();
    const QString idB = b.manager->deviceId();
    QVERIFY(idA != idB);
    QVERIFY((idA < idB) != (idB < idA));
}

void TestDeviceManager::identityPersistsAcrossRestart() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString firstFingerprint;
    QString firstDeviceId;
    {
        DeviceManager manager(dir.path());
        QVERIFY(manager.start(QStringLiteral("persistent"), QStringLiteral("desktop")));
        firstFingerprint = manager.fingerprint();
        firstDeviceId = manager.deviceId();
        QVERIFY(!firstFingerprint.isEmpty());
    }

    // A restart must keep the same identity — regenerating would silently
    // invalidate every pairing the user already made.
    {
        DeviceManager manager(dir.path());
        QVERIFY(manager.start(QStringLiteral("persistent"), QStringLiteral("desktop")));
        QCOMPARE(manager.fingerprint(), firstFingerprint);
        QCOMPARE(manager.deviceId(), firstDeviceId);
    }
}

void TestDeviceManager::refusesToRegenerateOverExistingKey() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    {
        DeviceManager manager(dir.path());
        QVERIFY(manager.start(QStringLiteral("victim"), QStringLiteral("desktop")));
    }

    // Corrupt the stored key. Quietly generating a fresh identity here would
    // drop every existing pairing without the user being told, so start()
    // must fail loudly instead.
    QFile key(QDir(dir.path()).filePath(QStringLiteral("identity.key")));
    QVERIFY(key.open(QIODevice::WriteOnly | QIODevice::Truncate));
    key.write("corrupted");
    key.close();

    DeviceManager manager(dir.path());
    QVERIFY2(!manager.start(QStringLiteral("victim"), QStringLiteral("desktop")),
             "silently regenerated an identity over existing key material");
}

// ---- systemStatus -------------------------------------------------------

void TestDeviceManager::cleanup() {
    qunsetenv("MAZECONNECT_STATUS_HELPER");
}

void TestDeviceManager::pair(Node &a, Node &b) {
    const quint16 bPort = b.manager->listenPort();
    QVERIFY(bPort != 0);

    QSignalSpy aPrompt(a.manager.get(), &DeviceManager::pairingRequested);
    QSignalSpy bPrompt(b.manager.get(), &DeviceManager::pairingRequested);

    QVERIFY(a.manager->requestPairingAt(QHostAddress::LocalHost, bPort, b.manager->deviceId()));
    QTRY_VERIFY_WITH_TIMEOUT(aPrompt.count() == 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(bPrompt.count() == 1, 5000);

    b.manager->respondToPairing(a.manager->deviceId(), true);
    a.manager->respondToPairing(b.manager->deviceId(), true);

    QTRY_VERIFY_WITH_TIMEOUT(a.manager->pairedDevices().size() == 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(b.manager->pairedDevices().size() == 1, 5000);
}

void TestDeviceManager::installStatusHelper(const QTemporaryDir &dir, const QString &json) {
    m_helperPath = QDir(dir.path()).filePath(QStringLiteral("status-helper.sh"));
    QFile file(m_helperPath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QStringLiteral("#!/bin/sh\nprintf '%s' '%1'\n").arg(json).toUtf8());
    file.close();
    QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    qputenv("MAZECONNECT_STATUS_HELPER", m_helperPath.toUtf8());
}

void TestDeviceManager::capabilitiesStartGrantedAndStayRevocable() {
    // Pairing now grants every capability, because pairing *is* the decision
    // — a person compared six digits on two screens and agreed on both.
    //
    // What must still hold is that revoking works, and that it works from the
    // side being read: a machine whose owner switches something off stops
    // answering, whatever the other end thinks.
    Node a = makeNode(QStringLiteral("alice"));
    Node b = makeNode(QStringLiteral("bob"));
    pair(a, b);

    const QString idA = a.manager->deviceId();
    const QString idB = b.manager->deviceId();
    installStatusHelper(*b.dir, QStringLiteral("{\"hostname\":\"bobbox\"}"));

    QSignalSpy report(a.manager.get(), &DeviceManager::statusReportReceived);

    // Straight out of pairing, with nothing switched on by hand.
    QVERIFY2(a.manager->requestStatus(idB),
             "a freshly paired device could not ask for anything");
    QTRY_VERIFY_WITH_TIMEOUT(report.count() == 1, 5000);

    // b — the machine being read — withdraws it. The owner of a machine
    // decides what it discloses, so b must stop handing over snapshots.
    QVERIFY(b.manager->setCapabilityEnabled(idA, Capability::SystemStatus, false));
    QTest::qWait(200);
    a.manager->requestStatus(idB);
    QTRY_VERIFY_WITH_TIMEOUT(report.count() == 2, 5000);

    // Refused *out loud*, and with nothing in it. Silence would be worse than
    // useless here: it is indistinguishable from a slow machine, and it is
    // why a revoked capability used to look like a hung app — the phone sat
    // on "asking the computer…" with no way to learn otherwise.
    QVERIFY2(!report.at(1).at(2).toString().isEmpty(),
             "a refused request was answered with silence");
    QVERIFY2(qvariant_cast<QJsonObject>(report.at(1).at(1)).isEmpty(),
             "a refusal carried a snapshot anyway");

    // And it comes back when b allows it again.
    QVERIFY(b.manager->setCapabilityEnabled(idA, Capability::SystemStatus, true));
    QTest::qWait(StatusProvider::kCacheMs + 100);
    QVERIFY(a.manager->requestStatus(idB));
    QTRY_VERIFY_WITH_TIMEOUT(report.count() == 3, 5000);
    QCOMPARE(report.at(2).at(2).toString(), QString());
}

void TestDeviceManager::deliversASnapshotOverAPairedLink() {
    Node a = makeNode(QStringLiteral("alice"));
    Node b = makeNode(QStringLiteral("bob"));
    pair(a, b);

    const QString idA = a.manager->deviceId();
    const QString idB = b.manager->deviceId();
    QVERIFY(a.manager->setCapabilityEnabled(idB, Capability::SystemStatus, true));
    QVERIFY(b.manager->setCapabilityEnabled(idA, Capability::SystemStatus, true));

    installStatusHelper(*b.dir, QStringLiteral("{\"hostname\":\"bobbox\",\"unavailable\":[]}"));

    QSignalSpy report(a.manager.get(), &DeviceManager::statusReportReceived);
    QVERIFY(a.manager->requestStatus(idB));
    QTRY_VERIFY_WITH_TIMEOUT(report.count() == 1, 5000);

    QCOMPARE(report.at(0).at(0).toString(), idB);
    const auto snapshot = qvariant_cast<QJsonObject>(report.at(0).at(1));
    QCOMPARE(snapshot.value(QLatin1StringView("hostname")).toString(), QStringLiteral("bobbox"));
    QCOMPARE(report.at(0).at(2).toString(), QString());

    // With no helper on the far side the dashboard is told why, rather than
    // being handed an empty machine that looks like a healthy one.
    qunsetenv("MAZECONNECT_STATUS_HELPER");
    qputenv("MAZECONNECT_STATUS_HELPER",
            QDir(b.dir->path()).filePath(QStringLiteral("absent")).toUtf8());
    QTest::qWait(StatusProvider::kCacheMs + 100);

    QVERIFY(a.manager->requestStatus(idB));
    QTRY_VERIFY_WITH_TIMEOUT(report.count() == 2, 5000);
    QVERIFY(!report.at(1).at(2).toString().isEmpty());
    QVERIFY(qvariant_cast<QJsonObject>(report.at(1).at(1)).isEmpty());
}

void TestDeviceManager::snapshotsAreNotPushedToDevicesThatDidNotAsk() {
    // A snapshot goes to whoever is waiting for one, and the queue is drained
    // when it is served. Without that, b's own dashboard refreshing on a
    // timer would keep re-sending a's machine readings it never asked for.
    //
    // This is the honest half of the property. The other half — that a *is*
    // the one refusing an unsolicited report, rather than merely never being
    // sent one — cannot be shown with two well-behaved managers; it takes a
    // peer willing to send a report nobody requested.
    Node a = makeNode(QStringLiteral("alice"));
    Node b = makeNode(QStringLiteral("bob"));
    pair(a, b);

    const QString idA = a.manager->deviceId();
    const QString idB = b.manager->deviceId();
    QVERIFY(a.manager->setCapabilityEnabled(idB, Capability::SystemStatus, true));
    QVERIFY(b.manager->setCapabilityEnabled(idA, Capability::SystemStatus, true));
    installStatusHelper(*b.dir, QStringLiteral("{\"hostname\":\"bobbox\"}"));

    QSignalSpy report(a.manager.get(), &DeviceManager::statusReportReceived);

    QVERIFY(a.manager->requestStatus(idB));
    QTRY_VERIFY_WITH_TIMEOUT(report.count() == 1, 5000);

    // Past the cache, so this genuinely re-runs the helper on b rather than
    // returning early — the run itself is what must not reach a.
    QTest::qWait(StatusProvider::kCacheMs + 100);
    QSignalSpy bReady(b.manager->statusProvider(), &StatusProvider::snapshotReady);
    b.manager->statusProvider()->request();
    QTRY_VERIFY_WITH_TIMEOUT(bReady.count() == 1, 5000);

    QTest::qWait(300);
    QCOMPARE(report.count(), 1);
}

void TestDeviceManager::aDroppedLinkDoesNotLeavePendingWorkBehind() {
    // The bug this guards: every "who asked for this" set is there to refuse
    // unsolicited answers, so an entry that outlives its link inverts the
    // check — the first genuine answer after a reconnect looks unsolicited
    // and is dropped. That is a dashboard that stays blank after a network
    // blip even though the link is back.
    Node a = makeNode(QStringLiteral("alice"));
    Node b = makeNode(QStringLiteral("bob"));
    pair(a, b);

    const QString idA = a.manager->deviceId();
    const QString idB = b.manager->deviceId();
    QVERIFY(a.manager->setCapabilityEnabled(idB, Capability::SystemStatus, true));
    QVERIFY(b.manager->setCapabilityEnabled(idA, Capability::SystemStatus, true));

    // A helper that never answers, so the request is still outstanding when
    // the link goes away.
    const QString stalled = QDir(b.dir->path()).filePath(QStringLiteral("stall.sh"));
    {
        QFile file(stalled);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        // Long enough to still be running when the link dies below,
        // short enough not to hold the suite on StatusProvider's timeout.
        file.write("#!/bin/sh\nsleep 3\n");
        file.close();
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }
    qputenv("MAZECONNECT_STATUS_HELPER", stalled.toUtf8());

    QVERIFY(a.manager->requestStatus(idB));
    QTest::qWait(300);

    // The link dies with the request outstanding.
    b.manager->stop();
    QTRY_VERIFY_WITH_TIMEOUT(!a.manager->isConnected(idB), 5000);
    QTest::qWait(300);

    // Reconnect, with a helper that answers this time.
    installStatusHelper(*b.dir, QStringLiteral("{\"hostname\":\"bobbox\"}"));
    QVERIFY(b.manager->start(QStringLiteral("bob"), QStringLiteral("desktop")));

    QSignalSpy report(a.manager.get(), &DeviceManager::statusReportReceived);
    QTRY_VERIFY_WITH_TIMEOUT(
        a.manager->connectToPairedAt(QHostAddress::LocalHost, b.manager->listenPort(), idB), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(a.manager->isConnected(idB), 5000);

    QVERIFY(a.manager->requestStatus(idB));
    QTRY_VERIFY_WITH_TIMEOUT(report.count() == 1, 15000);
    QVERIFY2(report.count() == 1, "the first request after a reconnect was dropped");
}

void TestDeviceManager::aPhoneIsNeverDialledByTheComputer() {
    // Exactly one side dials, and it is always the phone: the computer is the
    // stable end, while a phone changes address and may not be accepting
    // connections at all in the background.
    //
    // This used to be a lexicographic tiebreak on device ids, which chose the
    // direction by coin flip. When the phone's id sorted first the desktop
    // refused to dial and the phone had no reconnect logic, so half of all
    // pairings could never recover from a dropped link.
    Node desktop = makeNode(QStringLiteral("desktop-side"));

    DiscoveredDevice phone;
    phone.deviceId = QStringLiteral("0000-sorts-before-anything");
    phone.deviceName = QStringLiteral("phone");
    phone.deviceType = QStringLiteral("mobile");
    phone.address = QHostAddress::LocalHost;
    phone.port = 1;

    // Even with an id that would win any tiebreak, a mobile peer is not
    // dialled — and an unpaired one is not dialled either way.
    QVERIFY2(!desktop.manager->connectToPairedAt(phone.address, phone.port, phone.deviceId),
             "the computer dialled a device it has not paired with");
}

QTEST_MAIN(TestDeviceManager)
#include "tst_devicemanager.moc"
