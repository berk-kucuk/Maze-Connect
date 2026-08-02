#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QQmlEngine>

/**
 * A running record of what the link layer did.
 *
 * This exists mostly for the security entries. The daemon already refuses
 * unpaired peers, replayed counters and malformed frames; without somewhere
 * to show that, a user on a hostile network gets no signal that anything
 * was attempted. Kept in memory only — it is a window into the current
 * session, not an audit trail, and writing it to disk would create a record
 * of who was nearby that the app has no reason to keep.
 */
class ActivityLog : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by Backend")

    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int unseen READ unseen NOTIFY unseenChanged)

public:
    enum Kind {
        Info,
        Security,
        Transfer,
        Pairing,
    };
    Q_ENUM(Kind)

    enum Roles {
        KindRole = Qt::UserRole + 1,
        TitleRole,
        DetailRole,
        TimeRole,
    };

    explicit ActivityLog(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(m_entries.size()); }
    int unseen() const { return m_unseen; }

    void append(Kind kind, const QString &title, const QString &detail = QString());

public slots:
    void markAllSeen();
    void clear();

signals:
    void countChanged();
    void unseenChanged();

private:
    struct Entry {
        Kind kind;
        QString title;
        QString detail;
        QDateTime at;
    };

    /// Bounded so a peer that triggers a lot of refusals cannot grow this
    /// without limit.
    static constexpr int kMaxEntries = 200;

    QList<Entry> m_entries;
    int m_unseen = 0;
};
