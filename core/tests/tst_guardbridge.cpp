#include <QtTest>

#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "mazeconnect/core/GuardBridge.h"

using namespace mazeconnect::core;

/**
 * GuardBridge against a stand-in maze-guardd.
 *
 * This is the privileged path, and the test that matters most is
 * `neverProducesPanicOrRestore`. The claim in GuardBridge's header — that
 * PANIC and RESTORE are *absent* rather than refused — is only worth anything
 * if it is checked against the bytes that actually reach the socket. A comment
 * saying "we never send PANIC" survives an edit that starts sending it; this
 * does not.
 *
 * The stand-in records every line it is given, so each test can assert on the
 * whole conversation rather than on one call's return value.
 */
class TestGuardBridge : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void reportsAnAbsentBroker();
    void parsesStatus();
    void unavailableIsNotOff();
    void blockingSendsOffAndAllowingSendsOn();
    void neverProducesPanicOrRestore();
    void refusesADeviceNotInTheTable();
    void aDeviceNameCannotSmuggleASecondVerb();
    void surfacesABrokerRefusal();
    void reportsTheStateAfterwardsNotTheStateAsked();

private:
    void startBroker();

    QTemporaryDir m_dir;
    QString m_socketPath;
    QLocalServer m_server;

    /// Every line the bridge has sent, in order.
    QStringList m_seen;
    /// Reply for a STATUS, and for a KILL.
    QByteArray m_statusReply = "OK camera=on microphone=off bluetooth=none wifi=on usb=on\n";
    QByteArray m_killReply = "OK camera off\n";
};

void TestGuardBridge::init() {
    QVERIFY(m_dir.isValid());
    m_socketPath = QDir(m_dir.path()).filePath(QStringLiteral("guard.sock"));
    m_seen.clear();
    m_statusReply = "OK camera=on microphone=off bluetooth=none wifi=on usb=on\n";
    m_killReply = "OK camera off\n";
    qputenv("MAZECONNECT_GUARD_SOCKET", m_socketPath.toUtf8());
}

void TestGuardBridge::cleanup() {
    qunsetenv("MAZECONNECT_GUARD_SOCKET");
    m_server.close();
    QLocalServer::removeServer(m_socketPath);
}

void TestGuardBridge::startBroker() {
    QLocalServer::removeServer(m_socketPath);
    QVERIFY(m_server.listen(m_socketPath));

    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        QLocalSocket *socket = m_server.nextPendingConnection();
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            const QString line = QString::fromUtf8(socket->readAll()).trimmed();
            if (line.isEmpty()) {
                return;
            }
            m_seen << line;
            socket->write(line.startsWith(QLatin1StringView("STATUS")) ? m_statusReply
                                                                       : m_killReply);
            socket->flush();
            socket->disconnectFromServer();
        });
    });
}

void TestGuardBridge::reportsAnAbsentBroker() {
    // maze-tools not installed, or maze-guardd not running. Saying so beats
    // showing switches that quietly do nothing.
    GuardBridge bridge;
    QVERIFY(!bridge.isAvailable());

    QSignalSpy failed(&bridge, &GuardBridge::statusFailed);
    bridge.requestStatus();
    QCOMPARE(failed.count(), 1);
    QVERIFY(failed.at(0).at(0).toString().contains(QStringLiteral("not available")));
}

void TestGuardBridge::parsesStatus() {
    startBroker();
    GuardBridge bridge;
    QVERIFY(bridge.isAvailable());

    QSignalSpy ready(&bridge, &GuardBridge::statusReady);
    bridge.requestStatus();
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1, 5000);

    const auto states = qvariant_cast<QMap<GuardDevice, GuardState>>(ready.at(0).at(0));
    QCOMPARE(states.value(GuardDevice::Camera), GuardState::On);
    QCOMPARE(states.value(GuardDevice::Microphone), GuardState::Off);
    QCOMPARE(states.value(GuardDevice::Wifi), GuardState::On);
    QCOMPARE(states.value(GuardDevice::Usb), GuardState::On);
    QCOMPARE(m_seen, QStringList{QStringLiteral("STATUS")});
}

void TestGuardBridge::unavailableIsNotOff() {
    // A machine with no Bluetooth is not a machine whose Bluetooth is
    // blocked. Folding maze-guardd's "none" into "off" would report a
    // protection that does not exist.
    startBroker();
    GuardBridge bridge;

    QSignalSpy ready(&bridge, &GuardBridge::statusReady);
    bridge.requestStatus();
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1, 5000);

    const auto states = qvariant_cast<QMap<GuardDevice, GuardState>>(ready.at(0).at(0));
    QCOMPARE(states.value(GuardDevice::Bluetooth), GuardState::Unavailable);
    QVERIFY(states.value(GuardDevice::Bluetooth) != GuardState::Off);
}

void TestGuardBridge::blockingSendsOffAndAllowingSendsOn() {
    // The direction, pinned against the wire.
    //
    // maze-guardd's `on`/`off` name the **device**, not the switch: `KILL
    // wifi off` runs `rfkill block`. Reading them as "protection on/off"
    // inverts every button, and it shipped that way — pressing Block sent
    // `KILL wifi on` and unblocked wifi, while the Activity log recorded
    // "asked for on — now on" and looked entirely consistent.
    startBroker();
    GuardBridge bridge;
    QSignalSpy applied(&bridge, &GuardBridge::killApplied);

    // Blocking a device disables it.
    bridge.setDeviceEnabled(GuardDevice::Wifi, false);
    QTRY_VERIFY_WITH_TIMEOUT(applied.count() == 1, 5000);
    QVERIFY2(m_seen.contains(QStringLiteral("KILL wifi off")),
             "blocking a device did not send the disabling action");
    QVERIFY2(!m_seen.contains(QStringLiteral("KILL wifi on")),
             "blocking a device sent the *enabling* action");

    m_seen.clear();

    // Allowing it again enables it.
    bridge.setDeviceEnabled(GuardDevice::Wifi, true);
    QTRY_VERIFY_WITH_TIMEOUT(applied.count() == 2, 5000);
    QVERIFY2(m_seen.contains(QStringLiteral("KILL wifi on")),
             "allowing a device did not send the enabling action");
    QVERIFY2(!m_seen.contains(QStringLiteral("KILL wifi off")),
             "allowing a device sent the *disabling* action");

    // And the flag reported back is what was asked for, in the same
    // vocabulary: true means the device should work.
    QCOMPARE(applied.at(0).at(1).toBool(), false);
    QCOMPARE(applied.at(1).at(1).toBool(), true);
}

void TestGuardBridge::neverProducesPanicOrRestore() {
    // The assertion this class exists to keep true.
    //
    // PANIC and RESTORE are destructive and should require being in front of
    // the machine. They are not gated here — there is no code path that can
    // produce either word — and this checks that against the wire rather than
    // trusting it to stay true as the file is edited.
    startBroker();
    GuardBridge bridge;
    QSignalSpy applied(&bridge, &GuardBridge::killApplied);

    // Exercise every device, both directions, plus a status.
    const QList<GuardDevice> all = {GuardDevice::Camera, GuardDevice::Microphone,
                                    GuardDevice::Bluetooth, GuardDevice::Wifi,
                                    GuardDevice::Usb};
    bridge.requestStatus();
    for (const GuardDevice device : all) {
        bridge.setDeviceEnabled(device, true);
        bridge.setDeviceEnabled(device, false);
    }
    QTRY_VERIFY_WITH_TIMEOUT(applied.count() == all.size() * 2, 15000);

    QVERIFY(!m_seen.isEmpty());
    for (const QString &line : std::as_const(m_seen)) {
        QVERIFY2(!line.contains(QStringLiteral("PANIC"), Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("PANIC reached the broker: %1").arg(line)));
        QVERIFY2(!line.contains(QStringLiteral("RESTORE"), Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("RESTORE reached the broker: %1").arg(line)));

        // And nothing outside the two permitted verbs, either.
        QVERIFY2(line == QStringLiteral("STATUS")
                     || line.startsWith(QStringLiteral("KILL ")),
                 qPrintable(QStringLiteral("unexpected verb: %1").arg(line)));
    }
}

void TestGuardBridge::refusesADeviceNotInTheTable() {
    GuardDevice device{};
    QVERIFY(guardDeviceFromName(QStringLiteral("camera"), device));
    QCOMPARE(device, GuardDevice::Camera);

    // Not guessed at, not prefix-matched, not case-folded.
    QVERIFY(!guardDeviceFromName(QStringLiteral("cam"), device));
    QVERIFY(!guardDeviceFromName(QStringLiteral("Camera"), device));
    QVERIFY(!guardDeviceFromName(QStringLiteral("camera "), device));
    QVERIFY(!guardDeviceFromName(QString(), device));
    QVERIFY(!guardDeviceFromName(QStringLiteral("everything"), device));
}

void TestGuardBridge::aDeviceNameCannotSmuggleASecondVerb() {
    // The attack this design forecloses: a device name carrying a newline
    // would make "KILL <name> on" two lines, the second of which could be
    // PANIC. It cannot happen, because the name never travels as text — it is
    // looked up and becomes an enum, or it is refused.
    GuardDevice device{};
    QVERIFY(!guardDeviceFromName(QStringLiteral("camera on\nPANIC"), device));
    QVERIFY(!guardDeviceFromName(QStringLiteral("camera\nRESTORE"), device));
    QVERIFY(!guardDeviceFromName(QStringLiteral("camera; PANIC"), device));

    // And the round trip through the table only ever yields the fixed names.
    const QList<GuardDevice> all = {GuardDevice::Camera, GuardDevice::Microphone,
                                    GuardDevice::Bluetooth, GuardDevice::Wifi,
                                    GuardDevice::Usb};
    for (const GuardDevice each : all) {
        const QString name = guardDeviceName(each);
        QVERIFY(!name.isEmpty());
        QVERIFY2(!name.contains(QLatin1Char('\n')) && !name.contains(QLatin1Char(' ')),
                 "a device name could split the command line");
    }
}

void TestGuardBridge::surfacesABrokerRefusal() {
    // maze-guardd says no — most often because the caller has no active local
    // session. That is its answer, and it should reach the user rather than
    // being reported as success.
    startBroker();
    m_killReply = "ERR unauthorized\n";

    GuardBridge bridge;
    QSignalSpy applied(&bridge, &GuardBridge::killApplied);
    bridge.setDeviceEnabled(GuardDevice::Wifi, false);

    QTRY_VERIFY_WITH_TIMEOUT(applied.count() == 1, 5000);
    QCOMPARE(applied.at(0).at(3).toString(), QStringLiteral("unauthorized"));
}

void TestGuardBridge::reportsTheStateAfterwardsNotTheStateAsked() {
    // A request the broker accepted is not proof the hardware complied, so
    // the state reported back is re-read rather than assumed.
    startBroker();
    m_statusReply = "OK camera=on microphone=off bluetooth=none wifi=on usb=on\n";

    GuardBridge bridge;
    QSignalSpy applied(&bridge, &GuardBridge::killApplied);
    // Ask to block the camera; the broker keeps reporting it as unblocked.
    bridge.setDeviceEnabled(GuardDevice::Camera, false);

    QTRY_VERIFY_WITH_TIMEOUT(applied.count() == 1, 5000);
    // What was asked: the camera should be disabled.
    QCOMPARE(applied.at(0).at(1).toBool(), false);
    // What is actually true afterwards: the broker still reports it working.
    // Reporting the request instead would tell the user a protection is in
    // place when it is not — the one lie this feature must never tell.
    QCOMPARE(qvariant_cast<GuardState>(applied.at(0).at(2)), GuardState::On);
}

QTEST_MAIN(TestGuardBridge)
#include "tst_guardbridge.moc"
