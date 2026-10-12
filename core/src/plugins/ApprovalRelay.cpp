#include "mazeconnect/core/ApprovalRelay.h"

#include "mazeconnect/core/Limits.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QTimer>

#include <cerrno>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mazeconnect::core {

Q_LOGGING_CATEGORY(lcRelay, "maze.connect.approval")

namespace {

constexpr auto kDirName = "maze-connect";
constexpr auto kSocketName = "approval.sock";

bool hasControlCharacters(const QString &s) {
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)
            || (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069)) {
            return true;
        }
    }
    return false;
}

/// A bounded, control-free string field, or nullopt.
std::optional<QString> boundedString(const QJsonObject &obj, const char *key, int maxChars,
                                     bool allowEmpty) {
    const QJsonValue value = obj.value(QLatin1StringView(key));
    if (!value.isString()) {
        return std::nullopt;
    }
    const QString s = value.toString();
    if (s.size() > maxChars || hasControlCharacters(s) || (!allowEmpty && s.isEmpty())) {
        return std::nullopt;
    }
    return s;
}

QString currentUserName() {
    const struct passwd *pw = ::getpwuid(::getuid());
    return pw && pw->pw_name ? QString::fromLocal8Bit(pw->pw_name) : QString();
}

} // namespace

ApprovalRelay::ApprovalRelay(QObject *parent) : QObject(parent) {
    qRegisterMetaType<Request>();
    m_peerCheck = [](uid_t uid) { return uid == 0; };
}

ApprovalRelay::~ApprovalRelay() {
    stop();
}

bool ApprovalRelay::start(const QString &runtimeDir, QString *error) {
    stop();
    QString ignored;
    QString &err = error ? *error : ignored;
    if (runtimeDir.isEmpty() || !QDir::isAbsolutePath(runtimeDir)) {
        err = QStringLiteral("no runtime directory");
        return false;
    }
    const QString dir = QDir(runtimeDir).filePath(QLatin1StringView(kDirName));
    const QByteArray dirPath = QFile::encodeName(dir);

    if (::mkdir(dirPath.constData(), 0700) != 0 && errno != EEXIST) {
        err = QStringLiteral("cannot create %1").arg(dir);
        return false;
    }
    // Whatever was there must be ours, a real directory, and closed to
    // everyone else — otherwise another user could have put a socket of
    // their own in it before we started.
    struct stat st {};
    if (::lstat(dirPath.constData(), &st) != 0 || !S_ISDIR(st.st_mode)
        || st.st_uid != ::getuid() || (st.st_mode & 0077) != 0) {
        err = QStringLiteral("%1 is not a private directory of this user").arg(dir);
        return false;
    }

    m_socketPath = QDir(dir).filePath(QLatin1StringView(kSocketName));
    // sockaddr_un holds 108 bytes; a longer path fails in bind() with an
    // error that names nothing useful.
    if (QFile::encodeName(m_socketPath).size() >= 108) {
        err = QStringLiteral("socket path too long: %1").arg(m_socketPath);
        return false;
    }
    QLocalServer::removeServer(m_socketPath);
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    m_server->setMaxPendingConnections(kMaxClients);
    if (!m_server->listen(m_socketPath)) {
        err = m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, &ApprovalRelay::onNewConnection);
    qCInfo(lcRelay) << "approval relay listening at" << m_socketPath;
    return true;
}

void ApprovalRelay::stop() {
    const auto sockets = m_clients.keys();
    for (QLocalSocket *socket : sockets) {
        drop(socket, true);
    }
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
}

bool ApprovalRelay::isListening() const {
    return m_server && m_server->isListening();
}

void ApprovalRelay::setPeerCheckForTesting(std::function<bool(uid_t)> check) {
    m_peerCheck = std::move(check);
}

void ApprovalRelay::onNewConnection() {
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        struct ucred cred {};
        socklen_t len = sizeof(cred);
        const bool known = ::getsockopt(static_cast<int>(socket->socketDescriptor()), SOL_SOCKET,
                                        SO_PEERCRED, &cred, &len)
                == 0
            && len == sizeof(cred);
        if (!known || !m_peerCheck(cred.uid) || m_clients.size() >= kMaxClients) {
            qCWarning(lcRelay) << "refusing approval relay connection from uid"
                               << (known ? static_cast<long>(cred.uid) : -1L);
            socket->abort();
            socket->deleteLater();
            continue;
        }

        Client client;
        client.socket = socket;
        client.deadline = new QTimer(socket);
        client.deadline->setSingleShot(true);
        // Until the request line arrives, only a short wait: a connection
        // that says nothing is not going to.
        client.deadline->start(3000);
        connect(client.deadline, &QTimer::timeout, this, [this, socket] { drop(socket, true); });
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] { drop(socket, true); });
        m_clients.insert(socket, client);
    }
}

void ApprovalRelay::onReadyRead(QLocalSocket *socket) {
    auto it = m_clients.find(socket);
    if (it == m_clients.end()) {
        return;
    }
    it->buffer.append(socket->readAll());
    for (;;) {
        it = m_clients.find(socket);
        if (it == m_clients.end()) {
            return;
        }
        const qsizetype newline = it->buffer.indexOf('\n');
        if (newline < 0) {
            if (it->buffer.size() > kMaxLineBytes) {
                drop(socket, true);
            }
            return;
        }
        if (newline > kMaxLineBytes) {
            drop(socket, true);
            return;
        }
        const QByteArray line = it->buffer.left(newline);
        it->buffer.remove(0, newline + 1);
        if (it->id == 0) {
            handleRequestLine(*it, line);
        } else {
            handleResultLine(*it, line);
        }
    }
}

void ApprovalRelay::handleRequestLine(Client &client, const QByteArray &line) {
    QLocalSocket *socket = client.socket;
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    const QJsonObject obj = doc.object();
    auto refuse = [&](const QString &reason) {
        qCWarning(lcRelay) << "refusing approval request:" << reason;
        reply(client, errorLine(reason));
        drop(socket, false);
    };
    if (!doc.isObject() || obj.value(QLatin1StringView("v")).toInt(-1) != kVersion) {
        refuse(QStringLiteral("unsupported request"));
        return;
    }

    Request request;
    const auto purpose = approval::purposeFromName(
        obj.value(QLatin1StringView("purpose")).toString());
    // Unlocking is asked for by the phone itself, never through here.
    if (!purpose || *purpose == approval::Purpose::Unlock) {
        refuse(QStringLiteral("unknown purpose"));
        return;
    }
    const auto host = boundedString(obj, "host", limits::kMaxApprovalHostChars, false);
    const auto user = boundedString(obj, "user", limits::kMaxApprovalUserChars, false);
    const auto detail = boundedString(obj, "detail", limits::kMaxApprovalDetailChars, true);
    if (!host || !user || !detail) {
        refuse(QStringLiteral("malformed request"));
        return;
    }
    // This relay speaks for its own user's phones only.
    if (*user != currentUserName()) {
        refuse(QStringLiteral("not this user's relay"));
        return;
    }
    const QByteArray nonce = QByteArray::fromBase64(
        obj.value(QLatin1StringView("nonce")).toString().toLatin1(),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (nonce.size() != limits::kApprovalNonceSize) {
        refuse(QStringLiteral("malformed request"));
        return;
    }
    const QJsonValue issued = obj.value(QLatin1StringView("issuedAt"));
    const qint64 issuedAt = static_cast<qint64>(issued.toDouble(-1));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!issued.isDouble() || issuedAt <= 0 || qAbs(now - issuedAt) > kMaxClockSkewSec) {
        refuse(QStringLiteral("stale request"));
        return;
    }
    const int timeoutMs = obj.value(QLatin1StringView("timeoutMs")).toInt(-1);
    if (timeoutMs < kMinTimeoutMs || timeoutMs > kMaxTimeoutMs) {
        refuse(QStringLiteral("malformed request"));
        return;
    }

    request.id = m_nextId++;
    if (m_nextId == 0) {
        m_nextId = 1;
    }
    request.challenge.purpose = *purpose;
    request.challenge.host = *host;
    request.challenge.user = *user;
    request.challenge.detail = *detail;
    request.challenge.nonce = nonce;
    request.challenge.issuedAt = issuedAt;
    request.timeoutMs = timeoutMs;

    client.id = request.id;
    client.deadline->start(timeoutMs + 2000);
    emit requested(request);
}

void ApprovalRelay::handleResultLine(Client &client, const QByteArray &line) {
    if (!client.answered) {
        drop(client.socket, true);
        return;
    }
    const QJsonObject obj = QJsonDocument::fromJson(line).object();
    const quint32 id = client.id;
    const bool verified = obj.value(QLatin1StringView("verified")).toBool(false);
    QLocalSocket *socket = client.socket;
    drop(socket, false);
    emit finished(id, verified);
}

void ApprovalRelay::reply(Client &client, const QByteArray &line) {
    if (!client.socket) {
        return;
    }
    client.socket->write(line + '\n');
    client.socket->flush();
}

QByteArray ApprovalRelay::errorLine(const QString &reason) {
    // Printable ASCII only, and short: pam_mazeconnect writes it to a
    // terminal and to the system log, and is not going to guess at encodings.
    QByteArray text = reason.toLatin1().left(160);
    for (char &c : text) {
        if (c < 0x20 || c > 0x7E) {
            c = '?';
        }
    }
    return "ERR " + text;
}

ApprovalRelay::Client *ApprovalRelay::clientFor(quint32 id) {
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->id == id && id != 0) {
            return &it.value();
        }
    }
    return nullptr;
}

void ApprovalRelay::answer(quint32 id, const QByteArray &signature,
                           const QByteArray &verifierFingerprint, const QString &deviceName) {
    Client *client = clientFor(id);
    if (!client || client->answered) {
        return;
    }
    client->answered = true;
    // PAM verifies and says so in a moment; it will not wait long.
    client->deadline->start(5000);
    Q_UNUSED(deviceName); // PAM names the key from its own file, which root wrote
    reply(*client, "OK " + signature.toBase64() + ' ' + verifierFingerprint.toBase64());
}

void ApprovalRelay::fail(quint32 id, const QString &reason) {
    Client *client = clientFor(id);
    if (!client || client->answered) {
        return;
    }
    QLocalSocket *socket = client->socket;
    reply(*client, errorLine(reason));
    drop(socket, false);
}

void ApprovalRelay::drop(QLocalSocket *socket, bool notify) {
    const auto it = m_clients.find(socket);
    if (it == m_clients.end()) {
        return;
    }
    const quint32 id = it->id;
    const bool answered = it->answered;
    m_clients.erase(it);
    if (socket) {
        socket->disconnect(this);
        socket->disconnectFromServer();
        socket->deleteLater();
    }
    if (notify && id != 0) {
        if (answered) {
            emit finished(id, false);
        } else {
            emit abandoned(id);
        }
    }
}

} // namespace mazeconnect::core
