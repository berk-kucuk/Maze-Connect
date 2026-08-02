#include "mazeconnect/core/AiBridge.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QTimer>

namespace mazeconnect::core {
namespace {

Q_LOGGING_CATEGORY(lcAi, "maze.connect.ai")

/**
 * Maze AI's chat-only persona, carried over from
 * `Maze-AI/maze_ai/agent/prompts.py` (CHAT_ONLY).
 *
 * Kept verbatim rather than paraphrased so an answer here sounds like the
 * desktop assistant rather than like a different product wearing its name.
 * The tool-enabled PROTOCOL block is deliberately *not* here — see AiBridge's
 * class comment for why that is a decision rather than an omission.
 */
constexpr QLatin1StringView kSystemPrompt(R"(# Identity
You are Maze AI, the built-in AI assistant of Maze Linux — a privacy- and security-focused, Arch-based Linux distribution. You are an expert on Linux, the terminal, programming, security and general knowledge.

# Mode
You are currently in CHAT-ONLY mode: you cannot run commands, launch apps or touch the filesystem. Answer from your knowledge. When the best help is a command or a file edit, show it clearly (in a Markdown code block) and explain it so the user can run it themselves. Never claim to have executed anything.

# Conversation memory
The messages above are the ongoing conversation with this user in this chat. Use them as context and refer back to earlier turns instead of asking again.

# Answer style
Be clear, correct and concise. Use Markdown: backticks for commands/paths, fenced code blocks for multi-line snippets, bullet lists and bold for emphasis. Match the user's tone. When giving shell commands for an Arch system, prefer `pacman`/AUR conventions.

# Where you are
You are being read on a phone, over Maze Connect, rather than in the desktop app. Keep answers tight and scannable; prefer short paragraphs and lists over long prose.)");

} // namespace

AiBridge::AiBridge(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

AiBridge::~AiBridge() {
    for (auto &exchange : m_inFlight) {
        if (exchange.reply) {
            exchange.reply->disconnect(this);
            exchange.reply->abort();
            exchange.reply->deleteLater();
        }
    }
}

QUrl AiBridge::host() {
    const QString override = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("MAZECONNECT_OLLAMA_HOST"));
    if (!override.isEmpty()) {
        return QUrl(override);
    }
    return QUrl(QStringLiteral("http://127.0.0.1:11434"));
}

void AiBridge::listModels() {
    QNetworkRequest request(host().resolved(QUrl(QStringLiteral("/api/tags"))));
    QNetworkReply *reply = m_net->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            // The overwhelmingly common cause is Ollama simply not running,
            // and saying so is more useful than the transport error.
            emit modelsFailed(QStringLiteral("Ollama is not reachable on this machine"));
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            emit modelsFailed(QStringLiteral("Ollama returned something unexpected"));
            return;
        }

        QStringList models;
        const QJsonArray array = doc.object().value(QLatin1StringView("models")).toArray();
        for (const QJsonValue &value : array) {
            const QString name = value.toObject().value(QLatin1StringView("name")).toString();
            if (!name.isEmpty() && models.size() < kMaxModels) {
                models << name;
            }
        }
        m_models = models;

        if (models.isEmpty()) {
            // Reachable but empty is its own answer: "no models pulled" is a
            // thing the user can fix, and it is not the same as "not running".
            emit modelsFailed(QStringLiteral("Ollama has no models installed"));
            return;
        }
        emit modelsReady(models);
    });
}

quint32 AiBridge::ask(const QString &conversationKey, const QString &model,
                      const QString &prompt) {
    if (prompt.isEmpty() || prompt.size() > kMaxPromptChars) {
        return 0;
    }
    // Only a model Ollama actually reported. A name straight from a peer
    // would otherwise reach Ollama's API as-is.
    if (!m_models.contains(model)) {
        qCInfo(lcAi) << "refusing unknown model";
        return 0;
    }

    // One reply per conversation. Two overlapping streams would interleave
    // into a single history and produce a transcript that never happened.
    for (auto it = m_inFlight.begin(); it != m_inFlight.end(); ++it) {
        if (it->conversationKey == conversationKey) {
            cancel(it.key());
            break;
        }
    }

    QList<Turn> &turns = history(conversationKey);
    turns.append({QStringLiteral("user"), prompt});
    trim(turns);

    QJsonArray messages;
    {
        QJsonObject system;
        system.insert(QLatin1StringView("role"), QStringLiteral("system"));
        system.insert(QLatin1StringView("content"), QString(kSystemPrompt));
        messages.append(system);
    }
    for (const Turn &turn : turns) {
        QJsonObject entry;
        entry.insert(QLatin1StringView("role"), turn.role);
        entry.insert(QLatin1StringView("content"), turn.text);
        messages.append(entry);
    }

    QJsonObject options;
    options.insert(QLatin1StringView("temperature"), 0.4);
    options.insert(QLatin1StringView("num_ctx"), kNumCtx);

    QJsonObject body;
    body.insert(QLatin1StringView("model"), model);
    body.insert(QLatin1StringView("messages"), messages);
    body.insert(QLatin1StringView("stream"), true);
    body.insert(QLatin1StringView("options"), options);
    // No "tools" key, ever. Its absence is the feature.

    QNetworkRequest request(host().resolved(QUrl(QStringLiteral("/api/chat"))));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const quint32 requestId = m_nextRequestId++;
    QNetworkReply *reply = m_net->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));

    Exchange exchange;
    exchange.reply = reply;
    exchange.conversationKey = conversationKey;
    m_inFlight.insert(requestId, exchange);

    connect(reply, &QNetworkReply::readyRead, this, [this, requestId] { consume(requestId); });
    connect(reply, &QNetworkReply::finished, this, [this, requestId, reply] {
        const QString error = reply->error() == QNetworkReply::NoError
            ? QString()
            : QStringLiteral("Ollama stopped answering");
        finish(requestId, error);
    });

    QTimer::singleShot(kRequestTimeoutMs, this, [this, requestId] {
        const auto it = m_inFlight.find(requestId);
        if (it != m_inFlight.end() && it->reply) {
            qCWarning(lcAi) << "AI request timed out";
            it->reply->abort();
        }
    });

    return requestId;
}

void AiBridge::consume(quint32 requestId) {
    const auto it = m_inFlight.find(requestId);
    if (it == m_inFlight.end() || !it->reply) {
        return;
    }
    it->buffer.append(it->reply->readAll());

    // NDJSON, not SSE: one bare JSON object per line, no "data:" prefix and
    // no blank-line separator. Anything expecting SSE here silently reads
    // nothing.
    int newline = 0;
    while ((newline = it->buffer.indexOf('\n')) >= 0) {
        const QByteArray line = it->buffer.left(newline);
        it->buffer.remove(0, newline + 1);
        if (line.trimmed().isEmpty()) {
            continue;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject object = doc.object();

        const QString text = object.value(QLatin1StringView("message"))
                                 .toObject()
                                 .value(QLatin1StringView("content"))
                                 .toString();
        if (!text.isEmpty() && it->answer.size() < kMaxOutputChars) {
            it->answer += text;
            emit chunk(requestId, text);
        }

        if (object.value(QLatin1StringView("done")).toBool(false)) {
            // The stream is over even though the connection may not have
            // closed yet; finish() is idempotent from here.
            finish(requestId, QString());
            return;
        }
    }
}

void AiBridge::finish(quint32 requestId, const QString &error) {
    const auto it = m_inFlight.find(requestId);
    if (it == m_inFlight.end()) {
        return;
    }

    const QString key = it->conversationKey;
    const QString answer = it->answer;
    if (it->reply) {
        it->reply->disconnect(this);
        it->reply->deleteLater();
    }
    m_inFlight.erase(it);

    // Whatever the model actually said goes into the history, including a
    // partial answer after a cancel or a timeout. It is what the user read,
    // and a history that disagrees with the screen makes the next turn
    // answer a question nobody asked.
    if (!answer.isEmpty()) {
        QList<Turn> &turns = history(key);
        turns.append({QStringLiteral("assistant"), answer});
        trim(turns);
    }

    if (!error.isEmpty() && answer.isEmpty()) {
        emit failed(requestId, error);
        return;
    }
    emit done(requestId, answer);
}

void AiBridge::cancel(quint32 requestId) {
    const auto it = m_inFlight.find(requestId);
    if (it == m_inFlight.end() || !it->reply) {
        return;
    }
    it->reply->abort();
}

void AiBridge::resetConversation(const QString &conversationKey) {
    m_histories.remove(conversationKey);
}

QList<AiBridge::Turn> &AiBridge::history(const QString &key) {
    return m_histories[key];
}

void AiBridge::trim(QList<Turn> &turns) {
    // Oldest-first, by character budget, exactly as Maze AI does it. Dropping
    // from the front keeps the most recent exchange intact, which is the part
    // the next answer actually depends on.
    int total = 0;
    for (const Turn &turn : turns) {
        total += turn.text.size();
    }
    while (total > kHistoryBudgetChars && turns.size() > 1) {
        total -= turns.first().text.size();
        turns.removeFirst();
    }
}

} // namespace mazeconnect::core
