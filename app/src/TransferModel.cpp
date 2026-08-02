#include "TransferModel.h"

#include <QFileInfo>

TransferModel::TransferModel(QObject *parent) : QAbstractListModel(parent) {}

int TransferModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int TransferModel::activeCount() const {
    int active = 0;
    for (const Row &row : m_rows) {
        if (row.state == Running) {
            active += 1;
        }
    }
    return active;
}

int TransferModel::indexOf(quint32 transferId) const {
    for (qsizetype i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].transferId == transferId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

QVariant TransferModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case TransferIdRole:
        return row.transferId;
    case DeviceNameRole:
        return row.deviceName;
    case FilenameRole:
        return row.filename;
    case ReceivedRole:
        return row.received;
    case TotalRole:
        return row.total;
    case ProgressRole:
        // A zero-byte file is complete the moment it starts; reporting 0%
        // for it would leave a row that never appears to finish.
        return row.total > 0 ? static_cast<double>(row.received) / row.total : 1.0;
    case StateRole:
        return static_cast<int>(row.state);
    case DetailRole:
        return row.detail;
    case IncomingRole:
        return row.incoming;
    default:
        return {};
    }
}

QHash<int, QByteArray> TransferModel::roleNames() const {
    return {
        {TransferIdRole, "transferId"},
        {DeviceNameRole, "deviceName"},
        {FilenameRole, "filename"},
        {ReceivedRole, "received"},
        {TotalRole, "total"},
        {ProgressRole, "progress"},
        // Not "state": on a QML delegate that shadows QQuickItem::state, which is
// a string. The two would silently mean different things depending on
// which one a binding happened to resolve.
        {StateRole, "transferState"},
        {DetailRole, "detail"},
        {IncomingRole, "incoming"},
    };
}

void TransferModel::started(quint32 transferId, const QString &deviceName,
                            const QString &filename, qint64 totalBytes, bool incoming) {
    if (indexOf(transferId) >= 0) {
        return;
    }
    beginInsertRows(QModelIndex(), 0, 0);
    m_rows.prepend(Row{transferId, deviceName, filename, 0, totalBytes, Running, {}, incoming});
    endInsertRows();
    emit countChanged();
}

void TransferModel::progressed(quint32 transferId, qint64 received, qint64 total) {
    const int i = indexOf(transferId);
    if (i < 0) {
        return;
    }
    m_rows[i].received = received;
    if (total > 0) {
        m_rows[i].total = total;
    }
    const QModelIndex idx = index(i);
    emit dataChanged(idx, idx, {ReceivedRole, TotalRole, ProgressRole});
}

void TransferModel::finished(quint32 transferId, const QString &path) {
    const int i = indexOf(transferId);
    if (i < 0) {
        return;
    }
    m_rows[i].state = Completed;
    m_rows[i].received = m_rows[i].total;
    m_rows[i].detail = path;
    const QModelIndex idx = index(i);
    emit dataChanged(idx, idx, {StateRole, DetailRole, ReceivedRole, ProgressRole});
    emit countChanged();
}

void TransferModel::finishByFilename(const QString &savedName, const QString &path) {
    int match = -1;
    int runningIncoming = 0;
    int lastRunningIncoming = -1;

    for (qsizetype i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].state != Running || !m_rows[i].incoming) {
            continue;
        }
        runningIncoming += 1;
        lastRunningIncoming = static_cast<int>(i);
        if (m_rows[i].filename == savedName) {
            match = static_cast<int>(i);
            break;
        }
    }

    // De-duplication may have renamed the file on disk; if exactly one
    // incoming transfer is in flight there is no ambiguity about which
    // finished.
    if (match < 0 && runningIncoming == 1) {
        match = lastRunningIncoming;
    }
    if (match < 0) {
        return;
    }
    finished(m_rows[match].transferId, path);
}

void TransferModel::failed(quint32 transferId, const QString &reason) {
    const int i = indexOf(transferId);
    if (i < 0) {
        return;
    }
    m_rows[i].state = Failed;
    m_rows[i].detail = reason;
    const QModelIndex idx = index(i);
    emit dataChanged(idx, idx, {StateRole, DetailRole});
    emit countChanged();
}

void TransferModel::clearFinished() {
    for (qsizetype i = m_rows.size() - 1; i >= 0; --i) {
        if (m_rows[i].state == Running) {
            continue;
        }
        beginRemoveRows(QModelIndex(), static_cast<int>(i), static_cast<int>(i));
        m_rows.removeAt(i);
        endRemoveRows();
    }
    emit countChanged();
}
