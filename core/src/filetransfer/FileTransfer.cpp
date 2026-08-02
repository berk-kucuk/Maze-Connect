#include "mazeconnect/core/FileTransfer.h"

#include "mazeconnect/core/Limits.h"
#include "mazeconnect/core/PathSanitizer.h"

#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>

namespace mazeconnect::core {
namespace {

/// Bound on how many transfers one peer can have open at once, so a peer
/// cannot exhaust file descriptors by offering endlessly.
constexpr int kMaxConcurrentTransfers = 8;

QString randomTempName() {
    QByteArray raw(16, Qt::Uninitialized);
    QRandomGenerator::system()->generate(raw.begin(), raw.end());
    return QStringLiteral(".mc-%1.part").arg(QString::fromLatin1(raw.toHex()));
}

} // namespace

FileTransferReceiver::FileTransferReceiver(QString inboxRoot, QObject *parent)
    : QObject(parent), m_inboxRoot(std::move(inboxRoot)) {}

FileTransferReceiver::~FileTransferReceiver() {
    cancelAll();
}

bool FileTransferReceiver::ensureInbox() const {
    // Whether it already existed decides whether we may set its mode. The
    // inbox now lives under the user's Downloads, and tightening a directory
    // they created — or that a previous version of this app shared — is not
    // ours to do silently.
    const bool existed = QFileInfo::exists(m_inboxRoot);

    QDir dir;
    if (!dir.mkpath(m_inboxRoot)) {
        return false;
    }
    if (!existed) {
        // Received files may be anything; a directory we created is kept
        // owner-only rather than inheriting a permissive umask.
        QFile::setPermissions(m_inboxRoot,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                  | QFileDevice::ExeOwner);
    }
    return true;
}

bool FileTransferReceiver::validateOffer(quint32 transferId,
                                         const QString &filename,
                                         qint64 sizeBytes,
                                         QString &reason) const {
    if (m_transfers.contains(transferId)) {
        reason = QStringLiteral("transfer id already in use");
        return false;
    }
    if (m_transfers.size() >= kMaxConcurrentTransfers) {
        reason = QStringLiteral("too many concurrent transfers");
        return false;
    }
    if (sizeBytes < 0 || sizeBytes > limits::kMaxFileBytes) {
        reason = QStringLiteral("declared size out of range");
        return false;
    }
    if (PathSanitizer::sanitizeFilename(filename).isEmpty()) {
        reason = QStringLiteral("unsafe filename");
        return false;
    }
    return true;
}

bool FileTransferReceiver::begin(quint32 transferId,
                                 const QString &filename,
                                 qint64 sizeBytes,
                                 QString &reason) {
    if (!validateOffer(transferId, filename, sizeBytes, reason)) {
        return false;
    }
    if (!ensureInbox()) {
        reason = QStringLiteral("cannot create inbox directory");
        return false;
    }

    auto incoming = std::make_shared<Incoming>();
    incoming->transferId = transferId;
    incoming->requestedName = filename;
    incoming->sanitizedName = PathSanitizer::sanitizeFilename(filename);
    incoming->declaredSize = sizeBytes;
    incoming->tempPath = QDir(m_inboxRoot).filePath(randomTempName());

    incoming->file = std::make_unique<QFile>(incoming->tempPath);
    // NewOnly: never write through an existing file (or a symlink planted
    // at that name) — if the path exists at all, refuse.
    if (!incoming->file->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        reason = QStringLiteral("cannot open temporary file");
        return false;
    }
    incoming->file->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    m_transfers.insert(transferId, incoming);
    return true;
}

bool FileTransferReceiver::appendChunk(quint32 transferId,
                                       const QByteArray &chunk,
                                       QString &reason) {
    const auto it = m_transfers.find(transferId);
    if (it == m_transfers.end()) {
        reason = QStringLiteral("unknown transfer id");
        return false;
    }
    auto &incoming = *it.value();

    // Count what actually arrives rather than trusting the declared size —
    // a peer that overruns its own announcement is cut off here.
    if (incoming.receivedBytes + chunk.size() > incoming.declaredSize) {
        reason = QStringLiteral("peer sent more data than declared");
        cancel(transferId);
        return false;
    }

    if (incoming.file->write(chunk) != chunk.size()) {
        reason = QStringLiteral("write failed");
        cancel(transferId);
        return false;
    }
    incoming.receivedBytes += chunk.size();

    emit progress(transferId, incoming.receivedBytes, incoming.declaredSize);
    return true;
}

bool FileTransferReceiver::finish(quint32 transferId, QString &finalPath, QString &reason) {
    const auto it = m_transfers.find(transferId);
    if (it == m_transfers.end()) {
        reason = QStringLiteral("unknown transfer id");
        return false;
    }
    auto incoming = it.value();

    if (incoming->receivedBytes != incoming->declaredSize) {
        reason = QStringLiteral("size mismatch: expected %1, got %2")
                     .arg(incoming->declaredSize)
                     .arg(incoming->receivedBytes);
        cancel(transferId);
        return false;
    }

    incoming->file->flush();
    incoming->file->close();

    // Pick the final name only now, so nothing occupied it while we wrote.
    const QString unique = PathSanitizer::uniqueNameIn(m_inboxRoot, incoming->sanitizedName);
    if (unique.isEmpty()) {
        reason = QStringLiteral("could not find a free filename");
        cancel(transferId);
        return false;
    }
    const QString target = QDir(m_inboxRoot).filePath(unique);

    if (!QFile::rename(incoming->tempPath, target)) {
        reason = QStringLiteral("rename failed");
        cancel(transferId);
        return false;
    }

    // Last line of defence: resolve what we actually created and confirm it
    // is still inside the inbox. This is what catches a symlink that was
    // already sitting in the directory — no name-level check can.
    if (!PathSanitizer::isWithinRoot(target, m_inboxRoot)) {
        QFile::remove(target);
        m_transfers.erase(it);
        reason = QStringLiteral("final path escaped the inbox — removed");
        return false;
    }

    incoming->finalPath = target;
    finalPath = target;
    m_transfers.erase(it);
    return true;
}

void FileTransferReceiver::cancel(quint32 transferId) {
    const auto it = m_transfers.find(transferId);
    if (it == m_transfers.end()) {
        return;
    }
    auto incoming = it.value();
    if (incoming->file) {
        incoming->file->close();
    }
    // Never leave a partial file behind: it would be indistinguishable from
    // a complete one to anything that later reads the inbox.
    if (!incoming->tempPath.isEmpty()) {
        QFile::remove(incoming->tempPath);
    }
    m_transfers.erase(it);
}

void FileTransferReceiver::cancelAll() {
    const auto ids = m_transfers.keys();
    for (const quint32 id : ids) {
        cancel(id);
    }
}

} // namespace mazeconnect::core
