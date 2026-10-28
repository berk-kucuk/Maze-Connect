#include <QtTest>

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QFile>
#include <QTemporaryDir>

#include "mazeconnect/core/KdeConnectBridge.h"

using namespace mazeconnect::core;

// ---- a fake kdeconnectd, registered on the test's own session bus ---------

class FakeKde : public QObject {
    Q_OBJECT

public:
    QStringList calls;
    bool paired = true;
    bool pairRequestedByPeer = false;
    QStringList plugins{QStringLiteral("kdeconnect_ping"), QStringLiteral("kdeconnect_battery"),
                        QStringLiteral("kdeconnect_share"),
                        QStringLiteral("kdeconnect_findmyphone")};
};

class FakeDaemonAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.daemon")

public:
    explicit FakeDaemonAdaptor(QObject *owner, FakeKde *kde)
        : QDBusAbstractAdaptor(owner), m_kde(kde) {}

public Q_SLOTS:
    QStringList devices(bool onlyReachable, bool onlyPaired) {
        m_kde->calls << QStringLiteral("devices(%1,%2)").arg(onlyReachable).arg(onlyPaired);
        // One real id, and one that must never become an object path.
        return {QStringLiteral("_phone_1"), QStringLiteral("../../evil")};
    }

Q_SIGNALS:
    void deviceListChanged();

private:
    FakeKde *m_kde;
};

class FakeDeviceAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.device")
    Q_PROPERTY(QString name READ name)
    Q_PROPERTY(QString type READ type)
    Q_PROPERTY(bool isReachable READ yes)
    Q_PROPERTY(bool isPaired READ isPaired)
    Q_PROPERTY(bool isPairRequested READ no)
    Q_PROPERTY(bool isPairRequestedByPeer READ isPairRequestedByPeer)
    Q_PROPERTY(QString verificationKey READ verificationKey)

public:
    explicit FakeDeviceAdaptor(QObject *owner, FakeKde *kde)
        : QDBusAbstractAdaptor(owner), m_kde(kde) {}

    QString name() const { return QStringLiteral("Pixel\n9"); }
    QString type() const { return QStringLiteral("smartphone"); }
    bool yes() const { return true; }
    bool no() const { return false; }
    bool isPaired() const { return m_kde->paired; }
    bool isPairRequestedByPeer() const { return m_kde->pairRequestedByPeer; }
    QString verificationKey() const { return QStringLiteral("419792A9"); }

public Q_SLOTS:
    QStringList loadedPlugins() { return m_kde->plugins; }
    void requestPairing() { m_kde->calls << QStringLiteral("requestPairing"); }
    void acceptPairing() { m_kde->calls << QStringLiteral("acceptPairing"); }
    void cancelPairing() { m_kde->calls << QStringLiteral("cancelPairing"); }
    void unpair() { m_kde->calls << QStringLiteral("unpair"); }

Q_SIGNALS:
    void pairStateChanged(int pairState);

private:
    FakeKde *m_kde;
};

class FakePingAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.device.ping")

public:
    explicit FakePingAdaptor(QObject *owner, FakeKde *kde)
        : QDBusAbstractAdaptor(owner), m_kde(kde) {}

public Q_SLOTS:
    void sendPing() { m_kde->calls << QStringLiteral("sendPing"); }

private:
    FakeKde *m_kde;
};

class FakeBatteryAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.device.battery")
    Q_PROPERTY(int charge READ charge)
    Q_PROPERTY(bool isCharging READ isCharging)

public:
    explicit FakeBatteryAdaptor(QObject *owner) : QDBusAbstractAdaptor(owner) {}
    int charge() const { return 57; }
    bool isCharging() const { return true; }
};

class FakeShareAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.device.share")

public:
    explicit FakeShareAdaptor(QObject *owner, FakeKde *kde)
        : QDBusAbstractAdaptor(owner), m_kde(kde) {}

public Q_SLOTS:
    void shareUrl(const QString &url) { m_kde->calls << QStringLiteral("shareUrl ") + url; }
    void shareText(const QString &text) { m_kde->calls << QStringLiteral("shareText ") + text; }

private:
    FakeKde *m_kde;
};

class FakeAppAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.qtproject.Qt.QCoreApplication")

public:
    explicit FakeAppAdaptor(QObject *owner, FakeKde *kde)
        : QDBusAbstractAdaptor(owner), m_kde(kde) {}

public Q_SLOTS:
    void quit() { m_kde->calls << QStringLiteral("quit"); }

private:
    FakeKde *m_kde;
};

// ---- tests -----------------------------------------------------------------

class TestKdeConnectBridge : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void deviceIdsAreRestricted();
    void constructionTouchesNothing();
    void offWritesBothOverridesAndOnRemovesThem();
    void onLeavesAnUnhiddenUserEntryAlone();
    void aHiddenEntryFromElsewhereCountsAsOff();
    void onIsRefusedWhenNotInstalled();
    void offIsANoOpWhenNotInstalled();
    void nothingIsCalledWhileNotRunning();

    void cleanup();
    void devicesAreRead();
    void anInvalidIdIsNeverUsed();
    void actionsReachTheDaemon();
    void shareFileSendsAFileUrl();
    void unpairedDevicesOnlyOfferPairing();
    void anIncomingRequestCanBeAnswered();
    void switchingOffQuitsTheDaemon();
    void theDaemonGoingAwayClearsTheList();

private:
    bool haveBus() const { return !qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS"); }
    void startFake();
    void stopFake();
    bool waitForDevice(KdeConnectBridge &bridge, int battery = -2);

    QTemporaryDir m_home;
    QString m_daemon; ///< an executable file that stands in for kdeconnectd
    FakeKde *m_kde = nullptr;
    QList<QObject *> m_objects;
    int m_round = 0;
    QString m_connName;
};

void TestKdeConnectBridge::initTestCase() {
    QVERIFY(m_home.isValid());
    // Overrides go to a private config and data home, never the real ones.
    qputenv("XDG_CONFIG_HOME", (m_home.path() + QStringLiteral("/config")).toUtf8());
    qputenv("XDG_DATA_HOME", (m_home.path() + QStringLiteral("/data")).toUtf8());
    m_daemon = m_home.path() + QStringLiteral("/kdeconnectd");
    QFile daemon(m_daemon);
    QVERIFY(daemon.open(QIODevice::WriteOnly));
    daemon.write("#!/bin/sh\nexit 0\n");
    daemon.close();
    QVERIFY(daemon.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
}

void TestKdeConnectBridge::deviceIdsAreRestricted() {
    QVERIFY(KdeConnectBridge::isValidDeviceId(QStringLiteral("_d311d429_c06d_4307")));
    QVERIFY(KdeConnectBridge::isValidDeviceId(QStringLiteral("a1b2c3d4e5f60718")));
    QVERIFY(!KdeConnectBridge::isValidDeviceId(QString()));
    QVERIFY(!KdeConnectBridge::isValidDeviceId(QStringLiteral("../x")));
    QVERIFY(!KdeConnectBridge::isValidDeviceId(QStringLiteral("a/b")));
    QVERIFY(!KdeConnectBridge::isValidDeviceId(QStringLiteral("a-b")));
    QVERIFY(!KdeConnectBridge::isValidDeviceId(QStringLiteral("a b")));
    QVERIFY(!KdeConnectBridge::isValidDeviceId(QString(129, QLatin1Char('a'))));
}

void TestKdeConnectBridge::constructionTouchesNothing() {
    KdeConnectBridge bridge;
    bridge.setDaemonPath(m_daemon);
    QVERIFY(!QFileInfo::exists(KdeConnectBridge::autostartOverridePath()));
    QVERIFY(!QFileInfo::exists(KdeConnectBridge::activationOverridePath()));
    QVERIFY(!bridge.isRunning());
    QVERIFY(bridge.isEnabled());
}

void TestKdeConnectBridge::offWritesBothOverridesAndOnRemovesThem() {
    KdeConnectBridge bridge;
    bridge.setDaemonPath(m_daemon);
    QString error;

    QVERIFY2(bridge.setEnabled(false, error), qPrintable(error));
    QVERIFY(bridge.isBlocked());
    QVERIFY(!bridge.isEnabled());

    QFile autostart(KdeConnectBridge::autostartOverridePath());
    QVERIFY(autostart.open(QIODevice::ReadOnly));
    const QByteArray a = autostart.readAll();
    QVERIFY(a.contains("Hidden=true"));
    QVERIFY(a.contains(KdeConnectBridge::overrideMarker().toUtf8()));
    QVERIFY(a.startsWith("[Desktop Entry]"));

    QFile activation(KdeConnectBridge::activationOverridePath());
    QVERIFY(activation.open(QIODevice::ReadOnly));
    const QByteArray b = activation.readAll();
    QVERIFY(b.contains("Name=org.kde.kdeconnect\n"));
    QVERIFY(b.contains("Exec=/bin/false\n"));

    QVERIFY2(bridge.setEnabled(true, error), qPrintable(error));
    QVERIFY(!bridge.isBlocked());
    QVERIFY(bridge.isEnabled());
    QVERIFY(!QFileInfo::exists(KdeConnectBridge::autostartOverridePath()));
    QVERIFY(!QFileInfo::exists(KdeConnectBridge::activationOverridePath()));
}

void TestKdeConnectBridge::onLeavesAnUnhiddenUserEntryAlone() {
    // Someone's own, still-enabled tweak of the autostart entry is theirs.
    const QString path = KdeConnectBridge::autostartOverridePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("[Desktop Entry]\nType=Application\nExec=/usr/bin/kdeconnectd --replace\n");
    file.close();

    KdeConnectBridge bridge;
    bridge.setDaemonPath(m_daemon);
    QVERIFY(!bridge.isBlocked());
    QString error;
    QVERIFY(bridge.setEnabled(true, error));
    QVERIFY(QFileInfo::exists(path));
    QFile::remove(path);
}

void TestKdeConnectBridge::aHiddenEntryFromElsewhereCountsAsOff() {
    // Plasma's Autostart page hides the entry the same way; the switch must
    // say "off" then, and turning it on must actually turn it on.
    const QString path = KdeConnectBridge::autostartOverridePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("[Desktop Entry]\nType=Application\nHidden=True\n");
    file.close();

    KdeConnectBridge bridge;
    bridge.setDaemonPath(m_daemon);
    QVERIFY(bridge.isBlocked());
    QString error;
    QVERIFY(bridge.setEnabled(true, error));
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(bridge.isEnabled());
}

void TestKdeConnectBridge::onIsRefusedWhenNotInstalled() {
    KdeConnectBridge bridge;
    bridge.setDaemonPath(m_home.path() + QStringLiteral("/missing"));
    QVERIFY(!bridge.isInstalled());
    QVERIFY(!bridge.isEnabled());
    QString error;
    QVERIFY(!bridge.setEnabled(true, error));
    QVERIFY(!error.isEmpty());
}

void TestKdeConnectBridge::offIsANoOpWhenNotInstalled() {
    KdeConnectBridge bridge;
    bridge.setDaemonPath(m_home.path() + QStringLiteral("/missing"));
    QString error;
    QVERIFY(bridge.setEnabled(false, error));
    // No files left behind for a program that is not there.
    QVERIFY(!QFileInfo::exists(KdeConnectBridge::autostartOverridePath()));
    QVERIFY(!QFileInfo::exists(KdeConnectBridge::activationOverridePath()));
}

void TestKdeConnectBridge::nothingIsCalledWhileNotRunning() {
    KdeConnectBridge bridge;
    bridge.setDaemonPath(m_daemon);
    QString error;
    QVERIFY(!bridge.ping(QStringLiteral("_phone_1"), error));
    QVERIFY(!bridge.requestPairing(QStringLiteral("_phone_1"), error));
    QVERIFY(bridge.devices().isEmpty());
}

// ---- against the fake daemon ---------------------------------------------

void TestKdeConnectBridge::cleanup() {
    stopFake();
    QFile::remove(KdeConnectBridge::autostartOverridePath());
    QFile::remove(KdeConnectBridge::activationOverridePath());
}

void TestKdeConnectBridge::startFake() {
    m_kde = new FakeKde;
    // A connection of its own, as the real daemon is a separate process.
    m_connName = QStringLiteral("fake-kde-%1").arg(++m_round);
    QDBusConnection conn =
        QDBusConnection::connectToBus(QDBusConnection::SessionBus, m_connName);
    QVERIFY(conn.isConnected());

    auto add = [this] {
        auto *obj = new QObject;
        m_objects << obj;
        return obj;
    };
    QObject *daemon = add();
    new FakeDaemonAdaptor(daemon, m_kde);
    QObject *device = add();
    new FakeDeviceAdaptor(device, m_kde);
    QObject *ping = add();
    new FakePingAdaptor(ping, m_kde);
    QObject *battery = add();
    new FakeBatteryAdaptor(battery);
    QObject *share = add();
    new FakeShareAdaptor(share, m_kde);
    QObject *app = add();
    new FakeAppAdaptor(app, m_kde);

    const auto opts = QDBusConnection::ExportAdaptors;
    QVERIFY(conn.registerObject(QStringLiteral("/modules/kdeconnect"), daemon, opts));
    QVERIFY(conn.registerObject(QStringLiteral("/modules/kdeconnect/devices/_phone_1"), device,
                                opts));
    QVERIFY(conn.registerObject(QStringLiteral("/modules/kdeconnect/devices/_phone_1/ping"), ping,
                                opts));
    QVERIFY(conn.registerObject(QStringLiteral("/modules/kdeconnect/devices/_phone_1/battery"),
                                battery, opts));
    QVERIFY(conn.registerObject(QStringLiteral("/modules/kdeconnect/devices/_phone_1/share"),
                                share, opts));
    QVERIFY(conn.registerObject(QStringLiteral("/MainApplication"), app, opts));
    QVERIFY(conn.registerService(QStringLiteral("org.kde.kdeconnect")));
}

void TestKdeConnectBridge::stopFake() {
    if (!m_connName.isEmpty()) {
        QDBusConnection conn(m_connName);
        conn.unregisterService(QStringLiteral("org.kde.kdeconnect"));
        QDBusConnection::disconnectFromBus(m_connName);
        m_connName.clear();
    }
    qDeleteAll(m_objects);
    m_objects.clear();
    delete m_kde;
    m_kde = nullptr;
}

bool TestKdeConnectBridge::waitForDevice(KdeConnectBridge &bridge, int battery) {
    return QTest::qWaitFor(
        [&] {
            const auto list = bridge.devices();
            if (list.size() != 1 || list.first().name.isEmpty()) {
                return false;
            }
            return battery == -2 || list.first().battery == battery;
        },
        5000);
}

void TestKdeConnectBridge::devicesAreRead() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    startFake();
    KdeConnectBridge bridge(QDBusConnection::sessionBus());
    bridge.setDaemonPath(m_daemon);
    QVERIFY(bridge.ensureStarted());
    QTRY_VERIFY_WITH_TIMEOUT(bridge.isRunning(), 3000);
    QVERIFY(waitForDevice(bridge, 57));

    const KdeConnectDevice d = bridge.devices().first();
    QCOMPARE(d.id, QStringLiteral("_phone_1"));
    QCOMPARE(d.name, QStringLiteral("Pixel9")); // control characters dropped
    QCOMPARE(d.type, QStringLiteral("smartphone"));
    QVERIFY(d.reachable);
    QVERIFY(d.paired);
    QCOMPARE(d.verificationKey, QStringLiteral("419792A9"));
    QVERIFY(d.charging);
    QVERIFY(d.canPing);
    QVERIFY(d.canRing);
    QVERIFY(d.canShare);
    QVERIFY(!d.canClipboard);
    QVERIFY(m_kde->calls.contains(QStringLiteral("devices(0,0)")));
}

void TestKdeConnectBridge::anInvalidIdIsNeverUsed() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    startFake();
    KdeConnectBridge bridge(QDBusConnection::sessionBus());
    bridge.setDaemonPath(m_daemon);
    QVERIFY(bridge.ensureStarted());
    QVERIFY(waitForDevice(bridge));
    // "../../evil" came back from the daemon and was dropped, not listed.
    QCOMPARE(bridge.devices().size(), 1);
    QString error;
    QVERIFY(!bridge.ping(QStringLiteral("../../evil"), error));
}

void TestKdeConnectBridge::actionsReachTheDaemon() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    startFake();
    KdeConnectBridge bridge(QDBusConnection::sessionBus());
    bridge.setDaemonPath(m_daemon);
    QVERIFY(bridge.ensureStarted());
    QVERIFY(waitForDevice(bridge, 57));

    QString error;
    QVERIFY2(bridge.ping(QStringLiteral("_phone_1"), error), qPrintable(error));
    QTRY_VERIFY(m_kde->calls.contains(QStringLiteral("sendPing")));

    QVERIFY2(bridge.shareText(QStringLiteral("_phone_1"), QStringLiteral("hello"), error),
             qPrintable(error));
    QTRY_VERIFY(m_kde->calls.contains(QStringLiteral("shareText hello")));

    // The plugin is not loaded for this device: refused here, never sent.
    QVERIFY(!bridge.sendClipboard(QStringLiteral("_phone_1"), error));
    QVERIFY(!error.isEmpty());

    QVERIFY(!bridge.shareText(QStringLiteral("_phone_1"), QString(), error));
    QVERIFY(!bridge.shareText(QStringLiteral("_phone_1"),
                              QString(KdeConnectBridge::kMaxTextChars + 1, QLatin1Char('x')),
                              error));
    QVERIFY(!bridge.ping(QStringLiteral("_nobody"), error));
}

void TestKdeConnectBridge::shareFileSendsAFileUrl() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    startFake();
    KdeConnectBridge bridge(QDBusConnection::sessionBus());
    bridge.setDaemonPath(m_daemon);
    QVERIFY(bridge.ensureStarted());
    QVERIFY(waitForDevice(bridge, 57));

    const QString path = m_home.path() + QStringLiteral("/a note.txt");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
    file.close();

    QString error;
    QVERIFY2(bridge.shareFile(QStringLiteral("_phone_1"), path, error), qPrintable(error));
    const QString expected =
        QStringLiteral("shareUrl ") + QUrl::fromLocalFile(path).toString();
    QTRY_VERIFY(m_kde->calls.contains(expected));

    QVERIFY(!bridge.shareFile(QStringLiteral("_phone_1"), m_home.path(), error)); // a directory
    QVERIFY(!bridge.shareFile(QStringLiteral("_phone_1"), path + QStringLiteral(".gone"), error));
}

void TestKdeConnectBridge::unpairedDevicesOnlyOfferPairing() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    startFake();
    m_kde->paired = false;
    KdeConnectBridge bridge(QDBusConnection::sessionBus());
    bridge.setDaemonPath(m_daemon);
    QVERIFY(bridge.ensureStarted());
    QVERIFY(waitForDevice(bridge));

    const KdeConnectDevice d = bridge.devices().first();
    QVERIFY(!d.paired);
    QVERIFY(!d.canPing);
    QCOMPARE(d.battery, -1);

    QString error;
    QVERIFY(!bridge.ping(QStringLiteral("_phone_1"), error));
    QVERIFY(!bridge.unpair(QStringLiteral("_phone_1"), error));
    QVERIFY2(bridge.requestPairing(QStringLiteral("_phone_1"), error), qPrintable(error));
    QTRY_VERIFY(m_kde->calls.contains(QStringLiteral("requestPairing")));
}

void TestKdeConnectBridge::anIncomingRequestCanBeAnswered() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    startFake();
    m_kde->paired = false;
    KdeConnectBridge bridge(QDBusConnection::sessionBus());
    bridge.setDaemonPath(m_daemon);
    QVERIFY(bridge.ensureStarted());
    QVERIFY(waitForDevice(bridge));

    QString error;
    QVERIFY(!bridge.acceptPairing(QStringLiteral("_phone_1"), error)); // nobody asked

    // The phone asks: KDE Connect says so with a signal, and the bridge reads
    // the device again by itself.
    m_kde->pairRequestedByPeer = true;
    QDBusMessage signal = QDBusMessage::createSignal(
        QStringLiteral("/modules/kdeconnect/devices/_phone_1"),
        QStringLiteral("org.kde.kdeconnect.device"), QStringLiteral("pairStateChanged"));
    signal << 2;
    QVERIFY(QDBusConnection(m_connName).send(signal));
    QTRY_VERIFY_WITH_TIMEOUT(bridge.devices().first().pairRequestedByPeer, 3000);

    QVERIFY2(bridge.acceptPairing(QStringLiteral("_phone_1"), error), qPrintable(error));
    QTRY_VERIFY(m_kde->calls.contains(QStringLiteral("acceptPairing")));
    QVERIFY2(bridge.rejectPairing(QStringLiteral("_phone_1"), error), qPrintable(error));
    QTRY_VERIFY(m_kde->calls.contains(QStringLiteral("cancelPairing")));
}

void TestKdeConnectBridge::switchingOffQuitsTheDaemon() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    startFake();
    KdeConnectBridge bridge(QDBusConnection::sessionBus());
    bridge.setDaemonPath(m_daemon);
    QVERIFY(bridge.ensureStarted());
    QTRY_VERIFY_WITH_TIMEOUT(bridge.isRunning(), 3000);

    QString error;
    QVERIFY2(bridge.setEnabled(false, error), qPrintable(error));
    QTRY_VERIFY(m_kde->calls.contains(QStringLiteral("quit")));
    QVERIFY(bridge.isBlocked());
}

void TestKdeConnectBridge::theDaemonGoingAwayClearsTheList() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    startFake();
    KdeConnectBridge bridge(QDBusConnection::sessionBus());
    bridge.setDaemonPath(m_daemon);
    QVERIFY(bridge.ensureStarted());
    QVERIFY(waitForDevice(bridge));

    stopFake();
    QTRY_VERIFY_WITH_TIMEOUT(!bridge.isRunning(), 3000);
    QVERIFY(bridge.devices().isEmpty());
    QString error;
    QVERIFY(!bridge.ping(QStringLiteral("_phone_1"), error));
}

QTEST_GUILESS_MAIN(TestKdeConnectBridge)
#include "tst_kdeconnectbridge.moc"
