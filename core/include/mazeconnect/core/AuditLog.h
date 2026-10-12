#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

namespace mazeconnect::core {

/**
 * What paired devices did to this computer, kept across restarts.
 *
 * The in-window Activity list is a view of the current session; this is the
 * record a person consults afterwards — "did my phone run something last
 * night?", "who unlocked the screen?". It holds **actions by paired
 * devices** (pairing, permissions, commands, killswitches, remote control,
 * unlocks and approvals, files) and nothing about devices that were merely
 * nearby, nor the content of anything sent: a command's id, never its
 * output; a file's name, never its bytes; never clipboard text.
 *
 * One JSON object per line in an owner-only file, rotated once it passes
 * kRotateBytes so it cannot grow without bound. It is not tamper-proof and
 * does not pretend to be: anything running as this user can edit it. The
 * record that is — sudo and polkit approvals — goes to the system journal
 * from pam_mazeconnect, as root.
 */
class AuditLog {
public:
    struct Entry {
        QDateTime at;
        QString deviceId;
        QString deviceName;
        QString action;
        QString detail;
        bool ok = true;
    };

    static constexpr qint64 kRotateBytes = 256 * 1024;
    static constexpr int kMaxFieldChars = 200;

    explicit AuditLog(QString path = QString());

    QString path() const { return m_path; }

    /// Append one entry. Fields are cut to kMaxFieldChars and stripped of
    /// control characters, so a device name cannot forge a second line.
    bool record(const QString &deviceId, const QString &deviceName, const QString &action,
                const QString &detail = QString(), bool ok = true);

    /// Up to @p max entries, newest first, across the rotated file too.
    QList<Entry> recent(int max) const;

    /// Remove the log and its rotated copy.
    bool clear();

private:
    QString m_path;
};

} // namespace mazeconnect::core
