#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <sys/types.h>

#include "mazeconnect/core/Approval.h"

class QLocalServer;
class QLocalSocket;
class QTimer;

namespace mazeconnect::core {

/**
 * The local end pam_mazeconnect talks to: `$XDG_RUNTIME_DIR/maze-connect/
 * approval.sock`.
 *
 * **This is a relay and nothing more, and it is untrusted by design.** The
 * PAM module, running as root, makes the challenge, shows its code on the
 * terminal, and verifies the phone's signature itself against a key that
 * root enrolled. All this object does is carry the challenge to the phones
 * and the signature back. It never learns the code — PAM does not send it —
 * so even something that took this socket over could not answer for the
 * owner, nor pre-fill what the owner is meant to type.
 *
 * It accepts connections from root only (SO_PEERCRED) under a deadline. PAM
 * sends one line of JSON; the answer is one line of fixed-form text —
 * `OK <signature> <verifier>` or `ERR <reason>` — so that the side running
 * as root never has to parse JSON from a process it does not trust. Then PAM
 * sends `{"v":1,"verified":…}`. See docs/PROTOCOL.md § approval relay.
 */
class ApprovalRelay : public QObject {
    Q_OBJECT

public:
    struct Request {
        quint32 id = 0;
        approval::Challenge challenge; ///< code is always empty here
        int timeoutMs = 0;
    };

    static constexpr int kVersion = 1;
    static constexpr int kMaxLineBytes = 2048;
    static constexpr int kMaxClients = 4;
    static constexpr int kMinTimeoutMs = 5000;
    static constexpr int kMaxTimeoutMs = 60000;
    /// A request issued further from now than this is refused.
    static constexpr qint64 kMaxClockSkewSec = 300;

    explicit ApprovalRelay(QObject *parent = nullptr);
    ~ApprovalRelay() override;

    /**
     * Listen under @p runtimeDir (normally $XDG_RUNTIME_DIR). The
     * `maze-connect` directory there is created 0700, and refused if it
     * exists owned by someone else or open to anyone else.
     */
    bool start(const QString &runtimeDir, QString *error);
    void stop();
    bool isListening() const;
    QString socketPath() const { return m_socketPath; }

    /// The phone answered request @p id; send it to PAM.
    void answer(quint32 id, const QByteArray &signature, const QByteArray &verifierFingerprint,
                const QString &deviceName);
    /// Request @p id will not be answered; tell PAM why.
    void fail(quint32 id, const QString &reason);

    /// For tests: decide which peer uids may connect (default: root only).
    void setPeerCheckForTesting(std::function<bool(uid_t)> check);

signals:
    void requested(const mazeconnect::core::ApprovalRelay::Request &request);
    /// PAM said whether the signature it was given verified.
    void finished(quint32 id, bool verified);
    /// PAM went away, or the deadline passed, before an answer.
    void abandoned(quint32 id);

private:
    struct Client {
        QPointer<QLocalSocket> socket;
        QByteArray buffer;
        quint32 id = 0;
        bool answered = false;
        QTimer *deadline = nullptr;
    };

    void onNewConnection();
    void onReadyRead(QLocalSocket *socket);
    void handleRequestLine(Client &client, const QByteArray &line);
    void handleResultLine(Client &client, const QByteArray &line);
    void reply(Client &client, const QByteArray &line);
    static QByteArray errorLine(const QString &reason);
    void drop(QLocalSocket *socket, bool notify);
    Client *clientFor(quint32 id);

    QLocalServer *m_server = nullptr;
    QString m_socketPath;
    QHash<QLocalSocket *, Client> m_clients;
    quint32 m_nextId = 1;
    std::function<bool(uid_t)> m_peerCheck;
};

} // namespace mazeconnect::core

Q_DECLARE_METATYPE(mazeconnect::core::ApprovalRelay::Request)
