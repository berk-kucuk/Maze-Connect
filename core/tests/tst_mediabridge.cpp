#include <QtTest>

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QJsonArray>
#include <QTemporaryDir>

#include "mazeconnect/core/MediaBridge.h"

using namespace mazeconnect::core;

// ---- a fake MPRIS player, registered on the test's own session bus -------

class FakePlayer : public QObject {
    Q_OBJECT

public:
    QString status = QStringLiteral("Paused");
    QString title = QStringLiteral("Seven Nation Army");
    qlonglong position = 12'000'000;
    double volume = 0.5;
    bool canNext = true;
    QStringList calls;
    qlonglong lastSetPosition = -1;
    QString lastSetTrack;
};

class FakeRootAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(QString Identity READ identity)

public:
    explicit FakeRootAdaptor(FakePlayer *player) : QDBusAbstractAdaptor(player) {}
    QString identity() const { return QStringLiteral("Fake\nPlayer"); }
};

class FakePlayerAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(bool CanPlay READ yes)
    Q_PROPERTY(bool CanPause READ yes)
    Q_PROPERTY(bool CanGoNext READ canNext)
    Q_PROPERTY(bool CanGoPrevious READ no)
    Q_PROPERTY(bool CanSeek READ yes)
    Q_PROPERTY(bool CanControl READ yes)

public:
    explicit FakePlayerAdaptor(FakePlayer *player)
        : QDBusAbstractAdaptor(player), m_player(player) {}

    QString playbackStatus() const { return m_player->status; }
    QVariantMap metadata() const {
        QVariantMap map;
        map.insert(QStringLiteral("xesam:title"), m_player->title);
        map.insert(QStringLiteral("xesam:artist"),
                   QStringList{QStringLiteral("The White Stripes")});
        map.insert(QStringLiteral("xesam:album"), QStringLiteral("Elephant"));
        map.insert(QStringLiteral("mpris:length"), qlonglong(231'000'000));
        map.insert(QStringLiteral("mpris:trackid"),
                   QVariant::fromValue(QDBusObjectPath(QStringLiteral("/track/1"))));
        return map;
    }
    qlonglong position() const { return m_player->position; }
    double volume() const { return m_player->volume; }
    void setVolume(double v) {
        m_player->volume = v;
        m_player->calls << QStringLiteral("Volume");
    }
    bool yes() const { return true; }
    bool no() const { return false; }
    bool canNext() const { return m_player->canNext; }

public Q_SLOTS:
    void PlayPause() {
        m_player->calls << QStringLiteral("PlayPause");
        m_player->status = m_player->status == QLatin1StringView("Playing")
            ? QStringLiteral("Paused") : QStringLiteral("Playing");
    }
    void Next() { m_player->calls << QStringLiteral("Next"); }
    void SetPosition(const QDBusObjectPath &track, qlonglong position) {
        m_player->calls << QStringLiteral("SetPosition");
        m_player->lastSetTrack = track.path();
        m_player->lastSetPosition = position;
    }

private:
    FakePlayer *m_player;
};

// ---- tests -----------------------------------------------------------------

class TestMediaBridge : public QObject {
    Q_OBJECT

private slots:
    void playerIdsComeOnlyFromMprisNames();
    void textIsCleanedAndBounded();
    void wpctlOutputIsParsed();
    void constructionTouchesNoBus();

    void init();
    void cleanup();
    void aPlayerIsFoundAndRead();
    void commandsReachThePlayer();
    void seekIsAbsoluteAndTiedToTheTrack();
    void whatThePlayerCannotDoIsRefused();
    void anUnknownPlayerIsRefused();
    void aPlayerThatQuitsDisappears();
    void systemVolumeComesFromTheTool();
    void systemVolumeIsBounded();

private:
    bool haveBus() const { return !qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS"); }
    QJsonObject playerEntry(const MediaBridge &bridge) const;

    FakePlayer *m_player = nullptr;
    QString m_serviceName;
    int m_round = 0;
};

void TestMediaBridge::playerIdsComeOnlyFromMprisNames() {
    QCOMPARE(MediaBridge::playerIdFromBusName(QStringLiteral("org.mpris.MediaPlayer2.spotify")),
             QStringLiteral("spotify"));
    QCOMPARE(MediaBridge::playerIdFromBusName(
                 QStringLiteral("org.mpris.MediaPlayer2.firefox.instance_1_42")),
             QStringLiteral("firefox.instance_1_42"));
    QVERIFY(MediaBridge::playerIdFromBusName(QStringLiteral("org.kde.StatusNotifier")).isEmpty());
    QVERIFY(MediaBridge::playerIdFromBusName(QStringLiteral("org.mpris.MediaPlayer2.")).isEmpty());
    QVERIFY(MediaBridge::playerIdFromBusName(QStringLiteral("org.mpris.MediaPlayer2.a b"))
                .isEmpty());
    QVERIFY(MediaBridge::playerIdFromBusName(
                QStringLiteral("org.mpris.MediaPlayer2.") + QString(65, QLatin1Char('x')))
                .isEmpty());
}

void TestMediaBridge::textIsCleanedAndBounded() {
    QCOMPARE(MediaBridge::cleanText(QStringLiteral("a\nb\tc\x7f" "d")), QStringLiteral("a b c d"));
    QCOMPARE(MediaBridge::cleanText(QString(1000, QLatin1Char('x'))).size(),
             qsizetype(MediaBridge::kMaxTextChars));
}

void TestMediaBridge::wpctlOutputIsParsed() {
    int percent = -1;
    bool muted = true;
    QVERIFY(MediaBridge::parseWpctlVolume("Volume: 0.45\n", percent, muted));
    QCOMPARE(percent, 45);
    QCOMPARE(muted, false);
    QVERIFY(MediaBridge::parseWpctlVolume("Volume: 1.00 [MUTED]\n", percent, muted));
    QCOMPARE(percent, 100);
    QCOMPARE(muted, true);
    QVERIFY(MediaBridge::parseWpctlVolume("Volume: 9.99\n", percent, muted));
    QCOMPARE(percent, 150); // clamped
    QVERIFY(!MediaBridge::parseWpctlVolume("Error: no such node\n", percent, muted));
}

void TestMediaBridge::constructionTouchesNoBus() {
    // A bridge that is never asked for anything must not go looking for a
    // session bus: the daemon and the test suite construct one on machines
    // that have none.
    MediaBridge bridge;
    QVERIFY(!bridge.isAvailable());
    QVERIFY(bridge.snapshot().value(QLatin1StringView("players")).toArray().isEmpty());
}

void TestMediaBridge::init() {
    if (!haveBus()) {
        return;
    }
    m_player = new FakePlayer;
    new FakeRootAdaptor(m_player);
    new FakePlayerAdaptor(m_player);
    // A fresh connection and service name per test, so one test's player
    // leaving cannot race the next one's arriving.
    const QString connectionName = QStringLiteral("fake-%1").arg(++m_round);
    QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, connectionName);
    QVERIFY(bus.isConnected());
    QVERIFY(bus.registerObject(QStringLiteral("/org/mpris/MediaPlayer2"), m_player,
                               QDBusConnection::ExportAdaptors));
    m_serviceName = QStringLiteral("org.mpris.MediaPlayer2.fake%1").arg(m_round);
    QVERIFY(bus.registerService(m_serviceName));
}

void TestMediaBridge::cleanup() {
    if (!m_player) {
        return;
    }
    QDBusConnection bus(QStringLiteral("fake-%1").arg(m_round));
    bus.unregisterService(m_serviceName);
    bus.unregisterObject(QStringLiteral("/org/mpris/MediaPlayer2"));
    QDBusConnection::disconnectFromBus(QStringLiteral("fake-%1").arg(m_round));
    delete m_player;
    m_player = nullptr;
}

QJsonObject TestMediaBridge::playerEntry(const MediaBridge &bridge) const {
    const QString id = m_serviceName.mid(QStringLiteral("org.mpris.MediaPlayer2.").size());
    for (const QJsonValue &value : bridge.snapshot().value(QLatin1StringView("players")).toArray()) {
        if (value.toObject().value(QLatin1StringView("id")).toString() == id) {
            return value.toObject();
        }
    }
    return {};
}

static QDBusConnection bridgeBus(int round) {
    return QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                         QStringLiteral("bridge-%1").arg(round));
}

void TestMediaBridge::aPlayerIsFoundAndRead() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    MediaBridge bridge(bridgeBus(m_round));
    bridge.setVolumeTool(QString());
    QVERIFY(bridge.ensureStarted());

    QTRY_VERIFY(!playerEntry(bridge).isEmpty());
    QTRY_COMPARE(playerEntry(bridge).value(QLatin1StringView("name")).toString(),
                 QStringLiteral("Fake Player")); // newline cleaned
    const QJsonObject entry = playerEntry(bridge);
    QCOMPARE(entry.value(QLatin1StringView("title")).toString(), QStringLiteral("Seven Nation Army"));
    QCOMPARE(entry.value(QLatin1StringView("artist")).toString(), QStringLiteral("The White Stripes"));
    QCOMPARE(entry.value(QLatin1StringView("album")).toString(), QStringLiteral("Elephant"));
    QCOMPARE(entry.value(QLatin1StringView("status")).toString(), QStringLiteral("paused"));
    QCOMPARE(entry.value(QLatin1StringView("lengthMs")).toInteger(), qint64(231'000));
    QCOMPARE(entry.value(QLatin1StringView("positionMs")).toInteger(), qint64(12'000));
    QCOMPARE(entry.value(QLatin1StringView("volume")).toInt(), 50);
    QCOMPARE(entry.value(QLatin1StringView("canNext")).toBool(), true);
    QCOMPARE(entry.value(QLatin1StringView("canPrevious")).toBool(), false);
    QCOMPARE(entry.value(QLatin1StringView("canSeek")).toBool(), true);
}

void TestMediaBridge::commandsReachThePlayer() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    MediaBridge bridge(bridgeBus(m_round));
    bridge.setVolumeTool(QString());
    QVERIFY(bridge.ensureStarted());
    QTRY_VERIFY(!playerEntry(bridge).isEmpty());
    const QString id = playerEntry(bridge).value(QLatin1StringView("id")).toString();

    QString error;
    QVERIFY(bridge.command(id, MediaAction::PlayPause, 0, error));
    QTRY_COMPARE(m_player->calls, QStringList{QStringLiteral("PlayPause")});
    // The player emits no PropertiesChanged here; the bridge reads it back
    // after the call anyway.
    QTRY_COMPARE(playerEntry(bridge).value(QLatin1StringView("status")).toString(),
                 QStringLiteral("playing"));

    QVERIFY(bridge.command(id, MediaAction::SetVolume, 80, error));
    QTRY_COMPARE(m_player->volume, 0.8);
}

void TestMediaBridge::seekIsAbsoluteAndTiedToTheTrack() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    MediaBridge bridge(bridgeBus(m_round));
    bridge.setVolumeTool(QString());
    QVERIFY(bridge.ensureStarted());
    QTRY_VERIFY(!playerEntry(bridge).isEmpty());
    const QString id = playerEntry(bridge).value(QLatin1StringView("id")).toString();

    QString error;
    QVERIFY(bridge.command(id, MediaAction::Seek, 61'000, error));
    QTRY_COMPARE(m_player->lastSetPosition, qlonglong(61'000'000));
    QCOMPARE(m_player->lastSetTrack, QStringLiteral("/track/1"));

    // Past the end is clamped to the track length, not passed on.
    QVERIFY(bridge.command(id, MediaAction::Seek, 999'999'000, error));
    QTRY_COMPARE(m_player->lastSetPosition, qlonglong(231'000'000));

    QVERIFY(!bridge.command(id, MediaAction::Seek, -1, error));
}

void TestMediaBridge::whatThePlayerCannotDoIsRefused() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    MediaBridge bridge(bridgeBus(m_round));
    bridge.setVolumeTool(QString());
    QVERIFY(bridge.ensureStarted());
    QTRY_VERIFY(!playerEntry(bridge).isEmpty());
    const QString id = playerEntry(bridge).value(QLatin1StringView("id")).toString();

    QString error;
    QVERIFY(!bridge.command(id, MediaAction::Previous, 0, error)); // CanGoPrevious = false
    QVERIFY(!error.isEmpty());
    QVERIFY(!bridge.command(id, MediaAction::SetVolume, 101, error));
    QVERIFY(!bridge.command(id, MediaAction::SystemVolume, 50, error)); // no tool configured
    QTest::qWait(100);
    QVERIFY(m_player->calls.isEmpty());
}

void TestMediaBridge::anUnknownPlayerIsRefused() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    MediaBridge bridge(bridgeBus(m_round));
    bridge.setVolumeTool(QString());
    QVERIFY(bridge.ensureStarted());
    QTRY_VERIFY(!playerEntry(bridge).isEmpty());

    // Ids are looked up among the players the bridge found — never used to
    // build a bus name. A real service's name that the bridge did not list
    // is as unknown as nonsense.
    QString error;
    QVERIFY(!bridge.command(QStringLiteral("org.freedesktop.DBus"), MediaAction::PlayPause, 0, error));
    QVERIFY(!bridge.command(QStringLiteral("../../x"), MediaAction::PlayPause, 0, error));
    QVERIFY(!bridge.command(QString(), MediaAction::Next, 0, error));
}

void TestMediaBridge::aPlayerThatQuitsDisappears() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    MediaBridge bridge(bridgeBus(m_round));
    bridge.setVolumeTool(QString());
    QVERIFY(bridge.ensureStarted());
    QTRY_VERIFY(!playerEntry(bridge).isEmpty());

    QSignalSpy changed(&bridge, &MediaBridge::changed);
    QDBusConnection(QStringLiteral("fake-%1").arg(m_round)).unregisterService(m_serviceName);
    QTRY_VERIFY(playerEntry(bridge).isEmpty());
    QTRY_VERIFY(!changed.isEmpty());
}

void TestMediaBridge::systemVolumeComesFromTheTool() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString tool = dir.filePath(QStringLiteral("wpctl"));
    const QString log = dir.filePath(QStringLiteral("args"));
    {
        QFile script(tool);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(QStringLiteral("#!/bin/sh\necho \"$@\" >> '%1'\n"
                                    "echo 'Volume: 0.40 [MUTED]'\n").arg(log).toUtf8());
    }
    QFile::setPermissions(tool, QFileDevice::ReadOwner | QFileDevice::ExeOwner);

    MediaBridge bridge(bridgeBus(m_round));
    bridge.setVolumeTool(tool);
    QVERIFY(bridge.ensureStarted());
    QTRY_COMPARE(bridge.snapshot().value(QLatin1StringView("systemVolume")).toInt(), 40);
    QCOMPARE(bridge.snapshot().value(QLatin1StringView("systemMuted")).toBool(), true);

    QString error;
    QVERIFY(bridge.command(QString(), MediaAction::SystemVolume, 65, error));
    QTRY_VERIFY([&] {
        QFile f(log);
        return f.open(QIODevice::ReadOnly)
            && f.readAll().contains("set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 0.65");
    }());
}

void TestMediaBridge::systemVolumeIsBounded() {
    if (!haveBus()) {
        QSKIP("no session bus (run under dbus-run-session)");
    }
    MediaBridge bridge(bridgeBus(m_round));
    bridge.setVolumeTool(QStringLiteral("/bin/true"));
    QVERIFY(bridge.ensureStarted());
    QString error;
    QVERIFY(!bridge.command(QString(), MediaAction::SystemVolume, 101, error));
    QVERIFY(!bridge.command(QString(), MediaAction::SystemVolume, -1, error));
    QVERIFY(!bridge.command(QString(), MediaAction::SystemMute, 2, error));
    QVERIFY(bridge.command(QString(), MediaAction::SystemMute, 1, error));
}

QTEST_MAIN(TestMediaBridge)
#include "tst_mediabridge.moc"
