#pragma once

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QMetaType>
#include <QObject>
#include <QTimer>

class QUdpSocket;

namespace mazeconnect::core {

/// A device seen on the LAN but not necessarily paired or trusted.
struct DiscoveredDevice {
    QString deviceId;
    QString deviceName;
    QString deviceType;
    QHostAddress address;
    quint16 port = 0;
    QDateTime lastSeen;
};

/**
 * UDP multicast presence beacon.
 *
 * Everything this class produces is UNAUTHENTICATED and must be treated as
 * a hint for the UI only. A beacon can be spoofed by anyone on the LAN, so
 * the name and id it carries are display strings, never authorisation. The
 * only thing acted on is the address/port, and only to *attempt* a
 * connection that then has to pass mutual TLS and pinning.
 */
class Beacon : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kPort = 38271;
    static const char *kMulticastGroup; // 239.255.83.10

    explicit Beacon(QObject *parent = nullptr);
    ~Beacon() override;

    /**
     * Start announcing and listening.
     *
     * @param deviceId    our stable id
     * @param deviceName  our display name
     * @param deviceType  "desktop" or "mobile"
     * @param servicePort the TCP port our TLS listener is bound to
     */
    bool start(const QString &deviceId,
               const QString &deviceName,
               const QString &deviceType,
               quint16 servicePort);
    void stop();
    bool isRunning() const;

    /// Devices seen recently, freshest first.
    QList<DiscoveredDevice> devices() const;

    /// Send one announcement immediately (e.g. when the user hits refresh).
    void announceNow();

    /**
     * Rebind the socket and re-join the group on every interface.
     *
     * A multicast membership belongs to the interface it was joined on, and
     * that set is decided once, at start(). Bring an interface up afterwards
     * — plug in a cable, connect a VPN, hand the machine a new address — and
     * the group is simply not joined there. Restarting is the only reliable
     * fix and it costs nothing.
     *
     * Does nothing before the first start(), which has not chosen an
     * identity to announce yet.
     */
    bool refresh();

signals:
    void deviceDiscovered(const DiscoveredDevice &device);
    void deviceLost(const QString &deviceId);

private slots:
    void readPendingDatagrams();
    void pruneStale();

private:
    void sendAnnounce();

    QUdpSocket *m_socket = nullptr;
    QTimer m_announceTimer;
    QTimer m_pruneTimer;

    QString m_deviceId;
    QString m_deviceName;
    QString m_deviceType;
    quint16 m_servicePort = 0;

    QHash<QString, DiscoveredDevice> m_devices;

    // Cheap per-source flood guard: a peer that blasts datagrams should not
    // be able to make us re-parse and re-emit without bound.
    QHash<QString, QDateTime> m_lastAccepted;
};

} // namespace mazeconnect::core

// Travels through queued signal/slot connections.
Q_DECLARE_METATYPE(mazeconnect::core::DiscoveredDevice)
