#include <QtTest>

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mazeconnect/core/Approval.h"
#include "mazeconnect/core/ApprovalRelay.h"
#include "mazeconnect/core/AuditLog.h"
#include "mazeconnect/core/Connection.h"
#include "mazeconnect/core/DeviceManager.h"
#include "mazeconnect/core/DeviceStore.h"
#include "mazeconnect/core/Identity.h"
#include "mazeconnect/core/Limits.h"
#include "mazeconnect/core/Version.h"

using namespace mazeconnect::core;

/**
 * Unlocking this computer and approving sudo with a phone's fingerprint.
 *
 * The property that matters is what the computer *refuses*: a signature over
 * the wrong challenge, by the wrong key, for the wrong computer, from a phone
 * that was not asked, or from one the owner has not switched on. Each test
 * below is a way in that must stay shut.
 */
class TestApproval : public QObject {
    Q_OBJECT

private slots:
    void signingInputMatchesTheSharedVector();
    void aSignatureVerifiesOnlyForWhatWasSigned();
    void onlyP256KeysAreAccepted();
    void codesAndKeyCodes();

    void auditLogIsPrivateBoundedAndNewestFirst();
    void auditLogRotates();
    void aNewKeySwitchesApprovalOff();

    void approvalIsOptInAndNeedsAKey();
    void unlockNeedsPermission();
    void unlockWorksWithTheRightSignatureOnly();
    void unlockIsRefusedWhenNotLocked();
    void answersNobodyAskedForAreIgnored();
    void aNewKeyClosesWhatIsOpen();

    void relayRefusesStrangersAndNonsense();
    void relayCarriesTheSignatureAndTheVerdict();
    void relayAnswersAtOnceWithNoPhone();

    void lockPhoneNeedsARequest();
    void onePhoneCannotAnswerForAnother();

private:
    struct Key {
        EVP_PKEY *pkey = nullptr;
        QByteArray spki;
        ~Key() { EVP_PKEY_free(pkey); }
    };
    static std::unique_ptr<Key> newKey(const char *curve = "P-256");
    static QByteArray sign(const Key &key, const QByteArray &input);

    struct Harness {
        std::unique_ptr<QTemporaryDir> dir;
        std::unique_ptr<DeviceManager> computer;
        Identity phoneIdentity;
        std::unique_ptr<DeviceStore> phoneStore;
        std::unique_ptr<Connection> phone;
        quint64 counter = 1;
        QList<Message> received;

        // A second paired phone, when asked for.
        Identity otherIdentity;
        std::unique_ptr<Connection> other;
        QList<Message> otherReceived;
        quint64 otherCounter = 1;

        bool send(const Message &m) { return phone->send(m); }
        quint64 next() { return ++counter; }
        Message last(MessageType type) const {
            for (auto it = received.crbegin(); it != received.crend(); ++it) {
                if (it->type() == type) {
                    return *it;
                }
            }
            return {};
        }
        int count(MessageType type) const {
            return static_cast<int>(std::count_if(received.cbegin(), received.cend(),
                                                  [type](const Message &m) {
                                                      return m.type() == type;
                                                  }));
        }
    };

    /// A computer with one pinned phone, linked and helloed. @p approvalKey
    /// is recorded for it, and Approval switched on when @p allowApproval.
    static std::unique_ptr<Harness> link(const QByteArray &approvalKey = {},
                                         bool allowApproval = false,
                                         const QByteArray &otherApprovalKey = {});
};

namespace {

constexpr auto kPhoneId = "phone-1";

class FakeSession : public SessionControl {
public:
    State current = State::Locked;
    int unlocks = 0;
    State state(QString *error) override {
        if (current == State::Unknown && error) {
            *error = QStringLiteral("no session");
        }
        return current;
    }
    bool unlock(QString *) override {
        ++unlocks;
        current = State::Unlocked;
        return true;
    }
};

approval::Challenge vectorChallenge() {
    approval::Challenge c;
    c.purpose = approval::Purpose::Sudo;
    c.host = QStringLiteral("maze");
    c.user = QStringLiteral("berk");
    c.detail = QStringLiteral("sudo · pts/1");
    for (int i = 0; i < 32; ++i) {
        c.nonce.append(static_cast<char>(i));
    }
    c.code = QStringLiteral("47");
    c.issuedAt = 1790000000;
    return c;
}

QString currentUser() {
    const struct passwd *pw = ::getpwuid(::getuid());
    return QString::fromLocal8Bit(pw->pw_name);
}

/// The challenge as the phone would rebuild it from an approvalRequest.
approval::Challenge challengeFrom(const Message &request, const QString &code = QString()) {
    approval::Challenge c;
    c.purpose = *approval::purposeFromName(request.string(QLatin1StringView("purpose"), 16));
    c.host = request.string(QLatin1StringView("host"), limits::kMaxApprovalHostChars);
    c.user = request.string(QLatin1StringView("user"), limits::kMaxApprovalUserChars);
    c.detail = request.string(QLatin1StringView("detail"), limits::kMaxApprovalDetailChars);
    c.nonce = request.binary(QLatin1StringView("nonce"), limits::kApprovalNonceSize);
    c.code = code;
    c.issuedAt = request.integer(QLatin1StringView("issuedAt"), 1LL << 40);
    return c;
}

quint32 requestIdOf(const Message &m) {
    return static_cast<quint32>(m.integer(QLatin1StringView("requestId"), 0xFFFFFFFFLL));
}

} // namespace

std::unique_ptr<TestApproval::Key> TestApproval::newKey(const char *curve) {
    auto key = std::make_unique<Key>();
    key->pkey = EVP_EC_gen(curve);
    unsigned char *der = nullptr;
    const int len = i2d_PUBKEY(key->pkey, &der);
    key->spki = QByteArray(reinterpret_cast<const char *>(der), len);
    OPENSSL_free(der);
    return key;
}

QByteArray TestApproval::sign(const Key &key, const QByteArray &input) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key.pkey);
    size_t len = 0;
    EVP_DigestSign(ctx, nullptr, &len,
                   reinterpret_cast<const unsigned char *>(input.constData()), input.size());
    QByteArray out(static_cast<qsizetype>(len), Qt::Uninitialized);
    EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(out.data()), &len,
                   reinterpret_cast<const unsigned char *>(input.constData()), input.size());
    EVP_MD_CTX_free(ctx);
    out.resize(static_cast<qsizetype>(len));
    return out;
}

std::unique_ptr<TestApproval::Harness> TestApproval::link(const QByteArray &approvalKey,
                                                          bool allowApproval,
                                                          const QByteArray &otherApprovalKey) {
    auto h = std::make_unique<Harness>();
    h->dir = std::make_unique<QTemporaryDir>();
    h->phoneIdentity = Identity::generate(QStringLiteral("phone"));

    Capabilities caps = defaultEnabledCapabilities();
    caps.setFlag(Capability::Approval, allowApproval);
    QJsonObject record{
        {"deviceId", QString::fromLatin1(kPhoneId)},
        {"deviceName", "Test phone"},
        {"deviceType", "mobile"},
        {"publicKey", QString::fromLatin1(h->phoneIdentity.publicKey().toBase64())},
        {"pairedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"enabledCapabilities", QJsonArray::fromStringList(capabilitiesToNames(caps))},
        {"knownCapabilities",
         QJsonArray::fromStringList(capabilitiesToNames(supportedCapabilities()))},
    };
    if (!approvalKey.isEmpty()) {
        record.insert("approvalKey", QString::fromLatin1(approvalKey.toBase64()));
    }
    QJsonArray records{record};
    if (!otherApprovalKey.isEmpty()) {
        h->otherIdentity = Identity::generate(QStringLiteral("other"));
        QJsonObject second = record;
        second.insert("deviceId", "phone-2");
        second.insert("deviceName", "Other phone");
        second.insert("publicKey", QString::fromLatin1(h->otherIdentity.publicKey().toBase64()));
        second.insert("approvalKey", QString::fromLatin1(otherApprovalKey.toBase64()));
        records.append(second);
    }
    {
        QFile f(QDir(h->dir->path()).filePath(QStringLiteral("devices.json")));
        if (!f.open(QIODevice::WriteOnly)) {
            return nullptr;
        }
        f.write(QJsonDocument(records).toJson());
    }

    h->computer = std::make_unique<DeviceManager>(h->dir->path());
    h->computer->setSessionControlForTesting(new FakeSession);
    if (!h->computer->start(QStringLiteral("computer"), QStringLiteral("desktop"))) {
        return nullptr;
    }
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
    if (!otherApprovalKey.isEmpty()) {
        h->other = std::make_unique<Connection>(h->otherIdentity, h->phoneStore.get(),
                                                Connection::Mode::Pairing);
        QObject::connect(h->other.get(), &Connection::messageReceived, h->other.get(),
                         [raw](const Message &m) { raw->otherReceived.append(m); });
        QSignalSpy otherUp(h->other.get(), &Connection::established);
        h->other->connectToPeer(QHostAddress::LocalHost, h->computer->listenPort());
        if (!otherUp.wait(5000)) {
            return nullptr;
        }
        h->other->send(Message::hello(h->otherCounter, QStringLiteral("phone-2"),
                                      QStringLiteral("Other phone"), QStringLiteral("mobile"),
                                      supportedCapabilities(),
                                      QString::number(kProtocolVersion)));
        if (!ready.wait(5000)) {
            return nullptr;
        }
    }
    return h;
}

// ---- Crypto -----------------------------------------------------------------

void TestApproval::signingInputMatchesTheSharedVector() {
    const QByteArray verifier(32, static_cast<char>(0xAA));
    const QByteArray input = approval::signingInput(vectorChallenge(), verifier);
    QVERIFY(input.startsWith("maze-connect/approval/v1"));
    // The same digest is asserted by the phone's ApprovalSigningTest and by
    // pam/tests/test_signing_input.c. If one of the three drifts, approvals
    // stop verifying in the field; this is where that is caught instead.
    QCOMPARE(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex(),
             QByteArray("ecb446382e92746576b13cc8937fdee33d74a74dfb35f6d3d2f8257b3cf7b6c7"));
}

void TestApproval::aSignatureVerifiesOnlyForWhatWasSigned() {
    const auto key = newKey();
    const QByteArray verifier(32, static_cast<char>(0xAA));
    const approval::Challenge base = vectorChallenge();
    const QByteArray signature = sign(*key, approval::signingInput(base, verifier));
    QVERIFY(approval::verify(key->spki, approval::signingInput(base, verifier), signature));

    // Every field is bound: change any one and the signature is worthless.
    QList<approval::Challenge> altered;
    for (int i = 0; i < 7; ++i) {
        approval::Challenge c = base;
        switch (i) {
        case 0: c.purpose = approval::Purpose::Unlock; break;
        case 1: c.host = QStringLiteral("other"); break;
        case 2: c.user = QStringLiteral("root"); break;
        case 3: c.detail = QStringLiteral("sudo · pts/2"); break;
        case 4: c.nonce[0] = static_cast<char>(0x7F); break;
        case 5: c.code = QStringLiteral("48"); break;
        case 6: c.issuedAt += 1; break;
        }
        QVERIFY2(!approval::verify(key->spki, approval::signingInput(c, verifier), signature),
                 qPrintable(QStringLiteral("field %1 is not bound").arg(i)));
    }
    // Another computer's identity: the answer was not given to it.
    QVERIFY(!approval::verify(key->spki,
                              approval::signingInput(base, QByteArray(32, 'B')), signature));
    // Another key.
    const auto other = newKey();
    QVERIFY(!approval::verify(other->spki, approval::signingInput(base, verifier), signature));
    // Garbage and over-long signatures never reach OpenSSL as valid.
    QVERIFY(!approval::verify(key->spki, approval::signingInput(base, verifier),
                              QByteArray(limits::kMaxApprovalSignature + 1, 'x')));
    QVERIFY(!approval::verify(key->spki, approval::signingInput(base, verifier), QByteArray()));
}

void TestApproval::onlyP256KeysAreAccepted() {
    QVERIFY(approval::isAcceptableKey(newKey()->spki));
    QVERIFY(!approval::isAcceptableKey(newKey("P-384")->spki));
    QVERIFY(!approval::isAcceptableKey(QByteArray()));
    QByteArray trailing = newKey()->spki;
    trailing.append('\0');
    QVERIFY2(!approval::isAcceptableKey(trailing), "trailing bytes after the key were ignored");
    // A P-384 signature over the right input still does not verify as P-256.
    const auto big = newKey("P-384");
    const QByteArray input = approval::signingInput(vectorChallenge(), QByteArray(32, 'A'));
    QVERIFY(!approval::verify(big->spki, input, sign(*big, input)));
}

void TestApproval::codesAndKeyCodes() {
    QSet<QString> seen;
    for (int i = 0; i < 2000; ++i) {
        const QString code = approval::newCode();
        QCOMPARE(code.size(), limits::kApprovalCodeDigits);
        QVERIFY(code.toInt() >= 10 && code.toInt() <= 99);
        seen.insert(code);
    }
    QVERIFY(seen.size() > 80); // all of 10..99 turn up, near enough
    QCOMPARE(approval::newNonce().size(), limits::kApprovalNonceSize);
    QVERIFY(approval::newNonce() != approval::newNonce());

    const QString code = approval::keyCode(newKey()->spki);
    QVERIFY(QRegularExpression(QStringLiteral("^[0-9A-F]{4}(-[0-9A-F]{4}){3}$"))
                .match(code).hasMatch());
}

// ---- Audit log --------------------------------------------------------------

void TestApproval::auditLogIsPrivateBoundedAndNewestFirst() {
    QTemporaryDir dir;
    AuditLog log(dir.filePath(QStringLiteral("audit.jsonl")));
    QVERIFY(log.record(QStringLiteral("p1"), QStringLiteral("Phone"), QStringLiteral("first")));
    // A name built to forge a second entry ends up as one line with spaces.
    QVERIFY(log.record(QStringLiteral("p1"),
                       QStringLiteral("Evil\n{\"action\":\"forged\"}") + QChar(0x202E),
                       QStringLiteral("second"), QString(500, QLatin1Char('d')), false));

    struct stat st {};
    QCOMPARE(::stat(QFile::encodeName(log.path()).constData(), &st), 0);
    QCOMPARE(st.st_mode & 0777, 0600u);

    const auto entries = log.recent(10);
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).action, QStringLiteral("second"));
    QCOMPARE(entries.at(0).ok, false);
    QVERIFY(!entries.at(0).deviceName.contains(QLatin1Char('\n')));
    QVERIFY(!entries.at(0).deviceName.contains(QChar(0x202E)));
    QCOMPARE(entries.at(0).detail.size(), AuditLog::kMaxFieldChars);
    QCOMPARE(entries.at(1).action, QStringLiteral("first"));

    QFile raw(log.path());
    QVERIFY(raw.open(QIODevice::ReadOnly));
    QCOMPARE(raw.readAll().count('\n'), 2);

    QVERIFY(log.clear());
    QVERIFY(log.recent(10).isEmpty());
}

void TestApproval::auditLogRotates() {
    QTemporaryDir dir;
    AuditLog log(dir.filePath(QStringLiteral("audit.jsonl")));
    const QString detail(AuditLog::kMaxFieldChars, QLatin1Char('x'));
    int written = 0;
    while (QFileInfo(log.path()).size() < AuditLog::kRotateBytes) {
        QVERIFY(log.record(QStringLiteral("p"), QStringLiteral("n"),
                           QStringLiteral("a%1").arg(written++), detail));
    }
    QVERIFY(log.record(QStringLiteral("p"), QStringLiteral("n"), QStringLiteral("after")));
    QVERIFY(QFileInfo::exists(log.path() + QStringLiteral(".1")));
    QVERIFY(QFileInfo(log.path()).size() < 1024);
    // Both files are read, newest first.
    const auto entries = log.recent(3);
    QCOMPARE(entries.at(0).action, QStringLiteral("after"));
    QCOMPARE(entries.at(1).action, QStringLiteral("a%1").arg(written - 1));
}

void TestApproval::aNewKeySwitchesApprovalOff() {
    QTemporaryDir dir;
    DeviceStore store(dir.filePath(QStringLiteral("devices.json")));
    PairedDevice device;
    device.deviceId = QStringLiteral("p1");
    device.deviceName = QStringLiteral("Phone");
    device.publicKey = Identity::generate(QStringLiteral("p")).publicKey();
    QVERIFY(store.add(device));

    const auto first = newKey();
    bool changed = false;
    QVERIFY(store.setApprovalKey(device.publicKey, first->spki, &changed));
    QVERIFY(changed);
    Capabilities caps = store.deviceForId(QStringLiteral("p1")).enabledCapabilities;
    caps.setFlag(Capability::Approval);
    QVERIFY(store.update(device.publicKey, device.deviceName, caps));

    // The same key again changes nothing.
    QVERIFY(store.setApprovalKey(device.publicKey, first->spki, &changed));
    QVERIFY(!changed);
    QVERIFY(store.deviceForId(QStringLiteral("p1")).enabledCapabilities.testFlag(
        Capability::Approval));

    // A different one is a different question.
    QVERIFY(store.setApprovalKey(device.publicKey, newKey()->spki, &changed));
    QVERIFY(changed);
    QVERIFY(!store.deviceForId(QStringLiteral("p1")).enabledCapabilities.testFlag(
        Capability::Approval));

    // Not P-256: refused, and the record is untouched.
    const QByteArray before = store.deviceForId(QStringLiteral("p1")).approvalKey;
    QVERIFY(!store.setApprovalKey(device.publicKey, newKey("P-384")->spki));
    QCOMPARE(store.deviceForId(QStringLiteral("p1")).approvalKey, before);

    // It survives a reload; a corrupt one on disk does not, nor its switch.
    DeviceStore reloaded(store.path());
    QVERIFY(reloaded.load());
    QCOMPARE(reloaded.deviceForId(QStringLiteral("p1")).approvalKey, before);

    QFile f(store.path());
    QVERIFY(f.open(QIODevice::ReadOnly));
    QJsonArray array = QJsonDocument::fromJson(f.readAll()).array();
    f.close();
    QJsonObject obj = array.at(0).toObject();
    obj.insert("approvalKey", QString::fromLatin1(QByteArray("not a key").toBase64()));
    obj.insert("enabledCapabilities", QJsonArray{"approval", "fileTransfer"});
    array.replace(0, obj);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QJsonDocument(array).toJson());
    f.close();
    QVERIFY(reloaded.load());
    QVERIFY(reloaded.deviceForId(QStringLiteral("p1")).approvalKey.isEmpty());
    QVERIFY(!reloaded.deviceForId(QStringLiteral("p1")).enabledCapabilities.testFlag(
        Capability::Approval));
}

// ---- Over the link ----------------------------------------------------------

void TestApproval::approvalIsOptInAndNeedsAKey() {
    // Pairing does not grant it.
    QVERIFY(!defaultEnabledCapabilities().testFlag(Capability::Approval));

    auto h = link();
    QVERIFY(h);
    const QString id = QString::fromLatin1(kPhoneId);
    QVERIFY2(!h->computer->setCapabilityEnabled(id, Capability::Approval, true),
             "approval switched on with no key to approve");

    QSignalSpy keyChanged(h->computer.get(), &DeviceManager::approvalKeyChanged);
    const auto key = newKey();
    h->send(Message::approvalKey(h->next(), key->spki));
    QTRY_COMPARE_WITH_TIMEOUT(keyChanged.count(), 1, 3000);
    QCOMPARE(h->computer->approvalKey(id), key->spki);
    // Seeing a key is not saying yes to it.
    QVERIFY(!h->computer->allows(id, Capability::Approval));
    QVERIFY(h->computer->setCapabilityEnabled(id, Capability::Approval, true));
    QVERIFY(h->computer->allows(id, Capability::Approval));

    // A key that is not P-256 is refused outright.
    h->send(Message::approvalKey(h->next(), newKey("P-384")->spki));
    QTest::qWait(300);
    QCOMPARE(h->computer->approvalKey(id), key->spki);
    QVERIFY(h->computer->allows(id, Capability::Approval));

    // Withdrawn on the phone: gone here, and so is the permission.
    h->send(Message::approvalKey(h->next(), QByteArray()));
    QTRY_COMPARE_WITH_TIMEOUT(keyChanged.count(), 2, 3000);
    QVERIFY(h->computer->approvalKey(id).isEmpty());
    QVERIFY(!h->computer->allows(id, Capability::Approval));
}

void TestApproval::unlockNeedsPermission() {
    const auto key = newKey();
    auto h = link(key->spki, false);
    QVERIFY(h);
    h->send(Message::unlockRequest(h->next()));
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalResult), 1, 3000);
    QCOMPARE(h->last(MessageType::ApprovalResult).boolean(QLatin1StringView("ok"), true), false);
    QCOMPARE(h->count(MessageType::ApprovalRequest), 0);
}

void TestApproval::unlockWorksWithTheRightSignatureOnly() {
    const auto key = newKey();
    auto h = link(key->spki, true);
    QVERIFY(h);
    auto *session = new FakeSession;
    h->computer->setSessionControlForTesting(session);
    // The computer's identity as the phone pinned it.
    const QByteArray computerKey = h->phone->peerPublicKey();
    const QByteArray computerFp = Identity::fingerprintOf(computerKey);
    QSignalSpy handled(h->computer.get(), &DeviceManager::approvalHandled);
    QSignalSpy alert(h->computer.get(), &DeviceManager::securityAlert);

    // A signature by another key over the right challenge: refused.
    h->send(Message::unlockRequest(h->next()));
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalRequest), 1, 3000);
    Message request = h->last(MessageType::ApprovalRequest);
    QCOMPARE(request.string(QLatin1StringView("purpose"), 16), QStringLiteral("unlock"));
    QCOMPARE(request.boolean(QLatin1StringView("codeRequired"), true), false);
    const auto stranger = newKey();
    h->send(Message::approvalResponse(
        h->next(), requestIdOf(request),
        sign(*stranger, approval::signingInput(challengeFrom(request), computerFp)), QString()));
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalResult), 1, 3000);
    QCOMPARE(h->last(MessageType::ApprovalResult).boolean(QLatin1StringView("ok"), true), false);
    QCOMPARE(session->unlocks, 0);
    QCOMPARE(alert.count(), 1);

    // The right key, but bound to another computer: refused.
    h->send(Message::unlockRequest(h->next()));
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalRequest), 2, 3000);
    request = h->last(MessageType::ApprovalRequest);
    h->send(Message::approvalResponse(
        h->next(), requestIdOf(request),
        sign(*key, approval::signingInput(challengeFrom(request), QByteArray(32, 'X'))),
        QString()));
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalResult), 2, 3000);
    QCOMPARE(session->unlocks, 0);

    // The right key over the right challenge for this computer: unlocked.
    h->send(Message::unlockRequest(h->next()));
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalRequest), 3, 3000);
    request = h->last(MessageType::ApprovalRequest);
    const QByteArray good = sign(*key, approval::signingInput(challengeFrom(request), computerFp));
    h->send(Message::approvalResponse(h->next(), requestIdOf(request), good, QString()));
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalResult), 3, 3000);
    QCOMPARE(h->last(MessageType::ApprovalResult).boolean(QLatin1StringView("ok")), true);
    QCOMPARE(session->unlocks, 1);
    QCOMPARE(handled.last().at(2).toBool(), true);

    // Replaying the same answer does nothing: that challenge is spent.
    session->current = SessionControl::State::Locked;
    h->send(Message::approvalResponse(h->next(), requestIdOf(request), good, QString()));
    QTest::qWait(300);
    QCOMPARE(session->unlocks, 1);
    QCOMPARE(h->count(MessageType::ApprovalResult), 3);

    // And it is in the log.
    const auto entries = h->computer->auditLog().recent(5);
    QVERIFY(std::any_of(entries.cbegin(), entries.cend(), [](const AuditLog::Entry &e) {
        return e.action == QStringLiteral("Unlock the screen") && e.ok;
    }));
}

void TestApproval::unlockIsRefusedWhenNotLocked() {
    const auto key = newKey();
    auto h = link(key->spki, true);
    QVERIFY(h);
    auto *session = new FakeSession;
    session->current = SessionControl::State::Unlocked;
    h->computer->setSessionControlForTesting(session);
    h->send(Message::unlockRequest(h->next()));
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalResult), 1, 3000);
    // No fingerprint is asked for a screen that is open.
    QCOMPARE(h->count(MessageType::ApprovalRequest), 0);

    // Nor more than the window allows.
    session->current = SessionControl::State::Locked;
    for (int i = 0; i < limits::kMaxUnlockRequestsPerMinute + 3; ++i) {
        h->send(Message::unlockRequest(h->next()));
    }
    QTest::qWait(500);
    QVERIFY(h->count(MessageType::ApprovalRequest) <= limits::kMaxUnlockRequestsPerMinute);
}

void TestApproval::answersNobodyAskedForAreIgnored() {
    const auto key = newKey();
    auto h = link(key->spki, true);
    QVERIFY(h);
    auto *session = new FakeSession;
    h->computer->setSessionControlForTesting(session);

    // A signed answer with no challenge behind it.
    approval::Challenge made;
    made.purpose = approval::Purpose::Unlock;
    made.host = QStringLiteral("x");
    made.user = currentUser();
    made.nonce = QByteArray(32, 'n');
    made.issuedAt = QDateTime::currentSecsSinceEpoch();
    const QByteArray fp = Identity::fingerprintOf(h->phone->peerPublicKey());
    for (quint32 id : {0u, 1u, 7u, 0xFFFFFFFFu}) {
        h->send(Message::approvalResponse(h->next(), id,
                                          sign(*key, approval::signingInput(made, fp)),
                                          QString()));
    }
    QTest::qWait(400);
    QCOMPARE(session->unlocks, 0);
    QCOMPARE(h->count(MessageType::ApprovalResult), 0);
    // A misdirected answer is noise, not a reason to drop the link.
    QVERIFY(h->computer->isConnected(QString::fromLatin1(kPhoneId)));
}

void TestApproval::aNewKeyClosesWhatIsOpen() {
    const auto key = newKey();
    auto h = link(key->spki, true);
    QVERIFY(h);
    auto *session = new FakeSession;
    h->computer->setSessionControlForTesting(session);
    h->send(Message::unlockRequest(h->next()));
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalRequest), 1, 3000);
    const Message request = h->last(MessageType::ApprovalRequest);

    // The key changes while the challenge is open: the challenge closes, and
    // a signature with the *new* key does not get to use it.
    const auto replacement = newKey();
    h->send(Message::approvalKey(h->next(), replacement->spki));
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalCancel), 1, 3000);
    const QByteArray fp = Identity::fingerprintOf(h->phone->peerPublicKey());
    h->send(Message::approvalResponse(
        h->next(), requestIdOf(request),
        sign(*replacement, approval::signingInput(challengeFrom(request), fp)), QString()));
    QTest::qWait(300);
    QCOMPARE(session->unlocks, 0);
    QVERIFY(!h->computer->allows(QString::fromLatin1(kPhoneId), Capability::Approval));
}

// ---- The PAM relay ----------------------------------------------------------

namespace {

struct RelayClient {
    QLocalSocket socket;
    QByteArray buffer;

    bool connectTo(const QString &path) {
        socket.connectToServer(path);
        return socket.waitForConnected(2000);
    }
    void sendLine(const QJsonObject &obj) {
        socket.write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
        socket.flush();
    }
    /// Next line, or empty on timeout/close.
    QByteArray readLine(int timeoutMs = 3000) {
        QElapsedTimer t;
        t.start();
        while (!buffer.contains('\n') && t.elapsed() < timeoutMs) {
            if (socket.bytesAvailable() == 0) {
                QTest::qWait(20);
            }
            buffer.append(socket.readAll());
            if (socket.state() != QLocalSocket::ConnectedState && socket.bytesAvailable() == 0
                && !buffer.contains('\n')) {
                break;
            }
        }
        const qsizetype nl = buffer.indexOf('\n');
        if (nl < 0) {
            return {};
        }
        const QByteArray line = buffer.left(nl);
        buffer.remove(0, nl + 1);
        return line;
    }
};

QJsonObject relayRequest(const QByteArray &nonce, qint64 issuedAt = 0) {
    return QJsonObject{
        {"v", 1},
        {"purpose", "sudo"},
        {"host", "maze"},
        {"user", currentUser()},
        {"detail", "sudo · pts/1"},
        {"nonce", QString::fromLatin1(nonce.toBase64())},
        {"issuedAt", static_cast<double>(issuedAt ? issuedAt
                                                  : QDateTime::currentSecsSinceEpoch())},
        {"timeoutMs", 10000},
    };
}

} // namespace

void TestApproval::relayRefusesStrangersAndNonsense() {
    QTemporaryDir runtime;
    ApprovalRelay relay;
    QVERIFY(relay.start(runtime.path(), nullptr));
    struct stat st {};
    QCOMPARE(::stat(QFile::encodeName(runtime.filePath("maze-connect")).constData(), &st), 0);
    QCOMPARE(st.st_mode & 0777, 0700u);

    // Default: root only. This test does not run as root.
    if (::getuid() != 0) {
        RelayClient client;
        QVERIFY(client.connectTo(relay.socketPath()));
        client.sendLine(relayRequest(QByteArray(32, 'n')));
        QVERIFY(client.readLine(1000).isEmpty());
    }

    relay.setPeerCheckForTesting([](uid_t) { return true; });
    QSignalSpy requested(&relay, &ApprovalRelay::requested);
    const auto refused = [&](const QJsonObject &request) {
        RelayClient client;
        if (!client.connectTo(relay.socketPath())) {
            return false;
        }
        client.sendLine(request);
        return client.readLine().startsWith("ERR ");
    };
    QJsonObject r = relayRequest(QByteArray(32, 'n'));
    r.insert("purpose", "unlock"); // never through PAM
    QVERIFY(refused(r));
    r = relayRequest(QByteArray(32, 'n'));
    r.insert("user", "root"); // not this user's phones
    QVERIFY(refused(r));
    QVERIFY(refused(relayRequest(QByteArray(31, 'n'))));
    QVERIFY(refused(relayRequest(QByteArray(32, 'n'), QDateTime::currentSecsSinceEpoch() - 3600)));
    r = relayRequest(QByteArray(32, 'n'));
    r.insert("detail", QStringLiteral("a\nb"));
    QVERIFY(refused(r));
    r = relayRequest(QByteArray(32, 'n'));
    r.insert("v", 2);
    QVERIFY(refused(r));
    r = relayRequest(QByteArray(32, 'n'));
    r.insert("timeoutMs", 3600000);
    QVERIFY(refused(r));
    QCOMPARE(requested.count(), 0);

    // An over-long line is dropped without an answer.
    RelayClient flood;
    QVERIFY(flood.connectTo(relay.socketPath()));
    flood.socket.write(QByteArray(ApprovalRelay::kMaxLineBytes + 10, 'x'));
    flood.socket.flush();
    QVERIFY(flood.readLine(1000).isEmpty());
    QCOMPARE(requested.count(), 0);

    // A directory that is not private is not used.
    QTemporaryDir open;
    QVERIFY(QDir(open.path()).mkdir("maze-connect"));
    QVERIFY(::chmod(QFile::encodeName(open.filePath("maze-connect")).constData(), 0755) == 0);
    ApprovalRelay second;
    QVERIFY(!second.start(open.path(), nullptr));
}

void TestApproval::relayCarriesTheSignatureAndTheVerdict() {
    const auto key = newKey();
    auto h = link(key->spki, true);
    QVERIFY(h);
    QTemporaryDir runtime;
    QVERIFY(h->computer->startApprovalRelay(runtime.path()));
    h->computer->approvalRelay()->setPeerCheckForTesting([](uid_t) { return true; });
    QSignalSpy handled(h->computer.get(), &DeviceManager::approvalHandled);

    RelayClient pam;
    QVERIFY(pam.connectTo(h->computer->approvalRelay()->socketPath()));
    const QByteArray nonce = approval::newNonce();
    const qint64 issuedAt = QDateTime::currentSecsSinceEpoch();
    pam.sendLine(relayRequest(nonce, issuedAt));

    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalRequest), 1, 3000);
    const Message request = h->last(MessageType::ApprovalRequest);
    QCOMPARE(request.string(QLatin1StringView("purpose"), 16), QStringLiteral("sudo"));
    QCOMPARE(request.boolean(QLatin1StringView("codeRequired")), true);
    // The code never left PAM, so it is not in the request.
    QVERIFY(!request.body().contains(QStringLiteral("code")));
    QCOMPARE(request.binary(QLatin1StringView("nonce"), 32), nonce);

    // The owner types the code shown on the terminal; the phone signs it.
    const QByteArray fp = Identity::fingerprintOf(h->phone->peerPublicKey());
    const QByteArray signature =
        sign(*key, approval::signingInput(challengeFrom(request, QStringLiteral("47")), fp));
    h->send(Message::approvalResponse(h->next(), requestIdOf(request), signature, QString()));

    const QList<QByteArray> answer = pam.readLine().split(' ');
    QCOMPARE(answer.size(), 3);
    QCOMPARE(answer.at(0), QByteArray("OK"));
    QCOMPARE(QByteArray::fromBase64(answer.at(1)), signature);
    const QByteArray verifier = QByteArray::fromBase64(answer.at(2));
    QCOMPARE(verifier, fp);

    // What PAM does with it: verify against the code it showed.
    approval::Challenge mine;
    mine.purpose = approval::Purpose::Sudo;
    mine.host = QStringLiteral("maze");
    mine.user = currentUser();
    mine.detail = QStringLiteral("sudo · pts/1");
    mine.nonce = nonce;
    mine.code = QStringLiteral("47");
    mine.issuedAt = issuedAt;
    QVERIFY(approval::verify(key->spki, approval::signingInput(mine, verifier), signature));
    mine.code = QStringLiteral("48");
    QVERIFY(!approval::verify(key->spki, approval::signingInput(mine, verifier), signature));

    pam.sendLine(QJsonObject{{"v", 1}, {"verified", true}});
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalResult), 1, 3000);
    QCOMPARE(h->last(MessageType::ApprovalResult).boolean(QLatin1StringView("ok")), true);
    QCOMPARE(handled.last().at(1).toString(), QStringLiteral("sudo"));

    // PAM going away mid-wait closes the challenge on the phone.
    RelayClient gone;
    QVERIFY(gone.connectTo(h->computer->approvalRelay()->socketPath()));
    gone.sendLine(relayRequest(approval::newNonce()));
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalRequest), 2, 3000);
    gone.socket.abort();
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalCancel), 1, 3000);
}

void TestApproval::relayAnswersAtOnceWithNoPhone() {
    // A key on record, but approval not switched on for it.
    const auto key = newKey();
    auto h = link(key->spki, false);
    QVERIFY(h);
    QTemporaryDir runtime;
    QVERIFY(h->computer->startApprovalRelay(runtime.path()));
    h->computer->approvalRelay()->setPeerCheckForTesting([](uid_t) { return true; });

    RelayClient pam;
    QVERIFY(pam.connectTo(h->computer->approvalRelay()->socketPath()));
    QElapsedTimer t;
    t.start();
    pam.sendLine(relayRequest(approval::newNonce()));
    const QByteArray answer = pam.readLine();
    QVERIFY(answer.startsWith("ERR "));
    // Text a terminal can show as it is.
    QVERIFY(std::all_of(answer.cbegin(), answer.cend(),
                        [](char c) { return c >= 0x20 && c <= 0x7E; }));
    // Straight to the password, not after a timeout.
    QVERIFY(t.elapsed() < 2000);
    QCOMPARE(h->count(MessageType::ApprovalRequest), 0);
}

void TestApproval::lockPhoneNeedsARequest() {
    auto h = link();
    QVERIFY(h);
    QSignalSpy answered(h->computer.get(), &DeviceManager::phoneLockAnswered);
    h->send(Message::lockPhoneResult(h->next(), true, QString()));
    QTest::qWait(300);
    QCOMPARE(answered.count(), 0);

    const QString id = QString::fromLatin1(kPhoneId);
    QVERIFY(h->computer->lockPhone(id));
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::LockPhone), 1, 3000);
    h->send(Message::lockPhoneResult(h->next(), true, QString()));
    QTRY_COMPARE_WITH_TIMEOUT(answered.count(), 1, 3000);
    QCOMPARE(answered.at(0).at(1).toBool(), true);

    // Bounded, and switchable off.
    for (int i = 0; i < limits::kMaxPhoneLocksPerMinute; ++i) {
        h->computer->lockPhone(id);
    }
    QVERIFY(!h->computer->lockPhone(id));
    QVERIFY(h->computer->setCapabilityEnabled(id, Capability::LockPhone, false));
    QVERIFY(!h->computer->lockPhone(id));
}

void TestApproval::onePhoneCannotAnswerForAnother() {
    // Two phones, both allowed, both with keys. Phone 1 asks to unlock; phone
    // 2 — paired, trusted, holding a valid key of its own — must not be able
    // to answer phone 1's challenge, even with a signature that would verify
    // under its own key.
    const auto key = newKey();
    const auto otherKey = newKey();
    auto h = link(key->spki, true, otherKey->spki);
    QVERIFY(h);
    auto *session = new FakeSession;
    h->computer->setSessionControlForTesting(session);

    h->send(Message::unlockRequest(h->next()));
    QTRY_COMPARE_WITH_TIMEOUT(h->count(MessageType::ApprovalRequest), 1, 3000);
    const Message request = h->last(MessageType::ApprovalRequest);
    QVERIFY(std::none_of(h->otherReceived.cbegin(), h->otherReceived.cend(),
                         [](const Message &m) { return m.type() == MessageType::ApprovalRequest; }));

    const QByteArray fp = Identity::fingerprintOf(h->other->peerPublicKey());
    h->other->send(Message::approvalResponse(
        ++h->otherCounter, requestIdOf(request),
        sign(*otherKey, approval::signingInput(challengeFrom(request), fp)), QString()));
    QTest::qWait(400);
    QCOMPARE(session->unlocks, 0);

    // Nor can it decline it on phone 1's behalf: the challenge is still open,
    // and phone 1's own answer still works.
    h->other->send(Message::approvalResponse(++h->otherCounter, requestIdOf(request),
                                             QByteArray(), QStringLiteral("declined")));
    QTest::qWait(200);
    h->send(Message::approvalResponse(
        h->next(), requestIdOf(request),
        sign(*key, approval::signingInput(challengeFrom(request), fp)), QString()));
    QTRY_COMPARE_WITH_TIMEOUT(session->unlocks, 1, 3000);
}

QTEST_MAIN(TestApproval)
#include "tst_approval.moc"
