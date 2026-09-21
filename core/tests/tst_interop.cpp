#include <QtTest>

#include <QJsonDocument>
#include <QJsonObject>

#include "mazeconnect/core/Framing.h"
#include "mazeconnect/core/GuardBridge.h"
#include "mazeconnect/core/Messages.h"
#include "mazeconnect/core/Sas.h"

using namespace mazeconnect::core;

/**
 * Golden vectors shared with the mobile client.
 *
 * Every assertion here is duplicated byte-for-byte in
 * Maze-Connect-Mobile's `InteropTest.kt`. The two clients are separate
 * implementations of one wire format, so the realistic failure is not a
 * crash — it is a silent drift (a renamed envelope key, a different base64
 * variant, an off-by-one in the frame header) that makes pairing fail in
 * the field with no useful error.
 *
 * These tests are the contract. Changing a vector here without changing it
 * in the Kotlin file is a protocol break, and the paired test will fail.
 */
class TestInterop : public QObject {
    Q_OBJECT

private slots:
    void frameHeaderLayout();
    void controlEnvelopeKeys();
    void pairRequestNonceEncoding();
    void dataFrameTransferIdLayout();
    void counterZeroIsRejected();
    void versionMismatchIsRejected();
    void sasKnownAnswer();
    void statusMessageShape();
    void capabilityWireNames();
    void commandMessageShape();
    void aiMessageShape();
    void guardMessageShape();
};

void TestInterop::frameHeaderLayout() {
    // [1 byte type][4 bytes big-endian length][payload]
    const QByteArray frame = FrameParser::encode(FrameType::Control, QByteArrayLiteral("ab"));
    QCOMPARE(frame.size(), qsizetype(7));
    QCOMPARE(static_cast<quint8>(frame[0]), quint8(0x01)); // Control
    QCOMPARE(static_cast<quint8>(frame[1]), quint8(0x00));
    QCOMPARE(static_cast<quint8>(frame[2]), quint8(0x00));
    QCOMPARE(static_cast<quint8>(frame[3]), quint8(0x00));
    QCOMPARE(static_cast<quint8>(frame[4]), quint8(0x02));
    QCOMPARE(frame.mid(5), QByteArrayLiteral("ab"));

    const QByteArray data = FrameParser::encode(FrameType::Data, QByteArray());
    QCOMPARE(static_cast<quint8>(data[0]), quint8(0x02)); // Data

    // A length that spans more than one byte must be big-endian.
    const QByteArray wide = FrameParser::encode(FrameType::Data, QByteArray(258, 'x'));
    QCOMPARE(static_cast<quint8>(wide[3]), quint8(0x01));
    QCOMPARE(static_cast<quint8>(wide[4]), quint8(0x02));
}

void TestInterop::controlEnvelopeKeys() {
    // The envelope is "v"/"t"/"c". A rename on one side alone would make
    // every message from that side unparseable on the other.
    Message ping = Message::unpair(42);
    const QJsonObject obj = QJsonDocument::fromJson(ping.toJson()).object();

    QCOMPARE(obj.value(QLatin1StringView("v")).toInt(), 4);
    QCOMPARE(obj.value(QLatin1StringView("t")).toString(), QStringLiteral("unpair"));
    QCOMPARE(obj.value(QLatin1StringView("c")).toDouble(), 42.0);

    // And the type names on the wire.
    QCOMPARE(Message::typeName(MessageType::Hello), QStringLiteral("hello"));
    QCOMPARE(Message::typeName(MessageType::PairRequest), QStringLiteral("pairRequest"));
    QCOMPARE(Message::typeName(MessageType::PairResponse), QStringLiteral("pairResponse"));
    QCOMPARE(Message::typeName(MessageType::PairReveal), QStringLiteral("pairReveal"));
    QCOMPARE(Message::typeName(MessageType::PairResult), QStringLiteral("pairResult"));
    QCOMPARE(Message::typeName(MessageType::FileOffer), QStringLiteral("fileOffer"));
}

void TestInterop::pairRequestNonceEncoding() {
    // Standard base64, no line wrapping. Android's Base64.NO_WRAP must
    // agree with Qt's toBase64() here or pairing cannot complete.
    QByteArray nonce(Sas::kNonceSize, Qt::Uninitialized);
    for (int i = 0; i < Sas::kNonceSize; ++i) {
        nonce[i] = static_cast<char>(i);
    }

    // PairRequest carries the COMMITMENT, never the nonce. If either client
    // ever puts the nonce back in this message the commitment round is gone
    // and the on-screen code stops meaning anything (Sas.h), so the absence
    // of the key is asserted as strictly as its presence.
    Message request = Message::pairRequest(1, Sas::commit(nonce));
    const QJsonObject reqObj = QJsonDocument::fromJson(request.toJson()).object();
    QVERIFY(!reqObj.contains(QLatin1StringView("nonce")));
    QCOMPARE(reqObj.value(QLatin1StringView("commitment")).toString(),
             QStringLiteral("Jz2nu+C6nOgcIlgQwwHoK6148NvbSJrusnW6o8PaWj4="));

    // The nonce itself travels in PairReveal, base64 the same way.
    Message reveal = Message::pairReveal(2, nonce);
    const QJsonObject revObj = QJsonDocument::fromJson(reveal.toJson()).object();
    QCOMPARE(revObj.value(QLatin1StringView("nonce")).toString(),
             QStringLiteral("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="));

    // And it survives a round trip through the validated accessor.
    Message parsed = Message::parse(reveal.toJson());
    QVERIFY(parsed.isValid());
    QCOMPARE(parsed.binary(QLatin1StringView("nonce"), Sas::kNonceSize), nonce);

    // Cross-platform known answer for the commitment, asserted on both
    // clients: a drift in the context string or the length prefix would
    // otherwise show up only as pairings that silently fail in the field.
    Message parsedRequest = Message::parse(request.toJson());
    QVERIFY(parsedRequest.isValid());
    QVERIFY(Sas::verifyCommitment(
        parsedRequest.binary(QLatin1StringView("commitment"), Sas::kCommitSize), nonce));
}

void TestInterop::dataFrameTransferIdLayout() {
    // [4 bytes big-endian transferId][chunk]
    const QByteArray encoded = datachunk::encode(0x01020304, QByteArrayLiteral("hi"));
    QCOMPARE(encoded.size(), qsizetype(6));
    QCOMPARE(static_cast<quint8>(encoded[0]), quint8(0x01));
    QCOMPARE(static_cast<quint8>(encoded[1]), quint8(0x02));
    QCOMPARE(static_cast<quint8>(encoded[2]), quint8(0x03));
    QCOMPARE(static_cast<quint8>(encoded[3]), quint8(0x04));
    QCOMPARE(encoded.mid(4), QByteArrayLiteral("hi"));

    quint32 id = 0;
    QByteArray chunk;
    QVERIFY(datachunk::decode(encoded, id, chunk));
    QCOMPARE(id, 0x01020304u);
    QCOMPARE(chunk, QByteArrayLiteral("hi"));

    // Too short to carry an id.
    QVERIFY(!datachunk::decode(QByteArrayLiteral("abc"), id, chunk));
}

void TestInterop::counterZeroIsRejected() {
    // Both sides reserve 0 so an absent or zeroed counter is never a valid
    // first message.
    const QByteArray zero = QByteArrayLiteral(R"({"v":4,"t":"unpair","c":0})");
    QVERIFY(!Message::parse(zero).isValid());

    const QByteArray missing = QByteArrayLiteral(R"({"v":4,"t":"unpair"})");
    QVERIFY(!Message::parse(missing).isValid());

    const QByteArray negative = QByteArrayLiteral(R"({"v":4,"t":"unpair","c":-1})");
    QVERIFY(!Message::parse(negative).isValid());

    const QByteArray valid = QByteArrayLiteral(R"({"v":4,"t":"unpair","c":1})");
    QVERIFY(Message::parse(valid).isValid());
}

void TestInterop::versionMismatchIsRejected() {
    // No negotiation to an older dialect: a mismatch is a hard stop.
    QVERIFY(!Message::parse(QByteArrayLiteral(R"({"v":5,"t":"unpair","c":1})")).isValid());
    // v3 is the version that sent the nonce in the clear. Refusing it is the
    // point of the bump: negotiating down would restore the grinding attack.
    QVERIFY(!Message::parse(QByteArrayLiteral(R"({"v":3,"t":"unpair","c":1})")).isValid());
    QVERIFY(!Message::parse(QByteArrayLiteral(R"({"t":"unpair","c":1})")).isValid());
    QVERIFY(!Message::parse(QByteArrayLiteral(R"({"v":4,"t":"nope","c":1})")).isValid());
}

void TestInterop::sasKnownAnswer() {
    // Duplicated in SasTest.matchesKnownAnswerFromSharedConstruction.
    QByteArray a(91, Qt::Uninitialized);
    QByteArray b(91, Qt::Uninitialized);
    for (int i = 0; i < 91; ++i) {
        a[i] = static_cast<char>(i);
        b[i] = static_cast<char>(i + 100);
    }
    QCOMPARE(Sas::derive(a, b, QByteArray(Sas::kNonceSize, char(1)),
                         QByteArray(Sas::kNonceSize, char(2))),
             QStringLiteral("876154"));
}

void TestInterop::statusMessageShape() {
    // Duplicated in InteropTest.statusMessageShape.
    QCOMPARE(Message::typeName(MessageType::StatusRequest), QStringLiteral("statusRequest"));
    QCOMPARE(Message::typeName(MessageType::StatusReport), QStringLiteral("statusReport"));

    // A request carries nothing but the envelope. The phone cannot ask for a
    // particular probe, and nothing it sends reaches the status helper.
    const QJsonObject request = QJsonDocument::fromJson(Message::statusRequest(7).toJson()).object();
    QCOMPARE(request.keys(), QStringList({QStringLiteral("c"), QStringLiteral("t"),
                                          QStringLiteral("v")}));

    // The snapshot is nested under "status", never flattened into the
    // envelope: a future field of it must not be able to collide with "v",
    // "t" or "c".
    QJsonObject snapshot;
    snapshot.insert(QLatin1StringView("hostname"), QStringLiteral("box"));
    const QJsonObject report =
        QJsonDocument::fromJson(Message::statusReport(8, snapshot).toJson()).object();
    QCOMPARE(report.value(QLatin1StringView("t")).toString(), QStringLiteral("statusReport"));
    QCOMPARE(report.value(QLatin1StringView("status")).toObject()
                 .value(QLatin1StringView("hostname")).toString(),
             QStringLiteral("box"));
    QVERIFY(!report.contains(QLatin1StringView("error")));

    // Failure travels as the same type with "error" instead of "status", so a
    // client has one message to handle rather than two.
    const QJsonObject unavailable =
        QJsonDocument::fromJson(Message::statusUnavailable(9, QStringLiteral("no maze-tools"))
                                    .toJson())
            .object();
    QCOMPARE(unavailable.value(QLatin1StringView("t")).toString(), QStringLiteral("statusReport"));
    QCOMPARE(unavailable.value(QLatin1StringView("error")).toString(),
             QStringLiteral("no maze-tools"));
    QVERIFY(!unavailable.contains(QLatin1StringView("status")));

    // "nothing changed" is the envelope and nothing else. A dashboard polls
    // every few seconds and an idle machine reads the same each time; this is
    // what stops that costing a fresh copy of the snapshot every poll.
    QCOMPARE(Message::typeName(MessageType::StatusUnchanged),
             QStringLiteral("statusUnchanged"));
    const QJsonObject unchanged =
        QJsonDocument::fromJson(Message::statusUnchanged(10).toJson()).object();
    QCOMPARE(unchanged.keys(), QStringList({QStringLiteral("c"), QStringLiteral("t"),
                                            QStringLiteral("v")}));
}

void TestInterop::capabilityWireNames() {
    // Capability names are matched as strings across the two clients; a
    // rename on one side alone silently disables the feature rather than
    // failing, which is why they are pinned here.
    QCOMPARE(capabilityName(Capability::FileTransfer), QStringLiteral("fileTransfer"));
    QCOMPARE(capabilityName(Capability::SystemStatus), QStringLiteral("systemStatus"));
    QCOMPARE(capabilityFromName(QStringLiteral("systemStatus")), Capability::SystemStatus);

    // An unknown name is not guessed at. Deliberately a name with no plans
    // behind it: "guardControl" was used here once and then became real,
    // which is how this assertion caught itself going stale.
    QCOMPARE(capabilityFromName(QStringLiteral("teleport")), Capability::None);
    QCOMPARE(capabilityFromName(QString()), Capability::None);
    QCOMPARE(capabilityFromName(QStringLiteral("commands ")), Capability::None);

    // Pairing grants everything this build implements. The two sets are still
    // distinct concepts — advertising is "I implement this", enabling is "you
    // may use it" — but they now start equal, because pairing is already the
    // deliberate act and a second wall in front of every feature only made
    // the app look broken.
    QVERIFY(supportedCapabilities().testFlag(Capability::SystemStatus));
    QVERIFY(defaultEnabledCapabilities().testFlag(Capability::SystemStatus));
    QCOMPARE(defaultEnabledCapabilities(), supportedCapabilities());

    // Computer -> phone only, but still matched as a string like every
    // other capability, and still granted by pairing like the rest.
    QCOMPARE(capabilityName(Capability::OpenOnPhone), QStringLiteral("openOnPhone"));
    QCOMPARE(capabilityFromName(QStringLiteral("openOnPhone")), Capability::OpenOnPhone);
    QVERIFY(supportedCapabilities().testFlag(Capability::OpenOnPhone));
    QVERIFY(defaultEnabledCapabilities().testFlag(Capability::OpenOnPhone));

    QCOMPARE(Message::typeName(MessageType::OpenOnPhone), QStringLiteral("openOnPhone"));
    const QJsonObject openOnPhone =
        QJsonDocument::fromJson(Message::openOnPhone(1, QStringLiteral("https://example.com")).toJson())
            .object();
    QCOMPARE(openOnPhone.value(QLatin1StringView("text")).toString(),
             QStringLiteral("https://example.com"));
}

void TestInterop::commandMessageShape() {
    // Duplicated in InteropTest.commandMessageShape.
    QCOMPARE(Message::typeName(MessageType::CommandList), QStringLiteral("commandList"));
    QCOMPARE(Message::typeName(MessageType::CommandCatalog), QStringLiteral("commandCatalog"));
    QCOMPARE(Message::typeName(MessageType::CommandRun), QStringLiteral("commandRun"));
    QCOMPARE(Message::typeName(MessageType::CommandResult), QStringLiteral("commandResult"));

    // The catalogue carries id, label and confirm — and never argv. This is
    // the single most important shape in this feature: the phone is not told
    // what a command *is*, only what it is called.
    QJsonObject entry;
    entry.insert(QLatin1StringView("id"), QStringLiteral("lock"));
    entry.insert(QLatin1StringView("label"), QStringLiteral("Lock screen"));
    entry.insert(QLatin1StringView("confirm"), false);
    QJsonArray entries;
    entries.append(entry);

    const QJsonObject catalog =
        QJsonDocument::fromJson(Message::commandCatalog(3, entries).toJson()).object();
    const QJsonObject row = catalog.value(QLatin1StringView("commands")).toArray().at(0).toObject();
    QCOMPARE(row.value(QLatin1StringView("id")).toString(), QStringLiteral("lock"));
    QVERIFY2(!row.contains(QLatin1StringView("argv")),
             "the catalogue must never carry the command line");

    // A run is an id and a request id. There is no argv field to fill in.
    const QJsonObject run =
        QJsonDocument::fromJson(Message::commandRun(4, 9, QStringLiteral("lock")).toJson())
            .object();
    QCOMPARE(run.keys(), QStringList({QStringLiteral("c"), QStringLiteral("id"),
                                      QStringLiteral("requestId"), QStringLiteral("t"),
                                      QStringLiteral("v")}));

    // A refusal and a failed run are different answers, carried by the same
    // type: "error" means it never started.
    const QJsonObject refused =
        QJsonDocument::fromJson(Message::commandRefused(5, 9, QStringLiteral("no such command"))
                                    .toJson())
            .object();
    QCOMPARE(refused.value(QLatin1StringView("t")).toString(), QStringLiteral("commandResult"));
    QVERIFY(refused.contains(QLatin1StringView("error")));
    QVERIFY(!refused.contains(QLatin1StringView("exitCode")));

    const QJsonObject result =
        QJsonDocument::fromJson(
            Message::commandResult(6, 9, QStringLiteral("lock"), 0, QStringLiteral("ok")).toJson())
            .object();
    QCOMPARE(result.value(QLatin1StringView("exitCode")).toInt(), 0);
    QVERIFY(!result.contains(QLatin1StringView("error")));
}

void TestInterop::aiMessageShape() {
    // Duplicated in InteropTest.aiMessageShape.
    QCOMPARE(Message::typeName(MessageType::AiModels), QStringLiteral("aiModels"));
    QCOMPARE(Message::typeName(MessageType::AiModelList), QStringLiteral("aiModelList"));
    QCOMPARE(Message::typeName(MessageType::AiPrompt), QStringLiteral("aiPrompt"));
    QCOMPARE(Message::typeName(MessageType::AiChunk), QStringLiteral("aiChunk"));
    QCOMPARE(Message::typeName(MessageType::AiDone), QStringLiteral("aiDone"));

    // A prompt is one line of text plus which model to use. There is no
    // history field and no system prompt: the conversation lives on the
    // computer, so a phone cannot rewrite what was said earlier or replace
    // Maze AI's persona.
    const QJsonObject prompt =
        QJsonDocument::fromJson(
            Message::aiPrompt(1, 7, QStringLiteral("gemma3"), QStringLiteral("hi")).toJson())
            .object();
    QCOMPARE(prompt.keys(), QStringList({QStringLiteral("c"), QStringLiteral("model"),
                                         QStringLiteral("requestId"), QStringLiteral("t"),
                                         QStringLiteral("text"), QStringLiteral("v")}));
    QVERIFY2(!prompt.contains(QLatin1StringView("messages")), "a prompt carried history");
    QVERIFY2(!prompt.contains(QLatin1StringView("system")), "a prompt carried a system prompt");
    QVERIFY2(!prompt.contains(QLatin1StringView("tools")), "a prompt offered tools");

    // Chunks are ordinary control frames — they carry the counter like
    // everything else, so replay protection and size caps apply unchanged.
    const QJsonObject chunk =
        QJsonDocument::fromJson(Message::aiChunk(2, 7, QStringLiteral("Hel")).toJson()).object();
    QCOMPARE(chunk.value(QLatin1StringView("c")).toDouble(), 2.0);
    QCOMPARE(chunk.value(QLatin1StringView("text")).toString(), QStringLiteral("Hel"));

    // aiDone carries "error" only when there is no answer at all.
    const QJsonObject ok =
        QJsonDocument::fromJson(Message::aiDone(3, 7, QString()).toJson()).object();
    QVERIFY(!ok.contains(QLatin1StringView("error")));
    const QJsonObject bad =
        QJsonDocument::fromJson(Message::aiDone(4, 7, QStringLiteral("no model")).toJson())
            .object();
    QCOMPARE(bad.value(QLatin1StringView("error")).toString(), QStringLiteral("no model"));

    QCOMPARE(capabilityName(Capability::Ai), QStringLiteral("ai"));
    QVERIFY(supportedCapabilities().testFlag(Capability::Ai));

    // The privileged one. Granted by pairing like the rest — but it is still
    // revocable, and what actually constrains it lives elsewhere: PANIC and
    // RESTORE have no representation in the protocol (tst_guardbridge), the
    // device table is a fixed enum, and every change is logged and announced.
    QCOMPARE(capabilityName(Capability::GuardControl), QStringLiteral("guardControl"));
    QCOMPARE(capabilityFromName(QStringLiteral("guardControl")), Capability::GuardControl);
    QVERIFY(supportedCapabilities().testFlag(Capability::GuardControl));
    QVERIFY(defaultEnabledCapabilities().testFlag(Capability::GuardControl));
}

void TestInterop::guardMessageShape() {
    // Duplicated in InteropTest.guardMessageShape.
    QCOMPARE(Message::typeName(MessageType::GuardStatus), QStringLiteral("guardStatus"));
    QCOMPARE(Message::typeName(MessageType::GuardReport), QStringLiteral("guardReport"));
    QCOMPARE(Message::typeName(MessageType::GuardRequest), QStringLiteral("guardRequest"));
    QCOMPARE(Message::typeName(MessageType::GuardResult), QStringLiteral("guardResult"));

    // A request is a device name and a boolean. There is no verb field, and
    // that is the point: PANIC and RESTORE have nowhere to be written even by
    // a peer that knows they exist on the far side.
    const QJsonObject request =
        QJsonDocument::fromJson(Message::guardRequest(1, QStringLiteral("camera"), true).toJson())
            .object();
    QCOMPARE(request.keys(), QStringList({QStringLiteral("c"), QStringLiteral("device"),
                                          QStringLiteral("on"), QStringLiteral("t"),
                                          QStringLiteral("v")}));
    QVERIFY2(!request.contains(QLatin1StringView("verb")), "a request could name a verb");
    QVERIFY2(!request.contains(QLatin1StringView("command")), "a request could carry a command");

    // The device vocabulary, matched exactly against maze-guardd's own.
    GuardDevice device{};
    QVERIFY(guardDeviceFromName(QStringLiteral("camera"), device));
    QVERIFY(guardDeviceFromName(QStringLiteral("microphone"), device));
    QVERIFY(guardDeviceFromName(QStringLiteral("bluetooth"), device));
    QVERIFY(guardDeviceFromName(QStringLiteral("wifi"), device));
    QVERIFY(guardDeviceFromName(QStringLiteral("usb"), device));
    QVERIFY2(!guardDeviceFromName(QStringLiteral("everything"), device),
             "a device name outside the table was accepted");

    const QJsonObject result =
        QJsonDocument::fromJson(
            Message::guardResult(2, QStringLiteral("wifi"), QStringLiteral("off"), QString())
                .toJson())
            .object();
    QCOMPARE(result.value(QLatin1StringView("state")).toString(), QStringLiteral("off"));
    QVERIFY(!result.contains(QLatin1StringView("error")));
}

QTEST_MAIN(TestInterop)
#include "tst_interop.moc"
