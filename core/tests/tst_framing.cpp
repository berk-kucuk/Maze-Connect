#include <QtTest>

#include "mazeconnect/core/Framing.h"
#include "mazeconnect/core/Limits.h"

using namespace mazeconnect::core;

class TestFraming : public QObject {
    Q_OBJECT

private slots:
    void roundTrip();
    void splitAcrossReads();
    void multipleFramesInOneRead();
    void rejectsOversizedLengthPrefixWithoutBuffering();
    void rejectsUnknownType();
    void staysPoisonedAfterError();
    void encodeRefusesOversizedPayload();
    void emptyPayloadIsValid();
};

void TestFraming::roundTrip() {
    const QByteArray payload = QByteArrayLiteral("{\"type\":\"ping\"}");
    const QByteArray frame = FrameParser::encode(FrameType::Control, payload);
    QVERIFY(!frame.isEmpty());

    FrameParser parser;
    parser.append(frame);

    FrameType type{};
    QByteArray out;
    QCOMPARE(parser.next(type, out), FrameParser::Status::Ready);
    QCOMPARE(type, FrameType::Control);
    QCOMPARE(out, payload);
    QCOMPARE(parser.next(type, out), FrameParser::Status::Incomplete);
}

void TestFraming::splitAcrossReads() {
    const QByteArray payload(1000, 'x');
    const QByteArray frame = FrameParser::encode(FrameType::Data, payload);

    FrameParser parser;
    FrameType type{};
    QByteArray out;

    // Feed one byte at a time; only the final byte may complete the frame.
    for (qsizetype i = 0; i < frame.size() - 1; ++i) {
        parser.append(frame.mid(i, 1));
        QCOMPARE(parser.next(type, out), FrameParser::Status::Incomplete);
    }
    parser.append(frame.right(1));
    QCOMPARE(parser.next(type, out), FrameParser::Status::Ready);
    QCOMPARE(out, payload);
}

void TestFraming::multipleFramesInOneRead() {
    QByteArray stream;
    stream += FrameParser::encode(FrameType::Control, QByteArrayLiteral("a"));
    stream += FrameParser::encode(FrameType::Data, QByteArrayLiteral("bb"));
    stream += FrameParser::encode(FrameType::Control, QByteArrayLiteral("ccc"));

    FrameParser parser;
    parser.append(stream);

    FrameType type{};
    QByteArray out;
    QCOMPARE(parser.next(type, out), FrameParser::Status::Ready);
    QCOMPARE(out, QByteArrayLiteral("a"));
    QCOMPARE(parser.next(type, out), FrameParser::Status::Ready);
    QCOMPARE(type, FrameType::Data);
    QCOMPARE(out, QByteArrayLiteral("bb"));
    QCOMPARE(parser.next(type, out), FrameParser::Status::Ready);
    QCOMPARE(out, QByteArrayLiteral("ccc"));
    QCOMPARE(parser.next(type, out), FrameParser::Status::Incomplete);
}

void TestFraming::rejectsOversizedLengthPrefixWithoutBuffering() {
    // A peer claiming ~4 GiB must cost us the 5-byte header and nothing else.
    QByteArray hostile;
    hostile.append(static_cast<char>(FrameType::Control));
    hostile.append(char(0xFF));
    hostile.append(char(0xFF));
    hostile.append(char(0xFF));
    hostile.append(char(0xFF));

    FrameParser parser;
    parser.append(hostile);

    FrameType type{};
    QByteArray out;
    QCOMPARE(parser.next(type, out), FrameParser::Status::Error);
    QVERIFY(parser.errorString().contains(QStringLiteral("exceeds cap")));
    // Nothing retained after the failure.
    QCOMPARE(parser.bufferedBytes(), qsizetype(0));
}

void TestFraming::rejectsUnknownType() {
    QByteArray hostile;
    hostile.append(char(0x7F)); // not Control or Data
    hostile.append(char(0x00));
    hostile.append(char(0x00));
    hostile.append(char(0x00));
    hostile.append(char(0x01));
    hostile.append('z');

    FrameParser parser;
    parser.append(hostile);

    FrameType type{};
    QByteArray out;
    QCOMPARE(parser.next(type, out), FrameParser::Status::Error);
}

void TestFraming::staysPoisonedAfterError() {
    FrameParser parser;
    parser.append(QByteArray(5, char(0x7F))); // bad type

    FrameType type{};
    QByteArray out;
    QCOMPARE(parser.next(type, out), FrameParser::Status::Error);

    // Even a perfectly valid frame afterwards must not be accepted: once
    // framing is violated there is no safe resync point.
    parser.append(FrameParser::encode(FrameType::Control, QByteArrayLiteral("ok")));
    QCOMPARE(parser.next(type, out), FrameParser::Status::Error);
}

void TestFraming::encodeRefusesOversizedPayload() {
    const QByteArray tooBig(limits::kMaxDataFrame + 1, 'x');
    QVERIFY(FrameParser::encode(FrameType::Data, tooBig).isEmpty());

    // And the exact cap is still allowed.
    const QByteArray atCap(limits::kMaxDataFrame, 'x');
    QVERIFY(!FrameParser::encode(FrameType::Data, atCap).isEmpty());
}

void TestFraming::emptyPayloadIsValid() {
    const QByteArray frame = FrameParser::encode(FrameType::Control, QByteArray());
    QCOMPARE(frame.size(), qsizetype(5));

    FrameParser parser;
    parser.append(frame);
    FrameType type{};
    QByteArray out;
    QCOMPARE(parser.next(type, out), FrameParser::Status::Ready);
    QVERIFY(out.isEmpty());
}

QTEST_MAIN(TestFraming)
#include "tst_framing.moc"
