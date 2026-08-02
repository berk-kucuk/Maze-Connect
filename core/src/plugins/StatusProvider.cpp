#include "mazeconnect/core/StatusProvider.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

namespace mazeconnect::core {
namespace {

Q_LOGGING_CATEGORY(lcStatus, "maze.connect.status")

// Baked at configure time from the install prefix so the helper is found
// beside mazeconnectd, wherever the package was installed.
#ifndef MAZECONNECT_STATUS_HELPER_PATH
#define MAZECONNECT_STATUS_HELPER_PATH "/usr/lib/mazeconnect/maze-connect-status"
#endif

} // namespace

StatusProvider::StatusProvider(QObject *parent) : QObject(parent) {}

StatusProvider::~StatusProvider() {
    if (m_process) {
        // The helper is short-lived and harmless, but leaving it attached to a
        // process we are destroying would let it outlive its own callbacks.
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

QString StatusProvider::helperPath() {
    const QString override =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("MAZECONNECT_STATUS_HELPER"));
    if (!override.isEmpty()) {
        return override;
    }
    return QStringLiteral(MAZECONNECT_STATUS_HELPER_PATH);
}

bool StatusProvider::isAvailable() const {
    const QFileInfo info(helperPath());
    return info.exists() && info.isFile() && info.isExecutable();
}

void StatusProvider::request() {
    if (!m_snapshot.isEmpty() && m_snapshotAt.isValid()
        && m_snapshotAt.msecsTo(QDateTime::currentDateTimeUtc()) < kCacheMs) {
        emit snapshotReady(m_snapshot);
        return;
    }

    // Already running: this request rides on that run. Everyone waiting is
    // answered by the same snapshotReady/snapshotFailed, so a peer sending
    // statusRequest in a tight loop still costs exactly one helper process.
    if (m_process) {
        return;
    }

    if (!isAvailable()) {
        failWith(QStringLiteral("maze-tools is not installed"));
        return;
    }
    startHelper();
}

void StatusProvider::startHelper() {
    m_output.clear();
    m_process = new QProcess(this);
    m_process->setProgram(helperPath());
    // No arguments, ever. The helper refuses them too — this is the half of
    // that promise which lives on our side.
    m_process->setArguments({});
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_output.append(m_process->readAllStandardOutput());
        if (m_output.size() > kMaxOutputBytes) {
            qCWarning(lcStatus) << "status helper output exceeded" << kMaxOutputBytes << "bytes";
            m_process->kill();
        }
    });

    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // Distinguished because they mean different things to whoever reads
        // the log: one is a broken install, the other is a helper we killed
        // for overrunning its time or its output budget.
        failWith(error == QProcess::FailedToStart
                     ? QStringLiteral("status helper could not run")
                     : QStringLiteral("status helper did not finish"));
    });

    connect(m_process, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                if (exitStatus != QProcess::NormalExit) {
                    failWith(QStringLiteral("status helper was killed"));
                    return;
                }
                if (exitCode != 0) {
                    // The helper prints its reason on stderr; it is ours, so
                    // it is safe to surface, but keep it bounded anyway.
                    const QString detail =
                        QString::fromUtf8(m_process->readAllStandardError().left(200)).trimmed();
                    failWith(detail.isEmpty()
                                 ? QStringLiteral("status helper failed")
                                 : detail);
                    return;
                }

                QJsonParseError error{};
                const QJsonDocument doc = QJsonDocument::fromJson(m_output, &error);
                if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                    failWith(QStringLiteral("status helper printed malformed JSON"));
                    return;
                }
                finishWith(doc.object());
            });

    // Guarded on the identity of *this* process, not merely on one being
    // alive: a later run started after this one finished must not be killed
    // by a timer left over from its predecessor.
    QPointer<QProcess> watched(m_process);
    QTimer::singleShot(kTimeoutMs, this, [this, watched] {
        if (watched && watched == m_process && watched->state() != QProcess::NotRunning) {
            qCWarning(lcStatus) << "status helper timed out";
            watched->kill();
        }
    });

    m_process->start();
}

void StatusProvider::finishWith(const QJsonObject &snapshot) {
    m_snapshot = snapshot;
    m_snapshotAt = QDateTime::currentDateTimeUtc();

    QProcess *finished = m_process;
    m_process = nullptr;
    if (finished) {
        finished->deleteLater();
    }
    emit snapshotReady(snapshot);
}

void StatusProvider::failWith(const QString &reason) {
    QProcess *finished = m_process;
    m_process = nullptr;
    if (finished) {
        finished->disconnect(this);
        finished->deleteLater();
    }
    qCInfo(lcStatus) << "snapshot unavailable:" << reason;
    emit snapshotFailed(reason);
}

} // namespace mazeconnect::core
