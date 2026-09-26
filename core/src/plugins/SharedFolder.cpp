#include "mazeconnect/core/SharedFolder.h"

#include "mazeconnect/core/Limits.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>

#include <algorithm>

namespace mazeconnect::core {
namespace {

QString tr(const char *text) {
    return QCoreApplication::translate("SharedFolder", text);
}

bool cleanName(const QString &name) {
    if (name.isEmpty() || name.size() > SharedFolder::kMaxNameChars) {
        return false;
    }
    for (const QChar c : name) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7f || (u >= 0x80 && u <= 0x9f)
            || (u >= 0x202a && u <= 0x202e) || (u >= 0x2066 && u <= 0x2069)) {
            return false;
        }
    }
    return true;
}

} // namespace

QString SharedFolder::defaultRoot() {
    const QString fromEnv = QString::fromLocal8Bit(qgetenv("MAZECONNECT_SHARED_DIR"));
    if (!fromEnv.isEmpty()) {
        return fromEnv;
    }
    return QDir::homePath() + QStringLiteral("/Maze Connect Shared");
}

SharedFolder::SharedFolder(QString root) : m_root(std::move(root)) {}

bool SharedFolder::ensureExists() const {
    QDir dir(m_root);
    if (dir.exists()) {
        return true;
    }
    if (!QDir().mkpath(m_root)) {
        return false;
    }
    // Owner-only, like the inbox: what is put here is for the phone, not for
    // every account on the machine.
    QFile::setPermissions(m_root, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return true;
}

QString SharedFolder::resolve(const QString &relative, QString &error) const {
    if (relative.size() > kMaxPathChars) {
        error = tr("that path is too long");
        return {};
    }
    if (relative.startsWith(QLatin1Char('/')) || relative.contains(QLatin1Char('\\'))) {
        error = tr("not a path inside the shared folder");
        return {};
    }
    const QStringList segments = relative.isEmpty()
        ? QStringList()
        : relative.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    if (segments.size() > kMaxDepth) {
        error = tr("that path is too deep");
        return {};
    }
    for (const QString &segment : segments) {
        if (segment.isEmpty() || segment == QLatin1StringView(".")
            || segment == QLatin1StringView("..") || segment.startsWith(QLatin1Char('.'))
            || !cleanName(segment)) {
            error = tr("not a path inside the shared folder");
            return {};
        }
    }

    const QFileInfo rootInfo(m_root);
    if (!rootInfo.isDir() || rootInfo.isSymLink()) {
        error = tr("the shared folder does not exist on the computer");
        return {};
    }
    const QString rootCanonical = rootInfo.canonicalFilePath();

    // Walk it: a symlink anywhere on the way is refused, not followed.
    QString path = m_root;
    for (const QString &segment : segments) {
        path += QLatin1Char('/') + segment;
        const QFileInfo info(path);
        if (info.isSymLink()) {
            error = tr("links are not followed out of the shared folder");
            return {};
        }
        if (!info.exists()) {
            error = tr("no such file or folder");
            return {};
        }
    }

    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty()
        || (canonical != rootCanonical
            && !canonical.startsWith(rootCanonical + QLatin1Char('/')))) {
        error = tr("not a path inside the shared folder");
        return {};
    }
    return canonical;
}

QJsonArray SharedFolder::list(const QString &relative, QString &error) const {
    const QString path = resolve(relative, error);
    if (path.isEmpty()) {
        return {};
    }
    if (!QFileInfo(path).isDir()) {
        error = tr("not a folder");
        return {};
    }

    QFileInfoList infos = QDir(path).entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    QJsonArray entries;
    for (const QFileInfo &info : infos) {
        if (entries.size() >= kMaxEntries) {
            break;
        }
        const QString name = info.fileName();
        if (name.startsWith(QLatin1Char('.')) || info.isSymLink() || !cleanName(name)) {
            continue;
        }
        const bool dir = info.isDir();
        if (!dir && !info.isFile()) {
            continue; // sockets, fifos, devices
        }
        QJsonObject entry;
        entry.insert(QLatin1StringView("name"), name);
        entry.insert(QLatin1StringView("dir"), dir);
        entry.insert(QLatin1StringView("size"), dir ? 0.0 : static_cast<double>(info.size()));
        entry.insert(QLatin1StringView("mtime"),
                     static_cast<double>(info.lastModified().toMSecsSinceEpoch()));
        entries.append(entry);
    }
    return entries;
}

QString SharedFolder::fileForFetch(const QString &relative, QString &error) const {
    if (relative.isEmpty()) {
        error = tr("not a file");
        return {};
    }
    const QString path = resolve(relative, error);
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink()) {
        error = tr("not a file");
        return {};
    }
    if (info.size() > limits::kMaxFileBytes) {
        error = tr("that file is too large to send");
        return {};
    }
    if (!info.isReadable()) {
        error = tr("that file cannot be read");
        return {};
    }
    return path;
}

} // namespace mazeconnect::core
