#include "mazeconnect/core/Beacon.h"

#include "mazeconnect/core/Limits.h"
#include "mazeconnect/core/Version.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QUdpSocket>

namespace mazeconnect::core {
namespace {

constexpr int kAnnounceIntervalMs = 5000;
constexpr int kPruneIntervalMs = 5000;

// A device is considered gone after roughly three missed announcements.
constexpr qint64 kStaleAfterSecs = 17;

// Minimum spacing between datagrams we will actually act on from one source.
constexpr qint64 kMinAcceptIntervalMs = 1000;

// Anyone on the LAN can send beacons, one per made-up device id. Without a
// ceiling both tables below grew by one entry per forged id for as long as
// the sender kept going; a home network has a handful of real devices.
constexpr int kMaxDevices = 64;
constexpr int kMaxRateEntries = 256;

bool hasControlCharacters(const QString &s) {
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)) {
            return true;
        }
    }
    return false;
}

QString boundedString(const QJsonObject &obj, QLatin1StringView key, int maxChars) {
    const QJsonValue value = obj.value(key);
    if (!value.isString()) {
        return {};
    }
    const QString s = value.toString();
    if (s.isEmpty() || s.size() > maxChars || hasControlCharacters(s)) {
        return {};
    }
    return s;
}

/**
 * An interface the local network is actually on.
 *
 * VPN tunnels are the reason this exists. With ProtonVPN (or any full-tunnel
 * VPN) up, the kernel routes the multicast group through the tunnel, because
 * the tunnel owns the default route — so an announcement sent "to the group"
 * went into the VPN and never reached the Wi-Fi the phone is on. The phone
 * simply never saw this computer, while the firewall, the port and the app
 * were all fine. Announcements now leave through each LAN interface by name.
 */
bool isLanInterface(const QNetworkInterface &iface) {
    const auto flags = iface.flags();
    if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
        || flags.testFlag(QNetworkInterface::IsLoopBack)
        || flags.testFlag(QNetworkInterface::IsPointToPoint)) {
        return false;
    }
    static const char *const kVirtual[] = {"tun", "tap", "wg", "ppp", "pvpn", "proton", "nordlynx",
                                           "vpn", "docker", "veth", "virbr", "br-", "vmnet",
                                           "tailscale", "zt"};
    const QString name = iface.name();
    for (const char *prefix : kVirtual) {
        if (name.startsWith(QLatin1StringView(prefix))) {
            return false;
        }
    }
    return true;
}

} // namespace

const char *Beacon::kMulticastGroup = "239.255.83.10";

Beacon::Beacon(QObject *parent) : QObject(parent) {
    m_announceTimer.setInterval(kAnnounceIntervalMs);
    connect(&m_announceTimer, &QTimer::timeout, this, &Beacon::sendAnnounce);

    m_pruneTimer.setInterval(kPruneIntervalMs);
    connect(&m_pruneTimer, &QTimer::timeout, this, &Beacon::pruneStale);
}

Beacon::~Beacon() {
    stop();
}

bool Beacon::isRunning() const {
    return m_socket != nullptr;
}

bool Beacon::start(const QString &deviceId,
                   const QString &deviceName,
                   const QString &deviceType,
                   quint16 servicePort) {
    stop();

    m_deviceId = deviceId;
    m_deviceName = deviceName;
    m_deviceType = deviceType;
    m_servicePort = servicePort;

    m_socket = new QUdpSocket(this);
    // ShareAddress so several Maze apps (or a second user session) can listen
    // on the same port instead of the first one winning silently.
    if (!m_socket->bind(QHostAddress::AnyIPv4, kPort,
                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        delete m_socket;
        m_socket = nullptr;
        return false;
    }

    const QHostAddress group(QString::fromLatin1(kMulticastGroup));
    bool joinedAny = false;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if (!isLanInterface(iface)
            || !iface.flags().testFlag(QNetworkInterface::CanMulticast)) {
            continue;
        }
        if (m_socket->joinMulticastGroup(group, iface)) {
            joinedAny = true;
        }
    }
    // Not fatal: on a network where multicast is filtered we still listen,
    // and the UI can fall back to the user entering an address by hand.
    Q_UNUSED(joinedAny);

    connect(m_socket, &QUdpSocket::readyRead, this, &Beacon::readPendingDatagrams);

    m_announceTimer.start();
    m_pruneTimer.start();
    sendAnnounce();
    return true;
}

void Beacon::stop() {
    m_announceTimer.stop();
    m_pruneTimer.stop();
    if (m_socket) {
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_devices.clear();
    m_lastAccepted.clear();
}

void Beacon::announceNow() {
    sendAnnounce();
}

bool Beacon::refresh() {
    if (m_deviceId.isEmpty()) {
        return false;
    }
    // Copied before start(), which calls stop() and clears them.
    const QString deviceId = m_deviceId;
    const QString deviceName = m_deviceName;
    const QString deviceType = m_deviceType;
    const quint16 servicePort = m_servicePort;
    return start(deviceId, deviceName, deviceType, servicePort);
}

void Beacon::sendAnnounce() {
    if (!m_socket) {
        return;
    }
    QJsonObject obj;
    obj.insert(QLatin1StringView("v"), kProtocolVersion);
    obj.insert(QLatin1StringView("deviceId"), m_deviceId);
    obj.insert(QLatin1StringView("deviceName"), m_deviceName);
    obj.insert(QLatin1StringView("deviceType"), m_deviceType);
    obj.insert(QLatin1StringView("port"), m_servicePort);

    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    const QHostAddress group(QString::fromLatin1(kMulticastGroup));

    // Out of every LAN interface by name, never "wherever the route goes" —
    // see isLanInterface() for why that was into a VPN tunnel. And also as a
    // subnet broadcast: plenty of home routers filter multicast between
    // Wi-Fi and Ethernet (IGMP snooping with no querier), and a broadcast to
    // the subnet's own address crosses them. The phone listens on the same
    // port for both; duplicate copies are absorbed by its rate limit.
    bool sentAny = false;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if (!isLanInterface(iface)) {
            continue;
        }
        bool hasIpv4 = false;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            hasIpv4 = true;
            const QHostAddress broadcast = entry.broadcast();
            if (!broadcast.isNull()
                && iface.flags().testFlag(QNetworkInterface::CanBroadcast)) {
                m_socket->writeDatagram(payload, broadcast, kPort);
            }
        }
        if (hasIpv4 && iface.flags().testFlag(QNetworkInterface::CanMulticast)) {
            m_socket->setMulticastInterface(iface);
            m_socket->writeDatagram(payload, group, kPort);
            sentAny = true;
        }
    }
    if (!sentAny) {
        // No LAN interface we recognise: fall back to the routing table.
        m_socket->setMulticastInterface(QNetworkInterface());
        m_socket->writeDatagram(payload, group, kPort);
    }
}

void Beacon::readPendingDatagrams() {
    if (!m_socket) {
        return;
    }
    while (m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket->receiveDatagram(limits::kMaxBeaconDatagram + 1);
        const QByteArray payload = datagram.data();

        // Anything bigger than a beacon has no business here.
        if (payload.isEmpty() || payload.size() > limits::kMaxBeaconDatagram) {
            continue;
        }

        QJsonParseError error{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }
        const QJsonObject obj = doc.object();

        if (obj.value(QLatin1StringView("v")).toInt(-1) != kProtocolVersion) {
            continue;
        }

        const QString deviceId = boundedString(obj, QLatin1StringView("deviceId"),
                                               limits::kMaxDeviceIdChars);
        const QString deviceName = boundedString(obj, QLatin1StringView("deviceName"),
                                                 limits::kMaxDeviceNameChars);
        const QString deviceType = boundedString(obj, QLatin1StringView("deviceType"), 16);
        if (deviceId.isEmpty() || deviceName.isEmpty()) {
            continue;
        }
        if (deviceType != QLatin1StringView("desktop") && deviceType != QLatin1StringView("mobile")) {
            continue;
        }
        if (deviceId == m_deviceId) {
            continue; // our own announcement echoed back
        }

        const int port = obj.value(QLatin1StringView("port")).toInt(-1);
        if (port <= 0 || port > 65535) {
            continue;
        }

        // Per-source rate limit, keyed on the claimed id *and* the source
        // address so spoofing one field alone does not bypass it.
        if (!m_devices.contains(deviceId) && m_devices.size() >= kMaxDevices) {
            continue;
        }
        const QString sourceKey = deviceId + u'@' + datagram.senderAddress().toString();
        if (!m_lastAccepted.contains(sourceKey) && m_lastAccepted.size() >= kMaxRateEntries) {
            continue;
        }
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const auto lastIt = m_lastAccepted.constFind(sourceKey);
        if (lastIt != m_lastAccepted.constEnd()
            && lastIt.value().msecsTo(now) < kMinAcceptIntervalMs) {
            continue;
        }
        m_lastAccepted.insert(sourceKey, now);

        DiscoveredDevice device;
        device.deviceId = deviceId;
        device.deviceName = deviceName;
        device.deviceType = deviceType;
        device.address = datagram.senderAddress();
        device.port = static_cast<quint16>(port);
        device.lastSeen = now;

        const bool isNew = !m_devices.contains(deviceId);
        m_devices.insert(deviceId, device);
        if (isNew) {
            emit deviceDiscovered(device);
        }
    }
}

void Beacon::pruneStale() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto keys = m_devices.keys();
    for (const QString &id : keys) {
        if (m_devices.value(id).lastSeen.secsTo(now) > kStaleAfterSecs) {
            m_devices.remove(id);
            emit deviceLost(id);
        }
    }
    for (auto it = m_lastAccepted.begin(); it != m_lastAccepted.end();) {
        it = it.value().secsTo(now) > kStaleAfterSecs ? m_lastAccepted.erase(it) : std::next(it);
    }
}

QList<DiscoveredDevice> Beacon::devices() const {
    QList<DiscoveredDevice> list = m_devices.values();
    std::sort(list.begin(), list.end(),
              [](const DiscoveredDevice &a, const DiscoveredDevice &b) {
                  return a.lastSeen > b.lastSeen;
              });
    return list;
}

} // namespace mazeconnect::core
