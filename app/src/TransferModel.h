#pragma once

#include <QAbstractListModel>
#include <QQmlEngine>

/**
 * Live and finished file transfers.
 *
 * DeviceManager already emits progress and completion for every transfer;
 * without this the user could accept a large file and then have no way to
 * tell whether it was still arriving, had finished, or had failed.
 */
class TransferModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by Backend")

    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY countChanged)

public:
    enum State {
        Running,
        Completed,
        Failed,
    };
    Q_ENUM(State)

    enum Roles {
        TransferIdRole = Qt::UserRole + 1,
        DeviceNameRole,
        FilenameRole,
        ReceivedRole,
        TotalRole,
        ProgressRole,
        StateRole,
        DetailRole,
        IncomingRole,
    };

    explicit TransferModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(m_rows.size()); }
    int activeCount() const;

    void started(quint32 transferId, const QString &deviceName, const QString &filename,
                 qint64 totalBytes, bool incoming);
    void progressed(quint32 transferId, qint64 received, qint64 total);
    void finished(quint32 transferId, const QString &path);

    /**
     * Complete the running incoming transfer whose name matches.
     *
     * DeviceManager::fileReceived reports the final path but not the
     * transfer id, and the saved name may have been de-duplicated
     * ("report (2).pdf"), so match on the offered name first and fall back
     * to the only running incoming transfer.
     */
    void finishByFilename(const QString &savedName, const QString &path);
    void failed(quint32 transferId, const QString &reason);

public slots:
    /// Drop finished and failed rows, keeping anything still running.
    void clearFinished();

signals:
    void countChanged();

private:
    struct Row {
        quint32 transferId = 0;
        QString deviceName;
        QString filename;
        qint64 received = 0;
        qint64 total = 0;
        State state = Running;
        QString detail;
        bool incoming = true;
    };

    int indexOf(quint32 transferId) const;

    QList<Row> m_rows;
};
