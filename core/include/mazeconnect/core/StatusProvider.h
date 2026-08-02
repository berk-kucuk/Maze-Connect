#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QProcess;

namespace mazeconnect::core {

/**
 * Produces the system snapshot the dashboard is built from.
 *
 * The probes themselves are not reimplemented here. maze-tools already ships
 * `maze_status.py`, which maze-control-center and maze-welcome use, so this
 * runs the small `maze-connect-status` helper and forwards the JSON it prints.
 * Two implementations of the same probes would drift; one does not.
 *
 * Nothing a peer sends reaches the helper. It is invoked with no arguments and
 * no stdin — a `statusRequest` decides *whether* it runs, never *how*. The
 * helper is unprivileged and read-only, which is why this is the right place
 * to prove out the new message plumbing before anything privileged uses it.
 */
class StatusProvider : public QObject {
    Q_OBJECT

public:
    explicit StatusProvider(QObject *parent = nullptr);
    ~StatusProvider() override;

    /**
     * Absolute path of the helper.
     *
     * $MAZECONNECT_STATUS_HELPER overrides it, for tests and for running from
     * a build tree. That is not a privilege boundary: the helper runs as the
     * user who is already running this process, so anyone able to set its
     * environment can run their own code regardless.
     */
    static QString helperPath();

    /// False when maze-tools is not installed. The dashboard then says so
    /// rather than showing an empty machine.
    bool isAvailable() const;

    /**
     * Ask for a snapshot.
     *
     * A snapshot younger than kCacheMs is returned immediately via
     * snapshotReady(). Otherwise the helper is started and the signal follows
     * when it finishes. Requests arriving while it runs are coalesced onto
     * that one run — a peer sending statusRequest in a loop cannot fork a
     * process per message.
     */
    void request();

    /// Most recent successful snapshot, or an empty object if there is none.
    QJsonObject lastSnapshot() const { return m_snapshot; }

    /// Never launch the helper more often than this.
    static constexpr int kCacheMs = 2000;

    /// A helper that has not printed a snapshot by now is killed. maze_status
    /// gives each of its own probes a few seconds, so this has to clear the
    /// sum of them without being unbounded.
    static constexpr int kTimeoutMs = 15000;

    /// stdout ceiling. The snapshot is ~2 KB; this is here so that a wrong
    /// file at the helper's path cannot make us buffer without limit, and so
    /// a report always fits inside one control frame.
    static constexpr int kMaxOutputBytes = 128 * 1024;

signals:
    /// A fresh (or cached) snapshot is available.
    void snapshotReady(const QJsonObject &snapshot);

    /// No snapshot: maze-tools missing, the helper failed, timed out, or
    /// printed something that is not a JSON object.
    void snapshotFailed(const QString &reason);

private:
    void startHelper();
    void finishWith(const QJsonObject &snapshot);
    void failWith(const QString &reason);

    QProcess *m_process = nullptr;
    QByteArray m_output;

    QJsonObject m_snapshot;
    QDateTime m_snapshotAt;
};

} // namespace mazeconnect::core
