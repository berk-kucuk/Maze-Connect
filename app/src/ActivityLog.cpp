#include "ActivityLog.h"

ActivityLog::ActivityLog(QObject *parent) : QAbstractListModel(parent) {}

int ActivityLog::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_entries.size());
}

QVariant ActivityLog::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
        return {};
    }
    const Entry &entry = m_entries.at(index.row());
    switch (role) {
    case KindRole:
        return static_cast<int>(entry.kind);
    case TitleRole:
        return entry.title;
    case DetailRole:
        return entry.detail;
    case TimeRole:
        return entry.at.toLocalTime().toString(QStringLiteral("HH:mm:ss"));
    default:
        return {};
    }
}

QHash<int, QByteArray> ActivityLog::roleNames() const {
    return {
        {KindRole, "kind"},
        {TitleRole, "title"},
        {DetailRole, "detail"},
        {TimeRole, "time"},
    };
}

void ActivityLog::append(Kind kind, const QString &title, const QString &detail) {
    // Newest first: the entry a user wants is almost always the last thing
    // that happened.
    beginInsertRows(QModelIndex(), 0, 0);
    m_entries.prepend(Entry{kind, title, detail, QDateTime::currentDateTimeUtc()});
    endInsertRows();

    if (m_entries.size() > kMaxEntries) {
        const int first = kMaxEntries;
        const int last = static_cast<int>(m_entries.size()) - 1;
        beginRemoveRows(QModelIndex(), first, last);
        m_entries.remove(first, last - first + 1);
        endRemoveRows();
    }

    m_unseen += 1;
    emit countChanged();
    emit unseenChanged();
}

void ActivityLog::markAllSeen() {
    if (m_unseen == 0) {
        return;
    }
    m_unseen = 0;
    emit unseenChanged();
}

void ActivityLog::clear() {
    if (m_entries.isEmpty()) {
        return;
    }
    beginResetModel();
    m_entries.clear();
    endResetModel();
    m_unseen = 0;
    emit countChanged();
    emit unseenChanged();
}
