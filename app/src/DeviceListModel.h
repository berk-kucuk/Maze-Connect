#pragma once

#include <QAbstractListModel>
#include <QQmlEngine>

#include "mazeconnect/core/DeviceManager.h"

/**
 * Flattens paired and discovered devices into one list for the UI.
 *
 * Paired devices come first and always appear, connected or not — a device
 * you have trusted should not vanish from the list just because it is
 * asleep. Unpaired devices seen on the LAN follow, clearly marked, because
 * everything about them is unauthenticated hearsay until pairing completes.
 */
class DeviceListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by Backend")

    // QAbstractListModel does not expose rowCount to QML, so `count` has to
    // be declared explicitly or every binding on it reads undefined.
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        DeviceIdRole = Qt::UserRole + 1,
        NameRole,
        TypeRole,
        PairedRole,
        ConnectedRole,
        FingerprintRole,
        AddressRole,
        CapabilitiesRole,
    };

    explicit DeviceListModel(QObject *parent = nullptr);

    void setManager(mazeconnect::core::DeviceManager *manager);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(m_rows.size()); }

signals:
    void countChanged();

public slots:
    void refresh();

private:
    struct Row {
        QString deviceId;
        QString name;
        QString type;
        bool paired = false;
        bool connected = false;
        QString fingerprint;
        QString address;
        QStringList capabilities;
    };

    mazeconnect::core::DeviceManager *m_manager = nullptr;
    QList<Row> m_rows;
};
