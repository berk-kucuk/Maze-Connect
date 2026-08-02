#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include "mazeconnect/core/AiBridge.h"

using namespace mazeconnect::core;

/**
 * AiBridge against a stand-in Ollama.
 *
 * A real Ollama is not a dependency of this suite, and the interesting cases
 * are ones a healthy Ollama never produces anyway: a stream that stops
 * halfway, a line split across two packets, a model name a peer invented.
 *
 * The load-bearing assertion is `neverOffersTools`. Maze AI proper runs shell
 * commands through a JSON tool protocol; this bridge must not, and "must not"
 * has to be checked against the bytes actually sent rather than trusted to
 * stay true as the file is edited.
 */
class TestAiBridge : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void listsModels();
    void reportsAnUnreachableOllama();
    void reportsAnOllamaWithNoModels();
    void streamsNdjsonChunks();
    void splitsLinesAcrossPackets();
    void neverOffersTools();
    void sendsTheChatOnlyPersona();
    void refusesAModelItWasNotTold();
    void refusesAnEmptyOrOversizedPrompt();
    void keepsHistoryPerConversation();
    void carriesHistoryForwardWithinAConversation();
    void aTruncatedStreamStillCompletes();

private:
    /**
     * Enough HTTP for these tests, routed on the path.
     *
     * /api/tags and /api/chat answer with different shapes, so a single canned
     * reply cannot exercise the streaming path — which is the part most worth
     * testing.
     */
    void serve();

    /// Get past listModels() so ask() has a model it will accept.
    void primeModels(AiBridge &bridge);

    QTcpServer m_server;
    QByteArray m_tagsReply;
    QByteArray m_chatReply;
    /// Write the chat body in two parts, to split a JSON line across packets.
    bool m_splitChatWrite = false;
    QByteArray m_lastRequestBody;
};

void TestAiBridge::init() {
    QVERIFY(m_server.listen(QHostAddress::LocalHost, 0));
    qputenv("MAZECONNECT_OLLAMA_HOST",
            QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()).toUtf8());
    m_tagsReply = R"({"models":[{"name":"m"}]})";
    m_chatReply.clear();
    m_splitChatWrite = false;
    m_lastRequestBody.clear();
    serve();
}

void TestAiBridge::cleanup() {
    qunsetenv("MAZECONNECT_OLLAMA_HOST");
    m_server.close();
}

void TestAiBridge::serve() {
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
        QTcpSocket *socket = m_server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            const QByteArray request = socket->readAll();
            const bool isChat = request.startsWith("POST /api/chat");
            const int split = request.indexOf("\r\n\r\n");
            if (split >= 0) {
                m_lastRequestBody = request.mid(split + 4);
            }

            const QByteArray body = isChat ? m_chatReply : m_tagsReply;
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n");
            if (isChat) {
                // No Content-Length: /api/chat is a stream, and that is how
                // Ollama really answers it.
                socket->write("Connection: close\r\n\r\n");
                if (m_splitChatWrite && body.size() > 8) {
                    socket->write(body.left(body.size() / 2));
                    socket->flush();
                    QTest::qWait(60);
                    socket->write(body.mid(body.size() / 2));
                } else {
                    socket->write(body);
                }
            } else {
                socket->write(
                    QStringLiteral("Content-Length: %1\r\n\r\n").arg(body.size()).toUtf8());
                socket->write(body);
            }
            socket->flush();
            socket->disconnectFromHost();
        });
    });
}

void TestAiBridge::primeModels(AiBridge &bridge) {
    QSignalSpy ready(&bridge, &AiBridge::modelsReady);
    bridge.listModels();
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1, 5000);
    m_lastRequestBody.clear();
}

void TestAiBridge::listsModels() {
    m_tagsReply = R"({"models":[{"name":"gemma3:latest"},{"name":"llama3:8b"}]})";

    AiBridge bridge;
    QSignalSpy ready(&bridge, &AiBridge::modelsReady);
    bridge.listModels();

    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1, 5000);
    QCOMPARE(ready.at(0).at(0).toStringList(),
             QStringList({QStringLiteral("gemma3:latest"), QStringLiteral("llama3:8b")}));
}

void TestAiBridge::reportsAnUnreachableOllama() {
    // Nothing listening at all: by far the most common real failure, and the
    // one the user can actually act on.
    m_server.close();

    AiBridge bridge;
    QSignalSpy failed(&bridge, &AiBridge::modelsFailed);
    bridge.listModels();

    QTRY_VERIFY_WITH_TIMEOUT(failed.count() == 1, 10000);
    QVERIFY(failed.at(0).at(0).toString().contains(QStringLiteral("not reachable")));
}

void TestAiBridge::reportsAnOllamaWithNoModels() {
    // Running but empty is a different problem with a different fix, so it
    // gets its own message rather than being folded into "unreachable".
    m_tagsReply = R"({"models":[]})";

    AiBridge bridge;
    QSignalSpy failed(&bridge, &AiBridge::modelsFailed);
    bridge.listModels();

    QTRY_VERIFY_WITH_TIMEOUT(failed.count() == 1, 5000);
    QVERIFY(failed.at(0).at(0).toString().contains(QStringLiteral("no models")));
}

void TestAiBridge::streamsNdjsonChunks() {
    // NDJSON: one bare JSON object per line. Not SSE — no "data:" prefix and
    // no blank-line separator. Code expecting SSE reads nothing at all here.
    m_chatReply = "{\"message\":{\"content\":\"Hel\"},\"done\":false}\n"
                  "{\"message\":{\"content\":\"lo\"},\"done\":false}\n"
                  "{\"message\":{\"content\":\"\"},\"done\":true}\n";

    AiBridge bridge;
    primeModels(bridge);

    QSignalSpy chunks(&bridge, &AiBridge::chunk);
    QSignalSpy done(&bridge, &AiBridge::done);
    QVERIFY(bridge.ask(QStringLiteral("dev"), QStringLiteral("m"), QStringLiteral("hi")) != 0);

    QTRY_VERIFY_WITH_TIMEOUT(done.count() == 1, 5000);
    QCOMPARE(chunks.count(), 2);
    QCOMPARE(chunks.at(0).at(1).toString(), QStringLiteral("Hel"));
    QCOMPARE(chunks.at(1).at(1).toString(), QStringLiteral("lo"));
    // The pieces reassemble into the whole answer, which is what goes into
    // the history.
    QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("Hello"));
}

void TestAiBridge::splitsLinesAcrossPackets() {
    // A JSON line arriving in two reads must not be parsed as two lines, and
    // the half-object must not be dropped. This is the classic streaming bug,
    // and it never appears when the whole reply fits in one packet.
    m_chatReply = "{\"message\":{\"content\":\"one\"},\"done\":false}\n"
                  "{\"message\":{\"content\":\"two\"},\"done\":true}\n";
    m_splitChatWrite = true;

    AiBridge bridge;
    primeModels(bridge);

    QSignalSpy done(&bridge, &AiBridge::done);
    bridge.ask(QStringLiteral("dev"), QStringLiteral("m"), QStringLiteral("hi"));

    QTRY_VERIFY_WITH_TIMEOUT(done.count() == 1, 5000);
    QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("onetwo"));
}

void TestAiBridge::neverOffersTools() {
    // The assertion this whole class exists to keep true.
    //
    // Maze AI proper emits a JSON tool protocol and runs shell commands with
    // it. If a "tools" key ever appears in this payload, a phone becomes a
    // remote driver for that — precisely the thing the desktop allow-list was
    // chosen instead of.
    m_chatReply = "{\"message\":{\"content\":\"ok\"},\"done\":true}\n";

    AiBridge bridge;
    primeModels(bridge);

    QVERIFY(bridge.ask(QStringLiteral("dev"), QStringLiteral("m"), QStringLiteral("hi")) != 0);
    QTRY_VERIFY_WITH_TIMEOUT(!m_lastRequestBody.isEmpty(), 5000);

    const QJsonObject body = QJsonDocument::fromJson(m_lastRequestBody).object();
    QVERIFY2(!body.contains(QLatin1StringView("tools")), "the request offered tools");
    QVERIFY2(!body.contains(QLatin1StringView("functions")), "the request offered functions");
    QVERIFY(body.value(QLatin1StringView("stream")).toBool());
    QCOMPARE(body.value(QLatin1StringView("options")).toObject()
                 .value(QLatin1StringView("num_ctx")).toInt(),
             AiBridge::kNumCtx);
}

void TestAiBridge::sendsTheChatOnlyPersona() {
    m_chatReply = "{\"message\":{\"content\":\"ok\"},\"done\":true}\n";

    AiBridge bridge;
    primeModels(bridge);

    bridge.ask(QStringLiteral("dev"), QStringLiteral("m"), QStringLiteral("hi"));
    QTRY_VERIFY_WITH_TIMEOUT(!m_lastRequestBody.isEmpty(), 5000);

    const QJsonArray messages = QJsonDocument::fromJson(m_lastRequestBody)
                                    .object()
                                    .value(QLatin1StringView("messages"))
                                    .toArray();
    QVERIFY(messages.size() >= 2);

    // The system prompt is ours and comes first. A phone sends one line of
    // text and cannot replace this.
    const QJsonObject system = messages.at(0).toObject();
    QCOMPARE(system.value(QLatin1StringView("role")).toString(), QStringLiteral("system"));
    const QString content = system.value(QLatin1StringView("content")).toString();
    QVERIFY2(content.contains(QStringLiteral("Maze AI")), "the persona was not sent");
    QVERIFY2(content.contains(QStringLiteral("CHAT-ONLY")), "the chat-only mode was not stated");

    const QJsonObject user = messages.at(1).toObject();
    QCOMPARE(user.value(QLatin1StringView("role")).toString(), QStringLiteral("user"));
    QCOMPARE(user.value(QLatin1StringView("content")).toString(), QStringLiteral("hi"));
}

void TestAiBridge::refusesAModelItWasNotTold() {
    m_tagsReply = R"({"models":[{"name":"known"}]})";
    m_chatReply = "{\"message\":{\"content\":\"ok\"},\"done\":true}\n";

    AiBridge bridge;
    primeModels(bridge);

    // A model name arrives from a peer. Passing it through would hand a
    // peer-chosen string straight to Ollama's API.
    QCOMPARE(bridge.ask(QStringLiteral("dev"), QStringLiteral("../../etc/passwd"),
                        QStringLiteral("hi")),
             0u);
    QCOMPARE(bridge.ask(QStringLiteral("dev"), QStringLiteral("unknown:latest"),
                        QStringLiteral("hi")),
             0u);
    QCOMPARE(bridge.ask(QStringLiteral("dev"), QString(), QStringLiteral("hi")), 0u);
    QVERIFY(bridge.ask(QStringLiteral("dev"), QStringLiteral("known"),
                       QStringLiteral("hi")) != 0);
}

void TestAiBridge::refusesAnEmptyOrOversizedPrompt() {
    AiBridge bridge;
    primeModels(bridge);

    QCOMPARE(bridge.ask(QStringLiteral("dev"), QStringLiteral("m"), QString()), 0u);
    QCOMPARE(bridge.ask(QStringLiteral("dev"), QStringLiteral("m"),
                        QString(AiBridge::kMaxPromptChars + 1, QLatin1Char('x'))),
             0u);
}

void TestAiBridge::keepsHistoryPerConversation() {
    m_chatReply = "{\"message\":{\"content\":\"ok\"},\"done\":true}\n";

    AiBridge bridge;
    primeModels(bridge);

    bridge.ask(QStringLiteral("phone-a"), QStringLiteral("m"), QStringLiteral("secret-a"));
    QTRY_VERIFY_WITH_TIMEOUT(!m_lastRequestBody.isEmpty(), 5000);
    m_lastRequestBody.clear();

    // A different device must not inherit the other's conversation: two
    // people paired to one machine do not share a chat.
    bridge.ask(QStringLiteral("phone-b"), QStringLiteral("m"), QStringLiteral("second"));
    QTRY_VERIFY_WITH_TIMEOUT(!m_lastRequestBody.isEmpty(), 5000);

    const QJsonArray messages = QJsonDocument::fromJson(m_lastRequestBody)
                                    .object()
                                    .value(QLatin1StringView("messages"))
                                    .toArray();
    for (const QJsonValue &value : messages) {
        QVERIFY2(!value.toObject().value(QLatin1StringView("content")).toString()
                      .contains(QStringLiteral("secret-a")),
                 "one device's conversation leaked into another's");
    }
}

void TestAiBridge::carriesHistoryForwardWithinAConversation() {
    // The point of keeping history on this side: the second turn has to know
    // about the first, or the phone would have to send the transcript back —
    // and then it could edit it.
    m_chatReply = "{\"message\":{\"content\":\"first answer\"},\"done\":true}\n";

    AiBridge bridge;
    primeModels(bridge);

    QSignalSpy done(&bridge, &AiBridge::done);
    bridge.ask(QStringLiteral("dev"), QStringLiteral("m"), QStringLiteral("first question"));
    QTRY_VERIFY_WITH_TIMEOUT(done.count() == 1, 5000);
    m_lastRequestBody.clear();

    bridge.ask(QStringLiteral("dev"), QStringLiteral("m"), QStringLiteral("second question"));
    QTRY_VERIFY_WITH_TIMEOUT(!m_lastRequestBody.isEmpty(), 5000);

    const QJsonArray messages = QJsonDocument::fromJson(m_lastRequestBody)
                                    .object()
                                    .value(QLatin1StringView("messages"))
                                    .toArray();
    // system + user + assistant + user
    QCOMPARE(messages.size(), 4);
    QCOMPARE(messages.at(1).toObject().value(QLatin1StringView("content")).toString(),
             QStringLiteral("first question"));
    QCOMPARE(messages.at(2).toObject().value(QLatin1StringView("role")).toString(),
             QStringLiteral("assistant"));
    QCOMPARE(messages.at(2).toObject().value(QLatin1StringView("content")).toString(),
             QStringLiteral("first answer"));
}

void TestAiBridge::aTruncatedStreamStillCompletes() {
    // The connection drops mid-answer: chunks arrived but no {"done":true}.
    // What the user already read must be reported rather than discarded, and
    // the request must not be left hanging.
    m_chatReply = "{\"message\":{\"content\":\"partial\"},\"done\":false}\n";

    AiBridge bridge;
    primeModels(bridge);

    QSignalSpy chunks(&bridge, &AiBridge::chunk);
    QSignalSpy done(&bridge, &AiBridge::done);
    QSignalSpy failed(&bridge, &AiBridge::failed);
    QVERIFY(bridge.ask(QStringLiteral("dev"), QStringLiteral("m"), QStringLiteral("hi")) != 0);

    QTRY_VERIFY_WITH_TIMEOUT(done.count() + failed.count() == 1, 10000);
    QCOMPARE(chunks.count(), 1);
    // Reported as a completed (if short) answer rather than a failure: the
    // text is real and the user saw it.
    QCOMPARE(done.count(), 1);
    QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("partial"));
}

QTEST_MAIN(TestAiBridge)
#include "tst_aibridge.moc"
