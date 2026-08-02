#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace mazeconnect::core {

/**
 * Maze AI, over the local Ollama instance.
 *
 * **Chat, not agent — and that is a design decision, not a shortcut.**
 *
 * Maze AI proper is agentic: it emits a JSON tool protocol and runs shell
 * commands with it. None of that is here. This class has no QProcess, no
 * tool catalogue, and no branch that could acquire one; the system prompt it
 * sends is Maze AI's own CHAT_ONLY persona, which is a configuration that
 * application already supports rather than a lesser version of it.
 *
 * The reason is the same decision that shaped `commands`: the user chose a
 * desktop-defined allow-list precisely so a phone could not run arbitrary
 * things. Letting the phone drive a tool-calling agent would route straight
 * around that, and would do it through a component whose output is a language
 * model's guess. If tool calling from the phone is ever wanted, it needs to be
 * its own deliberate decision, not a side effect of adding chat.
 *
 * The conversation lives *here*, keyed by device. A phone sends one line of
 * text; it does not send history, it does not send a system prompt, and it
 * therefore cannot replace the persona or backdate what "the assistant said
 * earlier". That is worth the small amount of state it costs.
 */
class AiBridge : public QObject {
    Q_OBJECT

public:
    explicit AiBridge(QObject *parent = nullptr);
    ~AiBridge() override;

    /**
     * Where Ollama is. **Loopback only.**
     *
     * $MAZECONNECT_OLLAMA_HOST overrides it for tests. Pointing this at
     * another machine would turn a local model into a network service and
     * send the user's conversation somewhere they did not choose, so the
     * default is not configurable through any remote path — nothing a peer
     * sends reaches this function.
     */
    static QUrl host();

    /// Ask Ollama what it has. Answers with modelsReady() or modelsFailed().
    void listModels();

    /**
     * Send a prompt and stream the reply.
     *
     * @p conversationKey scopes the history — one per device, so two phones
     *    do not read each other's conversation.
     * @p model must be one Ollama actually reported; an unknown name is
     *    refused rather than passed through.
     *
     * Returns a handle, or 0 if nothing was started.
     */
    quint32 ask(const QString &conversationKey, const QString &model, const QString &prompt);

    /// Stop a reply mid-stream. The partial answer stays in the history: it
    /// is what the user saw, and pretending otherwise would make the next
    /// turn incoherent.
    void cancel(quint32 requestId);

    /// Forget one conversation.
    void resetConversation(const QString &conversationKey);

    /// Models seen in the last successful listModels(). A prompt naming
    /// anything else is refused.
    QStringList knownModels() const { return m_models; }

    /// Ollama's context window. Matches Maze AI's own default.
    static constexpr int kNumCtx = 8192;

    /// History budget in characters, trimmed oldest-first. Carried over from
    /// Maze AI rather than re-derived.
    static constexpr int kHistoryBudgetChars = 24000;

    /// Longest single prompt accepted from a peer.
    static constexpr int kMaxPromptChars = 8000;

    /// A model list longer than this is not one we can show sensibly.
    static constexpr int kMaxModels = 64;

    /// Whole-request ceiling. Generous: a large model on a cold start is
    /// genuinely slow, and killing a working answer is worse than waiting.
    static constexpr int kRequestTimeoutMs = 180000;

    /// One reply in flight per conversation. A second ask() for the same
    /// device replaces the first rather than interleaving two streams into
    /// one history.
    static constexpr int kMaxOutputChars = 32000;

signals:
    void modelsReady(const QStringList &models);
    void modelsFailed(const QString &reason);

    void chunk(quint32 requestId, const QString &text);
    void done(quint32 requestId, const QString &fullText);
    void failed(quint32 requestId, const QString &reason);

private:
    struct Turn {
        QString role; ///< "user" or "assistant"
        QString text;
    };

    struct Exchange {
        QNetworkReply *reply = nullptr;
        QString conversationKey;
        QByteArray buffer;   ///< partial NDJSON line
        QString answer;      ///< accumulated reply
    };

    void consume(quint32 requestId);
    void finish(quint32 requestId, const QString &error);
    QList<Turn> &history(const QString &key);
    void trim(QList<Turn> &turns);

    QNetworkAccessManager *m_net = nullptr;
    QStringList m_models;
    QHash<QString, QList<Turn>> m_histories;
    QHash<quint32, Exchange> m_inFlight;
    quint32 m_nextRequestId = 1;
};

} // namespace mazeconnect::core
