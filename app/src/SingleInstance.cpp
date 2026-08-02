#include "SingleInstance.h"

#include <QDir>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>

namespace {

/// Where the lock lives. The runtime directory is per-session and cleared at
/// logout, which is the lifetime we want; falling back to temp keeps this
/// working in a plain shell where XDG_RUNTIME_DIR is not set.
QString lockPathFor(const QString &key) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) {
        dir = QDir::tempPath();
    }
    QDir().mkpath(dir);
    return QDir(dir).filePath(key + QStringLiteral(".lock"));
}

} // namespace

SingleInstance::SingleInstance(QString key, QObject *parent)
    : QObject(parent), m_key(std::move(key)) {
    connect(&m_server, &QLocalServer::newConnection, this, [this]() {
        // The payload does not matter; the connection itself is the signal.
        QLocalSocket *client = m_server.nextPendingConnection();
        if (client != nullptr) {
            client->disconnectFromServer();
            client->deleteLater();
        }
        emit raiseRequested();
    });
}

SingleInstance::~SingleInstance() = default;

bool SingleInstance::tryAcquire() {
    m_lock = std::make_unique<QLockFile>(lockPathFor(m_key));

    // No waiting: either we are first or we are not. QLockFile records the
    // pid, so a lock left behind by a crash is recognised and taken over
    // rather than blocking every future launch.
    if (!m_lock->tryLock(0)) {
        m_lock.reset();
        requestRaise();
        return false;
    }

    // We hold the lock, so any socket file still present is genuinely stale —
    // no live instance can be listening on it.
    QLocalServer::removeServer(m_key);

    // Owner-only: the socket is a control channel into a process holding
    // the device identity, so no other local user may reach it.
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.listen(m_key);

    // Deliberately not fatal if the listen failed: the raise channel is a
    // convenience, and refusing to start over it would be worse than
    // starting without it.
    return true;
}

void SingleInstance::requestRaise() {
    QLocalSocket probe;
    probe.connectToServer(m_key);
    if (probe.waitForConnected(2000)) {
        probe.disconnectFromServer();
    }
}
