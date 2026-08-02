#include "mazeconnect/core/PathSanitizer.h"

#include "mazeconnect/core/Limits.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace mazeconnect::core {
namespace {

// Reserved on Windows even with an extension ("CON.txt" is still CON).
// Checked because an inbox may be on a shared/exported filesystem.
const QSet<QString> &reservedNames() {
    static const QSet<QString> names = {
        QStringLiteral("con"), QStringLiteral("prn"), QStringLiteral("aux"),
        QStringLiteral("nul"),
        QStringLiteral("com1"), QStringLiteral("com2"), QStringLiteral("com3"),
        QStringLiteral("com4"), QStringLiteral("com5"), QStringLiteral("com6"),
        QStringLiteral("com7"), QStringLiteral("com8"), QStringLiteral("com9"),
        QStringLiteral("lpt1"), QStringLiteral("lpt2"), QStringLiteral("lpt3"),
        QStringLiteral("lpt4"), QStringLiteral("lpt5"), QStringLiteral("lpt6"),
        QStringLiteral("lpt7"), QStringLiteral("lpt8"), QStringLiteral("lpt9"),
    };
    return names;
}

bool hasControlCharacters(const QString &s) {
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        // C0 (including NUL), DEL, and C1.
        if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)) {
            return true;
        }
    }
    return false;
}

} // namespace

QString PathSanitizer::sanitizeFilename(const QString &rawName) {
    // Normalize first: ".." can be spelled with combining characters or
    // fullwidth forms, and we want the dot-check below to see the canonical
    // form rather than a lookalike.
    const QString name = rawName.normalized(QString::NormalizationForm_C).trimmed();

    if (name.isEmpty() || name.size() > limits::kMaxFilenameChars) {
        return {};
    }
    if (hasControlCharacters(name)) {
        return {};
    }
    // No directory component may survive — the caller always joins this onto
    // the inbox path itself.
    if (name.contains(u'/') || name.contains(u'\\')) {
        return {};
    }
    if (name == QStringLiteral(".") || name == QStringLiteral("..")) {
        return {};
    }
    // A name that is nothing but dots is not useful and behaves oddly across
    // filesystems.
    if (QString(name).remove(u'.').isEmpty()) {
        return {};
    }
    // Would be parsed as an option by anything that later shells out.
    if (name.startsWith(u'-')) {
        return {};
    }

    const QString stem = name.section(u'.', 0, 0).toLower();
    if (reservedNames().contains(stem)) {
        return {};
    }

    // Trailing dots/spaces are silently stripped by some filesystems, which
    // would make the name we validated differ from the name created.
    if (name.endsWith(u'.') || name.endsWith(u' ')) {
        return {};
    }

    return name;
}

bool PathSanitizer::isWithinRoot(const QString &candidatePath, const QString &confinementRoot) {
    if (candidatePath.isEmpty() || confinementRoot.isEmpty()) {
        return false;
    }

    // canonicalFilePath() resolves symlinks and "..", and returns empty for
    // anything that does not exist — which is why this is a post-write check.
    const QString canonicalRoot = QFileInfo(confinementRoot).canonicalFilePath();
    const QString canonicalCandidate = QFileInfo(candidatePath).canonicalFilePath();
    if (canonicalRoot.isEmpty() || canonicalCandidate.isEmpty()) {
        return false;
    }

    if (canonicalCandidate == canonicalRoot) {
        return false; // the root itself is not a valid destination file
    }

    // Compare on path boundaries so "/inbox-evil" is not accepted as being
    // inside "/inbox".
    const QString rootWithSep = canonicalRoot.endsWith(u'/') ? canonicalRoot : canonicalRoot + u'/';
    return canonicalCandidate.startsWith(rootWithSep);
}

QString PathSanitizer::uniqueNameIn(const QString &directory, const QString &sanitizedName) {
    if (sanitizedName.isEmpty()) {
        return {};
    }
    const QDir dir(directory);
    if (!dir.exists(sanitizedName)) {
        return sanitizedName;
    }

    const QFileInfo info(sanitizedName);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();

    // Bounded: a peer must not be able to make us stat indefinitely.
    for (int i = 2; i < 1000; ++i) {
        const QString candidate = suffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(base).arg(i)
            : QStringLiteral("%1 (%2).%3").arg(base).arg(i).arg(suffix);
        if (!dir.exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

} // namespace mazeconnect::core
