#pragma once

#include <QString>

namespace mazeconnect::core {

/**
 * This user's graphical session, as far as unlocking goes.
 *
 * Abstract so a test can stand in for logind; the real one is
 * LogindSessionControl below.
 */
class SessionControl {
public:
    virtual ~SessionControl() = default;

    enum class State { Locked, Unlocked, Unknown };

    /// Whether the screen is locked now. Unknown (with @p error) when there
    /// is no session to ask about.
    virtual State state(QString *error) = 0;

    /// Unlock it. Only ever called after state() said Locked and a phone's
    /// approval signature verified.
    virtual bool unlock(QString *error) = 0;
};

/**
 * logind's view of the session, over the system bus.
 *
 * The session is the one this process runs in ($XDG_SESSION_ID), or — for
 * the daemon, which runs under `systemd --user` and belongs to no session —
 * this user's display session. Unlocking calls Session.Unlock(), which
 * logind allows for a session's own user without a polkit prompt, and which
 * the screen locker (kscreenlocker, among others) obeys. No process is
 * spawned and no argument comes from the phone.
 */
class LogindSessionControl : public SessionControl {
public:
    State state(QString *error) override;
    bool unlock(QString *error) override;

private:
    /// The session's object path, resolved once and kept: a session does
    /// not move for the life of this process, and each lookup is a blocking
    /// call on the system bus. Dropped and resolved again if a call on it
    /// fails (the session ended, logind restarted).
    QString sessionPath(QString *error);
    QString resolveSessionPath(QString *error);
    QString m_path;
};

} // namespace mazeconnect::core
