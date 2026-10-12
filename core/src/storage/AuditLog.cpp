#include "mazeconnect/core/AuditLog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace mazeconnect::core {
namespace {

/// A line longer than this was not written by record() and is skipped.
constexpr qsizetype kMaxLineBytes = 4096;

QString clean(const QString &value) {
    QString out;
    out.reserve(std::min<qsizetype>(value.size(), AuditLog::kMaxFieldChars));
    for (const QChar c : value) {
        if (out.size() >= AuditLog::kMaxFieldChars) {
            break;
        }
        const char16_t u = c.unicode();
        const bool control = u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)
            || (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069)
            || u == 0x2028 || u == 0x2029;
        out.append(control ? QChar(u' ') : c);
    }
    return out;
}

void readInto(const QString &path, QList<AuditLog::Entry> &out) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    while (!file.atEnd()) {
        const QByteArray line = file.readLine(kMaxLineBytes + 1);
        if (line.size() > kMaxLineBytes) {
            // The rest of an over-long line is not a line of its own.
            bool ended = line.endsWith('\n');
            while (!ended && !file.atEnd()) {
                ended = file.readLine(kMaxLineBytes + 1).endsWith('\n');
            }
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed());
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject obj = doc.object();
        AuditLog::Entry entry;
        entry.at = QDateTime::fromString(obj.value(QLatin1StringView("at")).toString(),
                                         Qt::ISODate);
        if (!entry.at.isValid()) {
            continue;
        }
        entry.deviceId = clean(obj.value(QLatin1StringView("device")).toString());
        entry.deviceName = clean(obj.value(QLatin1StringView("name")).toString());
        entry.action = clean(obj.value(QLatin1StringView("action")).toString());
        entry.detail = clean(obj.value(QLatin1StringView("detail")).toString());
        entry.ok = obj.value(QLatin1StringView("ok")).toBool(true);
        out.append(entry);
    }
}

} // namespace

AuditLog::AuditLog(QString path) : m_path(std::move(path)) {}

bool AuditLog::record(const QString &deviceId, const QString &deviceName,
                      const QString &action, const QString &detail, bool ok) {
    if (m_path.isEmpty()) {
        return false;
    }
    const QFileInfo info(m_path);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }
    if (info.exists() && info.size() >= kRotateBytes) {
        const QString rotated = m_path + QStringLiteral(".1");
        QFile::remove(rotated);
        QFile::rename(m_path, rotated);
    }

    QJsonObject obj;
    obj.insert(QLatin1StringView("at"),
               QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    obj.insert(QLatin1StringView("device"), clean(deviceId));
    obj.insert(QLatin1StringView("name"), clean(deviceName));
    obj.insert(QLatin1StringView("action"), clean(action));
    if (!detail.isEmpty()) {
        obj.insert(QLatin1StringView("detail"), clean(detail));
    }
    obj.insert(QLatin1StringView("ok"), ok);

    QFile file(m_path);
    const bool fresh = !file.exists();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return false;
    }
    // Owner-only from the first write: which devices did what, and when, is
    // nobody else's business on a shared machine.
    if (fresh) {
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    line.append('\n');
    return file.write(line) == line.size();
}

QList<AuditLog::Entry> AuditLog::recent(int max) const {
    QList<Entry> entries;
    if (max <= 0) {
        return entries;
    }
    readInto(m_path + QStringLiteral(".1"), entries);
    readInto(m_path, entries);
    std::reverse(entries.begin(), entries.end());
    if (entries.size() > max) {
        entries.resize(max);
    }
    return entries;
}

bool AuditLog::clear() {
    const bool a = !QFile::exists(m_path) || QFile::remove(m_path);
    const QString rotated = m_path + QStringLiteral(".1");
    const bool b = !QFile::exists(rotated) || QFile::remove(rotated);
    return a && b;
}

} // namespace mazeconnect::core
