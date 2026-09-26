#include <QtTest>

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QImage>

#include "mazeconnect/core/Connection.h"
#include "mazeconnect/core/DeviceManager.h"
#include "mazeconnect/core/Identity.h"
#include "mazeconnect/core/Limits.h"
#include "mazeconnect/core/PhoneStatus.h"
#include "mazeconnect/core/Version.h"

using namespace mazeconnect::core;

/**
 * The phone-facing features — phone status, find my phone, shared text —
 * against a peer that is willing to misbehave.
 *
 * Two well-behaved DeviceManagers cannot show that the computer *refuses*
 * something, only that nothing asked for it. So the phone here is a bare
 * Connection holding a key the computer has pinned: a genuinely paired peer,
 * over real mutual TLS, that sends whatever the test tells it to — including
 * answers nobody requested, floods, and text crafted to mislead.
 */
class TestPhoneLink : public QObject {
    Q_OBJECT

private slots:
    void sanitizeKeepsOnlyValidatedFields();
    void sanitizeRejectsNonsense();

    void unsolicitedPhoneStatusIsDropped();
    void aRequestedReadingArrivesValidated();
    void sharedTextIsGatedAndRateLimited();
    void sharedTextRefusesMisleadingCharacters();
    void findPhoneResultsNeedARing();

    void fullControlIsOptIn();
    void theComputerIsAskedAndOnceMeansOnce();
    void presenterRunsAndStopsOnRevoke();
    void sharedFolderOverTheLink();
    void clipboardSyncIsGated();

    void initTestCase();

private:
    struct Harness {
        std::unique_ptr<QTemporaryDir> dir;
        std::unique_ptr<DeviceManager> computer;
        Identity phoneIdentity;
        std::unique_ptr<DeviceStore> phoneStore;
        std::unique_ptr<Connection> phone;
        quint64 counter = 1;
        QList<Message> received;

        bool send(const Message &m) { return phone->send(m); }
        quint64 next() { return ++counter; }
    };

    /// A computer with one pinned phone, and that phone linked and helloed.
    static std::unique_ptr<Harness> link(const QStringList &enabled = {});
};

namespace {

constexpr auto kPhoneId = "phone-1";

QJsonObject goodStatus() {
    return QJsonObject{
        {"battery", QJsonObject{{"level", 82}, {"charging", true}, {"plug", "ac"},
                                {"temperature", 314}, {"health", "good"}}},
        {"storage", QJsonObject{{"free", 46.0 * 1024 * 1024 * 1024},
                                {"total", 256.0 * 1024 * 1024 * 1024}}},
        {"memory", QJsonObject{{"available", 3.0 * 1024 * 1024 * 1024},
                               {"total", 8.0 * 1024 * 1024 * 1024}}},
        {"network", QJsonObject{{"type", "wifi"}, {"signal", 3}, {"metered", false}}},
        {"ringer", "vibrate"},
        {"dnd", true},
        {"powerSave", false},
        {"screenOn", true},
        {"model", "SM-S911B"},
        {"manufacturer", "samsung"},
        {"android", "15"},
        {"uptimeMs", 3600000},
    };
}

} // namespace

std::unique_ptr<TestPhoneLink::Harness> TestPhoneLink::link(const QStringList &enabled) {
    auto h = std::make_unique<Harness>();
    h->dir = std::make_unique<QTemporaryDir>();
    h->phoneIdentity = Identity::generate(QStringLiteral("phone"));

    // The pairing record, written the way DeviceStore writes one.
    // What a real pairing grants: everything except the opt-in ones.
    const QStringList caps = enabled.isEmpty()
        ? capabilitiesToNames(defaultEnabledCapabilities())
        : enabled;
    QJsonObject record{
        {"deviceId", QString::fromLatin1(kPhoneId)},
        {"deviceName", "Test phone"},
        {"deviceType", "mobile"},
        {"publicKey", QString::fromLatin1(h->phoneIdentity.publicKey().toBase64())},
        {"pairedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"enabledCapabilities", QJsonArray::fromStringList(caps)},
        {"knownCapabilities",
         QJsonArray::fromStringList(capabilitiesToNames(supportedCapabilities()))},
    };
    {
        QFile f(QDir(h->dir->path()).filePath(QStringLiteral("devices.json")));
        if (!f.open(QIODevice::WriteOnly)) {
            return nullptr;
        }
        f.write(QJsonDocument(QJsonArray{record}).toJson());
    }

    h->computer = std::make_unique<DeviceManager>(h->dir->path());
    if (!h->computer->start(QStringLiteral("computer"), QStringLiteral("desktop"))) {
        return nullptr;
    }

    // The phone does not need to verify the computer for these tests — the
    // property under test is what the computer accepts from a pinned key.
    h->phoneStore = std::make_unique<DeviceStore>(
        QDir(h->dir->path()).filePath(QStringLiteral("phone-devices.json")));
    h->phone = std::make_unique<Connection>(h->phoneIdentity, h->phoneStore.get(),
                                            Connection::Mode::Pairing);
    Harness *raw = h.get();
    QObject::connect(h->phone.get(), &Connection::messageReceived, h->phone.get(),
                     [raw](const Message &m) { raw->received.append(m); });

    QSignalSpy established(h->phone.get(), &Connection::established);
    QSignalSpy ready(h->computer.get(), &DeviceManager::deviceReady);
    h->phone->connectToPeer(QHostAddress::LocalHost, h->computer->listenPort());
    if (!established.wait(5000)) {
        return nullptr;
    }
    h->send(Message::hello(h->counter, QString::fromLatin1(kPhoneId), QStringLiteral("Test phone"),
                           QStringLiteral("mobile"), supportedCapabilities(),
                           QString::number(kProtocolVersion)));
    if (!ready.wait(5000)) {
        return nullptr;
    }
    return h;
}

namespace {

class FakeBackend : public InputBackend {
public:
    int keys = 0;
    bool active = false;
    void start() override { active = true; emit started(true, QString()); }
    void stop() override { active = false; }
    bool isActive() const override { return active; }
    void pointerMotion(double, double) override {}
    void pointerButton(int, bool) override {}
    void pointerScroll(int, int) override {}
    void keysym(int, bool) override { ++keys; }
};

QTemporaryDir *sharedDir() {
    static QTemporaryDir dir;
    return &dir;
}

template <typename Pred>
bool received(const QList<Message> &list, Pred pred) {
    return std::any_of(list.cbegin(), list.cend(), pred);
}

} // namespace

void TestPhoneLink::initTestCase() {
    // Every DeviceManager here reads its shared folder from this.
    qputenv("MAZECONNECT_SHARED_DIR", sharedDir()->filePath("shared").toUtf8());
}

// ---- The sanitiser ---------------------------------------------------------

void TestPhoneLink::sanitizeKeepsOnlyValidatedFields() {
    const QVariantMap s = phonestatus::sanitize(goodStatus());
    QCOMPARE(s.value("batteryLevel").toInt(), 82);
    QCOMPARE(s.value("charging").toBool(), true);
    QCOMPARE(s.value("plug").toString(), QStringLiteral("ac"));
    QCOMPARE(s.value("batteryTemp").toDouble(), 31.4);
    QCOMPARE(s.value("network").toString(), QStringLiteral("wifi"));
    QCOMPARE(s.value("signal").toInt(), 3);
    QCOMPARE(s.value("ringer").toString(), QStringLiteral("vibrate"));
    QCOMPARE(s.value("dnd").toBool(), true);
    QCOMPARE(s.value("model").toString(), QStringLiteral("SM-S911B"));
    QCOMPARE(s.value("storageTotal").toDouble(), 256.0 * 1024 * 1024 * 1024);
    QCOMPARE(s.value("memoryAvailable").toDouble(), 3.0 * 1024 * 1024 * 1024);
    QCOMPARE(s.value("uptimeMs").toDouble(), 3600000.0);

    // Nothing the phone sent beyond the known fields comes through.
    QJsonObject extra = goodStatus();
    extra.insert("script", "<b>hi</b>");
    QVERIFY(!phonestatus::sanitize(extra).contains("script"));
}

void TestPhoneLink::sanitizeRejectsNonsense() {
    QJsonObject bad{
        {"battery", QJsonObject{{"level", 101}, {"charging", "yes"}, {"plug", "nuclear"},
                                {"temperature", 1e9}, {"health", "fine"}}},
        // More free than there is space: a broken reading, not a full disk.
        {"storage", QJsonObject{{"free", 20}, {"total", 10}}},
        // Beyond any phone.
        {"memory", QJsonObject{{"available", 1}, {"total", 1e18}}},
        {"network", QJsonObject{{"type", "carrier-pigeon"}, {"signal", 9}}},
        {"ringer", "LOUD"},
        {"dnd", 1},
        {"model", QStringLiteral("Pixel\n8")},
        {"manufacturer", QString(QChar(0x202E)) + QStringLiteral("elgooG")},
        {"android", QString(200, QLatin1Char('x'))},
        {"uptimeMs", -5},
    };
    QVERIFY2(phonestatus::sanitize(bad).isEmpty(),
             "a field that failed validation still reached the dashboard");

    // Level 0 is a real reading and must survive as 0, not be dropped.
    QJsonObject empty{{"battery", QJsonObject{{"level", 0}}}};
    QCOMPARE(phonestatus::sanitize(empty).value("batteryLevel").toInt(), 0);
    QVERIFY(phonestatus::sanitize(QJsonObject()).isEmpty());
}

// ---- Over the link ---------------------------------------------------------

void TestPhoneLink::unsolicitedPhoneStatusIsDropped() {
    auto h = link();
    QVERIFY(h);
    QSignalSpy reading(h->computer.get(), &DeviceManager::phoneStatusReceived);

    // Nobody asked. Being paired is not a licence to put content on screen.
    h->send(Message::phoneStatus(h->next(), goodStatus()));
    QTest::qWait(300);
    QCOMPARE(reading.count(), 0);
    QVERIFY(h->computer->isConnected(QString::fromLatin1(kPhoneId)));
}

void TestPhoneLink::aRequestedReadingArrivesValidated() {
    auto h = link();
    QVERIFY(h);
    QSignalSpy reading(h->computer.get(), &DeviceManager::phoneStatusReceived);

    QVERIFY(h->computer->requestPhoneStatus(QString::fromLatin1(kPhoneId)));
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(h->received.cbegin(), h->received.cend(),
                    [](const Message &m) { return m.type() == MessageType::PhoneStatusRequest; }),
        3000);

    h->send(Message::phoneStatus(h->next(), goodStatus()));
    QTRY_COMPARE_WITH_TIMEOUT(reading.count(), 1, 3000);
    QCOMPARE(reading.at(0).at(0).toString(), QString::fromLatin1(kPhoneId));
    const QVariantMap status = reading.at(0).at(1).toMap();
    QCOMPARE(status.value("batteryLevel").toInt(), 82);
    QCOMPARE(reading.at(0).at(2).toString(), QString());

    // One request, one answer: a second reading riding on the first request
    // is unsolicited again.
    h->send(Message::phoneStatus(h->next(), goodStatus()));
    QTest::qWait(300);
    QCOMPARE(reading.count(), 1);

    // A refusal is delivered as one, with nothing in it.
    QVERIFY(h->computer->requestPhoneStatus(QString::fromLatin1(kPhoneId)));
    QTest::qWait(100);
    h->send(Message::phoneStatusUnavailable(h->next(), QStringLiteral("sharing is off")));
    QTRY_COMPARE_WITH_TIMEOUT(reading.count(), 2, 3000);
    QVERIFY(reading.at(1).at(1).toMap().isEmpty());
    QCOMPARE(reading.at(1).at(2).toString(), QStringLiteral("sharing is off"));
}

void TestPhoneLink::sharedTextIsGatedAndRateLimited() {
    auto h = link();
    QVERIFY(h);
    QSignalSpy shared(h->computer.get(), &DeviceManager::textShared);

    // Multi-line text is ordinary text — a clipboard is rarely one line.
    h->send(Message::shareText(h->next(), QStringLiteral("line one\nline two\tend")));
    QTRY_COMPARE_WITH_TIMEOUT(shared.count(), 1, 3000);
    QCOMPARE(shared.at(0).at(1).toString(), QStringLiteral("line one\nline two\tend"));

    // A flood: only the window's allowance gets through.
    for (int i = 0; i < 10; ++i) {
        h->send(Message::shareText(h->next(), QStringLiteral("spam %1").arg(i)));
    }
    QTest::qWait(400);
    QCOMPARE(shared.count(), limits::kMaxShareTextsPerWindow);

    // Revoked on the computer: refused whatever the phone thinks.
    QVERIFY(h->computer->setCapabilityEnabled(QString::fromLatin1(kPhoneId),
                                              Capability::ShareText, false));
    QTest::qWait(limits::kShareTextWindowMs + 100);
    h->send(Message::shareText(h->next(), QStringLiteral("after revoke")));
    QTest::qWait(300);
    QCOMPARE(shared.count(), limits::kMaxShareTextsPerWindow);
}

void TestPhoneLink::sharedTextRefusesMisleadingCharacters() {
    auto h = link();
    QVERIFY(h);
    QSignalSpy shared(h->computer.get(), &DeviceManager::textShared);

    // A right-to-left override makes a link display as somewhere it does not
    // go; a raw escape can reach a terminal someone pastes into.
    h->send(Message::shareText(h->next(), QStringLiteral("https://example.com/")
                                              + QChar(0x202E) + QStringLiteral("gpj.exe")));
    h->send(Message::shareText(h->next(), QStringLiteral("rm -rf ~") + QChar(0x1B)
                                              + QStringLiteral("[2K")));
    // Over the bound: refused, never truncated into something else.
    h->send(Message::shareText(h->next(),
                               QString(limits::kMaxShareTextChars + 1, QLatin1Char('a'))));
    QTest::qWait(400);
    QCOMPARE(shared.count(), 0);
}

void TestPhoneLink::findPhoneResultsNeedARing() {
    auto h = link();
    QVERIFY(h);
    QSignalSpy answered(h->computer.get(), &DeviceManager::findPhoneAnswered);

    // Unasked: dropped.
    h->send(Message::findPhoneResult(h->next(), true, QString()));
    QTest::qWait(300);
    QCOMPARE(answered.count(), 0);

    QVERIFY(h->computer->ringPhone(QString::fromLatin1(kPhoneId), true));
    QTRY_VERIFY_WITH_TIMEOUT(
        std::any_of(h->received.cbegin(), h->received.cend(),
                    [](const Message &m) {
                        return m.type() == MessageType::FindPhone
                            && m.boolean(QLatin1StringView("ring"));
                    }),
        3000);

    h->send(Message::findPhoneResult(h->next(), true, QString()));
    QTRY_COMPARE_WITH_TIMEOUT(answered.count(), 1, 3000);
    QCOMPARE(answered.at(0).at(1).toBool(), true);

    // The person holding the phone silences it: accepted, because it is
    // still ringing as far as this side knows.
    h->send(Message::findPhoneResult(h->next(), false, QString()));
    QTRY_COMPARE_WITH_TIMEOUT(answered.count(), 2, 3000);
    QCOMPARE(answered.at(1).at(1).toBool(), false);

    // And after that, nothing more is taken.
    h->send(Message::findPhoneResult(h->next(), true, QString()));
    QTest::qWait(300);
    QCOMPARE(answered.count(), 2);

    // Revoked: the computer will not even send a ring.
    QVERIFY(h->computer->setCapabilityEnabled(QString::fromLatin1(kPhoneId),
                                              Capability::FindPhone, false));
    QVERIFY(!h->computer->ringPhone(QString::fromLatin1(kPhoneId), true));
}

void TestPhoneLink::fullControlIsOptIn() {
    auto h = link();
    QVERIFY(h);
    auto *backend = new FakeBackend;
    h->computer->setInputBackendForTesting(backend);

    // Straight out of pairing: no mouse and keyboard.
    h->send(Message::inputSession(h->next(), true, QStringLiteral("full")));
    QTRY_VERIFY_WITH_TIMEOUT(received(h->received, [](const Message &m) {
        return m.type() == MessageType::InputState && !m.boolean(QLatin1StringView("active"))
            && !m.string(QLatin1StringView("error"), 300).isEmpty();
    }), 3000);
    QVERIFY(!backend->active);
    h->send(Message::inputEvent(h->next(), QJsonObject{{"kind", "key"}, {"key", "enter"}}));
    QTest::qWait(200);
    QCOMPARE(backend->keys, 0);

    // The owner switches it on, on the computer: now it runs.
    QVERIFY(h->computer->setCapabilityEnabled(QString::fromLatin1(kPhoneId),
                                              Capability::RemoteInput, true));
    h->received.clear();
    h->send(Message::inputSession(h->next(), true, QStringLiteral("full")));
    QTRY_VERIFY_WITH_TIMEOUT(received(h->received, [](const Message &m) {
        return m.type() == MessageType::InputState && m.boolean(QLatin1StringView("active"));
    }), 3000);
    h->send(Message::inputEvent(h->next(), QJsonObject{{"kind", "key"}, {"key", "enter"}}));
    QTRY_COMPARE_WITH_TIMEOUT(backend->keys, 2, 3000);

    // An older pairing record is not granted it by the migration either.
    QVERIFY(!defaultEnabledCapabilities().testFlag(Capability::RemoteInput));
}

void TestPhoneLink::theComputerIsAskedAndOnceMeansOnce() {
    auto h = link();
    QVERIFY(h);
    auto *backend = new FakeBackend;
    h->computer->setInputBackendForTesting(backend);
    QSignalSpy asked(h->computer.get(), &DeviceManager::remoteInputApprovalRequested);
    const QString phone = QString::fromLatin1(kPhoneId);

    // Not allowed: the phone is told to wait, and the owner is asked.
    h->send(Message::inputSession(h->next(), true, QStringLiteral("full")));
    QTRY_COMPARE_WITH_TIMEOUT(asked.count(), 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(received(h->received, [](const Message &m) {
        return m.type() == MessageType::InputState && m.boolean(QLatin1StringView("pending"));
    }), 3000);
    QVERIFY(!backend->active);

    // An answer for some other device changes nothing.
    h->computer->answerRemoteInput(QStringLiteral("someone-else"),
                                   DeviceManager::InputApproval::Always);
    QVERIFY(!backend->active);

    // Allow once: it runs now, without the phone asking again...
    h->computer->answerRemoteInput(phone, DeviceManager::InputApproval::Once);
    QTRY_VERIFY_WITH_TIMEOUT(backend->active, 3000);
    h->send(Message::inputEvent(h->next(), QJsonObject{{"kind", "key"}, {"key", "enter"}}));
    QTRY_COMPARE_WITH_TIMEOUT(backend->keys, 2, 3000);
    // ...and the capability itself was not switched on.
    QVERIFY(!h->computer->allows(phone, Capability::RemoteInput));

    // Once means once: after the session ends, the next one asks again.
    h->computer->stopRemoteInput();
    QTRY_VERIFY_WITH_TIMEOUT(!backend->active, 3000);
    h->send(Message::inputSession(h->next(), true, QStringLiteral("full")));
    QTRY_COMPARE_WITH_TIMEOUT(asked.count(), 2, 3000);
    QVERIFY(!backend->active);

    // Deny: refused, and nothing starts.
    h->received.clear();
    h->computer->answerRemoteInput(phone, DeviceManager::InputApproval::Deny);
    QTRY_VERIFY_WITH_TIMEOUT(received(h->received, [](const Message &m) {
        return m.type() == MessageType::InputState && !m.boolean(QLatin1StringView("active"))
            && !m.boolean(QLatin1StringView("pending"));
    }), 3000);
    QVERIFY(!backend->active);
    // A stale answer after the question closed does nothing either.
    h->computer->answerRemoteInput(phone, DeviceManager::InputApproval::Always);
    QTest::qWait(200);
    QVERIFY(!backend->active);
    QVERIFY(!h->computer->allows(phone, Capability::RemoteInput));
}

void TestPhoneLink::presenterRunsAndStopsOnRevoke() {
    auto h = link();
    QVERIFY(h);
    auto *backend = new FakeBackend;
    h->computer->setInputBackendForTesting(backend);
    QSignalSpy changed(h->computer.get(), &DeviceManager::remoteInputChanged);

    h->send(Message::inputSession(h->next(), true, QStringLiteral("presenter")));
    QTRY_VERIFY_WITH_TIMEOUT(backend->active, 3000);
    h->send(Message::inputEvent(h->next(), QJsonObject{{"kind", "key"}, {"key", "right"}}));
    QTRY_COMPARE_WITH_TIMEOUT(backend->keys, 2, 3000);
    // Presenter cannot type.
    h->send(Message::inputEvent(h->next(), QJsonObject{{"kind", "text"}, {"text", "id"}}));
    QTest::qWait(200);
    QCOMPARE(backend->keys, 2);

    // Revoked on the computer mid-session: it ends at once.
    QVERIFY(h->computer->setCapabilityEnabled(QString::fromLatin1(kPhoneId),
                                              Capability::Presenter, false));
    QTRY_VERIFY_WITH_TIMEOUT(!backend->active, 3000);
    QVERIFY(!changed.isEmpty());
    QCOMPARE(changed.last().at(1).toBool(), false);
}

void TestPhoneLink::sharedFolderOverTheLink() {
    const QString root = sharedDir()->filePath("shared");
    QDir().mkpath(root + "/Docs");
    {
        QFile f(root + "/Docs/cv.txt");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hello");
    }
    auto h = link();
    QVERIFY(h);

    h->send(Message::folderList(h->next(), 7, QStringLiteral("Docs")));
    QTRY_VERIFY_WITH_TIMEOUT(received(h->received, [](const Message &m) {
        return m.type() == MessageType::FolderListing
            && m.integer(QLatin1StringView("requestId"), 100) == 7
            && m.body().value(QLatin1StringView("entries")).toArray().size() == 1;
    }), 3000);

    // Out of the folder: an error, and no entries.
    h->send(Message::folderList(h->next(), 8, QStringLiteral("../..")));
    QTRY_VERIFY_WITH_TIMEOUT(received(h->received, [](const Message &m) {
        return m.type() == MessageType::FolderListing
            && m.integer(QLatin1StringView("requestId"), 100) == 8
            && !m.string(QLatin1StringView("error"), 300).isEmpty()
            && m.body().value(QLatin1StringView("entries")).toArray().isEmpty();
    }), 3000);

    // A fetch: the result names the transfer, and the offer that follows is it.
    h->send(Message::folderFetch(h->next(), 9, QStringLiteral("Docs/cv.txt")));
    QTRY_VERIFY_WITH_TIMEOUT(received(h->received, [](const Message &m) {
        return m.type() == MessageType::FileOffer;
    }), 3000);
    qint64 promised = -1;
    qint64 offered = -2;
    int resultIndex = -1;
    int offerIndex = -1;
    for (int i = 0; i < h->received.size(); ++i) {
        const Message &m = h->received.at(i);
        if (m.type() == MessageType::FolderFetchResult) {
            promised = m.integer(QLatin1StringView("transferId"), 0xFFFFFFFFLL);
            resultIndex = i;
        } else if (m.type() == MessageType::FileOffer) {
            offered = m.integer(QLatin1StringView("transferId"), 0xFFFFFFFFLL);
            offerIndex = i;
        }
    }
    QCOMPARE(promised, offered);
    QVERIFY(resultIndex >= 0 && resultIndex < offerIndex);

    // A preview of a picture comes back as a JPEG; a path out does not.
    {
        QImage img(640, 480, QImage::Format_RGB32);
        img.fill(Qt::blue);
        QVERIFY(img.save(root + "/Docs/pic.png"));
    }
    h->send(Message::folderPreview(h->next(), 11, QStringLiteral("Docs/pic.png"), false));
    QTRY_VERIFY_WITH_TIMEOUT(received(h->received, [](const Message &m) {
        return m.type() == MessageType::FolderPreviewResult
            && m.integer(QLatin1StringView("requestId"), 100) == 11
            && QByteArray::fromBase64(m.body().value(QLatin1StringView("data")).toString().toLatin1())
                   .startsWith("\xFF\xD8");
    }), 5000);
    h->send(Message::folderPreview(h->next(), 12, QStringLiteral("../../etc/passwd"), true));
    QTRY_VERIFY_WITH_TIMEOUT(received(h->received, [](const Message &m) {
        return m.type() == MessageType::FolderPreviewResult
            && m.integer(QLatin1StringView("requestId"), 100) == 12
            && !m.string(QLatin1StringView("error"), 300).isEmpty()
            && !m.body().contains(QLatin1StringView("data"));
    }), 3000);

    // Revoked: refused out loud.
    QVERIFY(h->computer->setCapabilityEnabled(QString::fromLatin1(kPhoneId),
                                              Capability::SharedFolder, false));
    h->send(Message::folderList(h->next(), 10, QString()));
    QTRY_VERIFY_WITH_TIMEOUT(received(h->received, [](const Message &m) {
        return m.type() == MessageType::FolderListing
            && m.integer(QLatin1StringView("requestId"), 100) == 10
            && !m.string(QLatin1StringView("error"), 300).isEmpty();
    }), 3000);
}

void TestPhoneLink::clipboardSyncIsGated() {
    auto h = link();
    QVERIFY(h);
    QSignalSpy clip(h->computer.get(), &DeviceManager::clipboardReceived);

    h->send(Message::clipboardSync(h->next(), QStringLiteral("copied\ntext")));
    QTRY_COMPARE_WITH_TIMEOUT(clip.count(), 1, 3000);
    h->send(Message::clipboardSync(h->next(), QString("x") + QChar(0x202e)));
    QTest::qWait(200);
    QCOMPARE(clip.count(), 1);

    QVERIFY(h->computer->setCapabilityEnabled(QString::fromLatin1(kPhoneId),
                                              Capability::ClipboardSync, false));
    h->send(Message::clipboardSync(h->next(), QStringLiteral("after")));
    QTest::qWait(300);
    QCOMPARE(clip.count(), 1);
    // And nothing goes out to a device that does not sync.
    QCOMPARE(h->computer->sendClipboard(QStringLiteral("mine")), 0);
}

QTEST_MAIN(TestPhoneLink)
#include "tst_phonelink.moc"
