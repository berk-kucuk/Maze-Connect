#include "DeviceListModel.h"

using namespace mazeconnect::core;

DeviceListModel::DeviceListModel(QObject *parent) : QAbstractListModel(parent) {}

void DeviceListModel::setManager(DeviceManager *manager) {
    m_manager = manager;
    if (m_manager) {
        connect(m_manager, &DeviceManager::deviceListChanged, this, &DeviceListModel::refresh);
        connect(m_manager, &DeviceManager::deviceConnected, this, &DeviceListModel::refresh);
        connect(m_manager, &DeviceManager::deviceDisconnected, this, &DeviceListModel::refresh);
    }
    refresh();
}

void DeviceListModel::refresh() {
    beginResetModel();
    m_rows.clear();

    if (m_manager) {
        for (const PairedDevice &device : m_manager->pairedDevices()) {
            Row row;
            row.deviceId = device.deviceId;
            row.name = device.deviceName;
            row.type = device.deviceType;
            row.paired = true;
            row.connected = m_manager->isConnected(device.deviceId);
            row.fingerprint = device.fingerprint();
            row.capabilities = capabilitiesToNames(device.enabledCapabilities);
            m_rows.append(row);
        }

        for (const DiscoveredDevice &device : m_manager->discoveredDevices()) {
            bool alreadyPaired = false;
            for (const Row &existing : m_rows) {
                if (existing.deviceId == device.deviceId) {
                    alreadyPaired = true;
                    break;
                }
            }
            if (alreadyPaired) {
                continue;
            }
            Row row;
            row.deviceId = device.deviceId;
            row.name = device.deviceName;
            row.type = device.deviceType;
            row.paired = false;
            row.connected = false;
            row.address = QStringLiteral("%1:%2").arg(device.address.toString()).arg(device.port);
            m_rows.append(row);
        }
    }

    endResetModel();
    emit countChanged();
}

int DeviceListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant DeviceListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case DeviceIdRole:
        return row.deviceId;
    case NameRole:
        return row.name;
    case TypeRole:
        return row.type;
    case PairedRole:
        return row.paired;
    case ConnectedRole:
        return row.connected;
    case FingerprintRole:
        return row.fingerprint;
    case AddressRole:
        return row.address;
    case CapabilitiesRole:
        return row.capabilities;
    default:
        return {};
    }
}

QHash<int, QByteArray> DeviceListModel::roleNames() const {
    return {
        {DeviceIdRole, "deviceId"},
        {NameRole, "name"},
        {TypeRole, "deviceType"},
        {PairedRole, "paired"},
        {ConnectedRole, "connected"},
        {FingerprintRole, "fingerprint"},
        {AddressRole, "address"},
        {CapabilitiesRole, "capabilities"},
    };
}
