#pragma once

#include <QFile>
#include <QHash>
#include <QObject>
#include <QString>

#include <memory>

namespace mazeconnect::core {

/**
 * Receives incoming files into a confined per-device inbox.
 *
 * Layered defences, because a filename and a byte count from a peer are
 * fully attacker-controlled:
 *  1. The name is reduced to a safe single component (PathSanitizer).
 *  2. Bytes are written to a randomly-named temp file, never to the final
 *     name, so a partially-received transfer cannot be mistaken for a
 *     complete one and a race on the final name has nothing to win.
 *  3. Actual bytes written are counted independently of the announced size;
 *     a peer that sends more than it declared is cut off.
 *  4. After the rename, the resulting path is re-resolved and verified to
 *     still be inside the inbox — this is what catches a pre-existing
 *     symlink that a name check cannot see.
 */
class FileTransferReceiver : public QObject {
    Q_OBJECT

public:
    struct Incoming {
        quint32 transferId = 0;
        QString requestedName;   ///< as sent by the peer, for display only
        QString sanitizedName;
        qint64 declaredSize = 0;
        qint64 receivedBytes = 0;
        QString tempPath;
        QString finalPath;
        std::unique_ptr<QFile> file;
    };

    explicit FileTransferReceiver(QString inboxRoot, QObject *parent = nullptr);
    ~FileTransferReceiver() override;

    /// Inbox directory; created on demand with owner-only permissions.
    QString inboxRoot() const { return m_inboxRoot; }

    /**
     * Validate an offer. Returns false (with @p reason set) if the filename
     * is unsafe, the size is out of bounds, or the id is already in use.
     * Accepting an offer does not create anything yet — that happens in
     * begin(), after the user consents.
     */
    bool validateOffer(quint32 transferId,
                       const QString &filename,
                       qint64 sizeBytes,
                       QString &reason) const;

    /// Open the temp file for an offer the user accepted.
    bool begin(quint32 transferId, const QString &filename, qint64 sizeBytes, QString &reason);

    /// Append a received chunk. Returns false on any violation; the transfer
    /// is aborted and its partial file removed.
    bool appendChunk(quint32 transferId, const QByteArray &chunk, QString &reason);

    /// Finalise: verify size, rename into place, re-check confinement.
    bool finish(quint32 transferId, QString &finalPath, QString &reason);

    /// Abort and delete any partial file.
    void cancel(quint32 transferId);
    void cancelAll();

    bool hasTransfer(quint32 transferId) const { return m_transfers.contains(transferId); }
    int activeCount() const { return static_cast<int>(m_transfers.size()); }

signals:
    void progress(quint32 transferId, qint64 receivedBytes, qint64 totalBytes);

private:
    bool ensureInbox() const;

    QString m_inboxRoot;
    QHash<quint32, std::shared_ptr<Incoming>> m_transfers;
};

} // namespace mazeconnect::core
