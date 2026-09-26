#pragma once

#include <QJsonArray>
#include <QString>

namespace mazeconnect::core {

/**
 * The one folder a paired phone may browse and download from.
 *
 * Not the home directory, not the filesystem: a single folder the user puts
 * things in on purpose (~/Maze Connect Shared by default). Everything a phone
 * names is a path *relative to it*, and every request is resolved again from
 * scratch — nothing is cached that a later request could lean on.
 *
 * What resolve() refuses, before touching the disk: absolute paths,
 * backslashes, empty, "." and ".." segments, hidden (dot) names, control
 * characters, more than kMaxDepth levels, more than kMaxPathChars. What it
 * refuses on the disk: any component that is a symlink, and any result whose
 * canonical path is not inside the canonical root. The two checks overlap on
 * purpose: the first keeps the obvious out without a syscall, the second is
 * the one that holds if the first ever has a gap.
 */
class SharedFolder {
public:
    static constexpr int kMaxPathChars = 1024;
    static constexpr int kMaxDepth = 32;
    static constexpr int kMaxEntries = 500;
    static constexpr int kMaxNameChars = 255;

    /// $MAZECONNECT_SHARED_DIR (tests) or ~/Maze Connect Shared.
    static QString defaultRoot();

    explicit SharedFolder(QString root = defaultRoot());

    QString root() const { return m_root; }

    /// Create the folder (owner-only) if it does not exist yet.
    bool ensureExists() const;

    /**
     * The absolute path for @p relative, or empty with @p error set. Does not
     * require the target to exist beyond what the checks need.
     */
    QString resolve(const QString &relative, QString &error) const;

    /**
     * One directory's entries: [{name, dir, size, mtime}], directories first.
     * Hidden files, symlinks and anything that is neither a regular file nor
     * a directory are left out.
     */
    QJsonArray list(const QString &relative, QString &error) const;

    /// A regular file inside the folder, ready to send; empty with @p error.
    QString fileForFetch(const QString &relative, QString &error) const;

private:
    QString m_root;
};

} // namespace mazeconnect::core
