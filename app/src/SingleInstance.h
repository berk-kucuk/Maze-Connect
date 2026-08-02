#pragma once

#include <QLocalServer>
#include <QObject>

#include <memory>

class QLockFile;

/**
 * Ensures one running copy per user session.
 *
 * Beyond being what people expect from a launcher icon, a second copy would
 * be actively broken here: both would try to bind the discovery port and
 * open a listener with the same identity key, so the phone would see two
 * conflicting endpoints for one machine.
 *
 * A later launch hands its request to the running instance and exits, which
 * raises the existing window instead of doing nothing visible.
 *
 * **Two mechanisms, and the split matters.** A lock file decides who is
 * primary; the local socket is only the "come to the front" channel. It was
 * originally the socket alone, which had a race that produced exactly the
 * duplicate this class exists to prevent: a second launch probed the socket
 * with a 300 ms timeout, and if the running instance was busy enough to miss
 * that window the probe was read as a leftover from a crash. The second copy
 * then *deleted the live instance's socket* and listened on it itself —
 * leaving two running copies, two windows, two tray icons, and a first
 * instance nothing could reach any more. QLockFile settles staleness by pid
 * rather than by how quickly a busy process answers.
 */
class SingleInstance : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(QString key, QObject *parent = nullptr);
    ~SingleInstance() override;

    /**
     * Try to become the primary instance.
     *
     * Returns true if this process now owns the lock. Returns false if
     * another copy is already running — it has been asked to show itself,
     * and this process should exit.
     */
    bool tryAcquire();

signals:
    /// Another launch asked us to come to the front.
    void raiseRequested();

private:
    /// Ask the running instance to show itself. Best effort: failing to
    /// reach it is not a reason to start a second copy.
    void requestRaise();

    QString m_key;
    QLocalServer m_server;
    std::unique_ptr<QLockFile> m_lock;
};
