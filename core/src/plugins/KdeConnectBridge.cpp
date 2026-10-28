#include "mazeconnect/core/KdeConnectBridge.h"

#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QUrl>

namespace mazeconnect::core {

Q_LOGGING_CATEGORY(lcKde, "maze.connect.kdeconnect")

namespace {

constexpr QLatin1StringView kService("org.kde.kdeconnect");
constexpr QLatin1StringView kModulePath("/modules/kdeconnect");
constexpr QLatin1StringView kDevicesPath("/modules/kdeconnect/devices/");
constexpr QLatin1StringView kDaemonInterface("org.kde.kdeconnect.daemon");
constexpr QLatin1StringView kDeviceInterface("org.kde.kdeconnect.device");
constexpr QLatin1StringView kBatteryInterface("org.kde.kdeconnect.device.battery");
constexpr QLatin1StringView kPingInterface("org.kde.kdeconnect.device.ping");
constexpr QLatin1StringView kFindInterface("org.kde.kdeconnect.device.findmyphone");
constexpr QLatin1StringView kShareInterface("org.kde.kdeconnect.device.share");
constexpr QLatin1StringView kClipboardInterface("org.kde.kdeconnect.device.clipboard");
constexpr QLatin1StringView kPropertiesInterface("org.freedesktop.DBus.Properties");

constexpr int kChangedDebounceMs = 100;
constexpr int kRefreshDebounceMs = 250;

QString configHome() {
    const QString fromEnv = QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME"));
    return fromEnv.isEmpty() ? QDir::homePath() + QStringLiteral("/.config") : fromEnv;
}

QString dataHome() {
    const QString fromEnv = QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME"));
    return fromEnv.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share") : fromEnv;
}

QByteArray readSmall(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.read(64 * 1024);
}

bool isOurs(const QString &path) {
    return readSmall(path).contains(KdeConnectBridge::overrideMarker().toUtf8());
}

/// `Hidden=true` in a desktop entry: the freedesktop way of saying "this
/// autostart entry is switched off".
bool isHiddenEntry(const QString &path) {
    const QList<QByteArray> lines = readSmall(path).split('\n');
    for (const QByteArray &raw : lines) {
        const QByteArray line = raw.trimmed();
        if (line.startsWith("Hidden=")) {
            return line.mid(7).trimmed().toLower() == "true";
        }
    }
    return false;
}

bool writeFile(const QString &path, const QByteArray &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content);
    return file.commit();
}

QString cleanName(const QString &text) {
    QString out;
    out.reserve(qMin(text.size(), int(KdeConnectBridge::kMaxNameChars)));
    for (const QChar c : text) {
        if (out.size() >= KdeConnectBridge::kMaxNameChars) {
            break;
        }
        if (c.category() != QChar::Other_Control && c.category() != QChar::Other_Format) {
            out.append(c);
        }
    }
    return out.trimmed();
}

QString devicePath(const QString &id) {
    return kDevicesPath + id;
}

} // namespace

KdeConnectBridge::KdeConnectBridge(QObject *parent)
    : KdeConnectBridge(QDBusConnection(QString()), parent) {
    m_useSessionBus = true;
}

KdeConnectBridge::KdeConnectBridge(const QDBusConnection &bus, QObject *parent)
    : QObject(parent), m_bus(bus), m_daemonPath(QStringLiteral("/usr/bin/kdeconnectd")) {
    m_changedDebounce.setSingleShot(true);
    m_changedDebounce.setInterval(kChangedDebounceMs);
    connect(&m_changedDebounce, &QTimer::timeout, this, &KdeConnectBridge::changed);

    m_refreshDebounce.setSingleShot(true);
    m_refreshDebounce.setInterval(kRefreshDebounceMs);
    connect(&m_refreshDebounce, &QTimer::timeout, this, &KdeConnectBridge::listDevices);
}

KdeConnectBridge::~KdeConnectBridge() = default;

void KdeConnectBridge::setDaemonPath(const QString &path) {
    m_daemonPath = path;
}

QString KdeConnectBridge::autostartOverridePath() {
    return configHome() + QStringLiteral("/autostart/org.kde.kdeconnect.daemon.desktop");
}

QString KdeConnectBridge::activationOverridePath() {
    return dataHome() + QStringLiteral("/dbus-1/services/org.kde.kdeconnect.service");
}

QString KdeConnectBridge::overrideMarker() {
    return QStringLiteral("# Written by Maze Connect: KDE Connect compatibility is off.");
}

bool KdeConnectBridge::isValidDeviceId(const QString &id) {
    if (id.isEmpty() || id.size() > kMaxIdChars) {
        return false;
    }
    for (const QChar c : id) {
        const char16_t u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
            || (u >= '0' && u <= '9') || u == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool KdeConnectBridge::ensureStarted() {
    if (m_started) {
        return m_available;
    }
    m_started = true;

    if (m_useSessionBus) {
        // Only now, on first use — constructing the bridge must not go looking
        // for a session bus (build chroot, test suite).
        if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS")) {
            return false;
        }
        m_bus = QDBusConnection::sessionBus();
    }
    if (!m_bus.isConnected()) {
        return false;
    }

    m_watcher = new QDBusServiceWatcher(kService, m_bus,
                                        QDBusServiceWatcher::WatchForRegistration
                                            | QDBusServiceWatcher::WatchForUnregistration,
                                        this);
    connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this,
            &KdeConnectBridge::onServiceRegistered);
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this,
            &KdeConnectBridge::onServiceUnregistered);

    // KDE Connect announces changes as plain Qt signals rather than
    // PropertiesChanged. An empty path matches every device's object; any of
    // them only triggers one debounced re-read.
    const struct {
        QLatin1StringView interface;
        const char *name;
    } signalsToWatch[] = {
        {kDaemonInterface, "deviceAdded"},
        {kDaemonInterface, "deviceRemoved"},
        {kDaemonInterface, "deviceVisibilityChanged"},
        {kDaemonInterface, "deviceListChanged"},
        {kDaemonInterface, "pairingRequestsChanged"},
        {kDeviceInterface, "reachableChanged"},
        {kDeviceInterface, "pairStateChanged"},
        {kDeviceInterface, "pluginsChanged"},
        {kDeviceInterface, "nameChanged"},
        {kBatteryInterface, "refreshed"},
    };
    for (const auto &s : signalsToWatch) {
        m_bus.connect(kService, QString(), s.interface, QString::fromLatin1(s.name), this,
                      SLOT(onKdeSignal(QDBusMessage)));
    }

    m_available = true;

    // Is it running right now? Asked of the bus itself, so this never starts it.
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"));
    call << QString(kService);
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<bool> reply = *w;
        if (!reply.isError() && reply.value()) {
            onServiceRegistered();
        }
    });
    return true;
}

bool KdeConnectBridge::isInstalled() const {
    return QFileInfo(m_daemonPath).isExecutable();
}

bool KdeConnectBridge::isBlocked() const {
    const QString autostart = autostartOverridePath();
    if (QFileInfo::exists(autostart) && (isOurs(autostart) || isHiddenEntry(autostart))) {
        return true;
    }
    const QString activation = activationOverridePath();
    return QFileInfo::exists(activation) && isOurs(activation);
}

bool KdeConnectBridge::setEnabled(bool enabled, QString &error) {
    ensureStarted();

    if (!enabled) {
        if (!isInstalled()) {
            return true; // nothing to switch off
        }
        const QByteArray marker = overrideMarker().toUtf8();
        const QByteArray autostart =
            "[Desktop Entry]\n" + marker
            + "\n# Turn it back on in Maze Connect > Settings, or delete this file.\n"
              "Type=Application\n"
              "Name=KDE Connect\n"
              "Exec=/usr/bin/kdeconnectd\n"
              "Hidden=true\n";
        // Exec=/bin/false: the name stays activatable, so a client asking for
        // it gets a prompt failure instead of a daemon that comes back.
        const QByteArray activation =
            marker + "\n# Turn it back on in Maze Connect > Settings, or delete this file.\n"
                     "[D-BUS Service]\n"
                     "Name=org.kde.kdeconnect\n"
                     "Exec=/bin/false\n";
        if (!writeFile(autostartOverridePath(), autostart)
            || !writeFile(activationOverridePath(), activation)) {
            error = tr("could not write the files that keep KDE Connect off");
            return false;
        }
        reloadBusConfig(false);
        quitDaemon();
        scheduleChanged();
        return true;
    }

    if (!isInstalled()) {
        error = tr("KDE Connect is not installed");
        return false;
    }
    const QString activation = activationOverridePath();
    if (QFileInfo::exists(activation) && isOurs(activation)) {
        QFile::remove(activation);
    }
    // Ours, or an entry hidden elsewhere (Plasma's Autostart page): the user
    // has just asked for KDE Connect, so neither should keep it off. An entry
    // someone customised without hiding it is left alone.
    const QString autostart = autostartOverridePath();
    if (QFileInfo::exists(autostart) && (isOurs(autostart) || isHiddenEntry(autostart))) {
        QFile::remove(autostart);
    }
    if (isBlocked()) {
        error = tr("could not remove the files that keep KDE Connect off");
        return false;
    }
    reloadBusConfig(true);
    scheduleChanged();
    return true;
}

void KdeConnectBridge::reloadBusConfig(bool startAfter) {
    if (!m_available) {
        return;
    }
    // The bus reads service files once; without a reload it would keep the
    // activation entry it had before the override was written or removed.
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("ReloadConfig"));
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, startAfter](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                if (w->isError()) {
                    qCWarning(lcKde) << "bus ReloadConfig failed:" << w->error().message();
                }
                if (!startAfter || m_running) {
                    return;
                }
                QDBusMessage start = QDBusMessage::createMethodCall(
                    QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                    QStringLiteral("org.freedesktop.DBus"), QStringLiteral("StartServiceByName"));
                start << QString(kService) << quint32(0);
                auto *sw = new QDBusPendingCallWatcher(m_bus.asyncCall(start, kCallTimeoutMs * 4),
                                                       this);
                connect(sw, &QDBusPendingCallWatcher::finished, this,
                        [](QDBusPendingCallWatcher *s) {
                            s->deleteLater();
                            if (s->isError()) {
                                qCWarning(lcKde) << "could not start kdeconnectd:"
                                                 << s->error().message();
                            }
                        });
            });
}

void KdeConnectBridge::quitDaemon() {
    if (!m_running) {
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(
        kService, QStringLiteral("/MainApplication"),
        QStringLiteral("org.qtproject.Qt.QCoreApplication"), QStringLiteral("quit"));
    call.setAutoStartService(false);
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [](QDBusPendingCallWatcher *w) { w->deleteLater(); });
}

void KdeConnectBridge::refresh() {
    if (!ensureStarted()) {
        scheduleChanged();
        return;
    }
    if (m_running) {
        listDevices();
    } else {
        scheduleChanged();
    }
}

void KdeConnectBridge::onServiceRegistered() {
    m_running = true;
    scheduleChanged();
    scheduleRefresh();
}

void KdeConnectBridge::onServiceUnregistered() {
    m_running = false;
    ++m_generation;
    m_devices.clear();
    scheduleChanged();
}

void KdeConnectBridge::onKdeSignal(const QDBusMessage &) {
    if (m_running) {
        scheduleRefresh();
    }
}

void KdeConnectBridge::scheduleChanged() {
    if (!m_changedDebounce.isActive()) {
        m_changedDebounce.start();
    }
}

void KdeConnectBridge::scheduleRefresh() {
    if (!m_refreshDebounce.isActive()) {
        m_refreshDebounce.start();
    }
}

void KdeConnectBridge::listDevices() {
    if (!m_running) {
        return;
    }
    const quint64 generation = ++m_generation;
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kModulePath, kDaemonInterface,
                                                       QStringLiteral("devices"));
    call << false << false; // every device KDE Connect knows: reachable or paired
    call.setAutoStartService(false);
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                if (generation != m_generation) {
                    return;
                }
                const QDBusPendingReply<QStringList> reply = *w;
                if (reply.isError()) {
                    qCWarning(lcKde) << "device list:" << reply.error().message();
                    return;
                }
                QList<KdeConnectDevice> next;
                for (const QString &id : reply.value()) {
                    if (next.size() >= kMaxDevices) {
                        break;
                    }
                    if (!isValidDeviceId(id)) {
                        continue;
                    }
                    // Keep what is already known, so the list does not blank
                    // out while the fresh reading is on its way.
                    const KdeConnectDevice *known = deviceById(id);
                    KdeConnectDevice device = known ? *known : KdeConnectDevice{};
                    device.id = id;
                    next.append(device);
                }
                m_devices = next;
                for (const KdeConnectDevice &device : std::as_const(m_devices)) {
                    readDevice(device.id, generation);
                }
                scheduleChanged();
            });
}

void KdeConnectBridge::readDevice(const QString &id, quint64 generation) {
    QDBusMessage call = QDBusMessage::createMethodCall(kService, devicePath(id),
                                                       kPropertiesInterface,
                                                       QStringLiteral("GetAll"));
    call << QString(kDeviceInterface);
    call.setAutoStartService(false);
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, id, generation](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                if (generation != m_generation) {
                    return;
                }
                const QDBusPendingReply<QVariantMap> reply = *w;
                if (reply.isError()) {
                    return;
                }
                const QVariantMap p = reply.value();
                for (KdeConnectDevice &d : m_devices) {
                    if (d.id != id) {
                        continue;
                    }
                    d.name = cleanName(p.value(QStringLiteral("name")).toString());
                    if (d.name.isEmpty()) {
                        d.name = id;
                    }
                    d.type = cleanName(p.value(QStringLiteral("type")).toString());
                    d.reachable = p.value(QStringLiteral("isReachable")).toBool();
                    d.paired = p.value(QStringLiteral("isPaired")).toBool();
                    d.pairRequested = p.value(QStringLiteral("isPairRequested")).toBool();
                    d.pairRequestedByPeer =
                        p.value(QStringLiteral("isPairRequestedByPeer")).toBool();
                    d.verificationKey =
                        cleanName(p.value(QStringLiteral("verificationKey")).toString());
                    if (d.paired) {
                        readPlugins(id, generation);
                    } else {
                        d.canPing = d.canRing = d.canShare = d.canClipboard = false;
                        d.battery = -1;
                        d.charging = false;
                    }
                    scheduleChanged();
                    break;
                }
            });
}

void KdeConnectBridge::readPlugins(const QString &id, quint64 generation) {
    QDBusMessage call = QDBusMessage::createMethodCall(kService, devicePath(id), kDeviceInterface,
                                                       QStringLiteral("loadedPlugins"));
    call.setAutoStartService(false);
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, id, generation](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                if (generation != m_generation) {
                    return;
                }
                const QDBusPendingReply<QStringList> reply = *w;
                if (reply.isError()) {
                    return;
                }
                const QStringList plugins = reply.value();
                for (KdeConnectDevice &d : m_devices) {
                    if (d.id != id) {
                        continue;
                    }
                    d.canPing = plugins.contains(QStringLiteral("kdeconnect_ping"));
                    d.canRing = plugins.contains(QStringLiteral("kdeconnect_findmyphone"));
                    d.canShare = plugins.contains(QStringLiteral("kdeconnect_share"));
                    d.canClipboard = plugins.contains(QStringLiteral("kdeconnect_clipboard"));
                    if (plugins.contains(QStringLiteral("kdeconnect_battery"))) {
                        readBattery(id, generation);
                    } else {
                        d.battery = -1;
                    }
                    scheduleChanged();
                    break;
                }
            });
}

void KdeConnectBridge::readBattery(const QString &id, quint64 generation) {
    QDBusMessage call = QDBusMessage::createMethodCall(
        kService, devicePath(id) + QStringLiteral("/battery"), kPropertiesInterface,
        QStringLiteral("GetAll"));
    call << QString(kBatteryInterface);
    call.setAutoStartService(false);
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, id, generation](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                if (generation != m_generation) {
                    return;
                }
                const QDBusPendingReply<QVariantMap> reply = *w;
                if (reply.isError()) {
                    return;
                }
                const QVariantMap p = reply.value();
                for (KdeConnectDevice &d : m_devices) {
                    if (d.id != id) {
                        continue;
                    }
                    bool ok = false;
                    const int charge = p.value(QStringLiteral("charge")).toInt(&ok);
                    d.battery = (ok && charge >= 0 && charge <= 100) ? charge : -1;
                    d.charging = p.value(QStringLiteral("isCharging")).toBool();
                    scheduleChanged();
                    break;
                }
            });
}

const KdeConnectDevice *KdeConnectBridge::deviceById(const QString &id) const {
    for (const KdeConnectDevice &d : m_devices) {
        if (d.id == id) {
            return &d;
        }
    }
    return nullptr;
}

bool KdeConnectBridge::checkPaired(const QString &deviceId, QString &error) const {
    const KdeConnectDevice *d = deviceById(deviceId);
    if (!d) {
        error = tr("KDE Connect does not know this device");
        return false;
    }
    if (!d->paired) {
        error = tr("this device is not paired in KDE Connect");
        return false;
    }
    if (!d->reachable) {
        error = tr("%1 is not reachable").arg(d->name);
        return false;
    }
    return true;
}

bool KdeConnectBridge::sendCall(const QString &path, const QString &interface,
                                const QString &method, const QList<QVariant> &arguments,
                                QString &error) {
    if (!m_running) {
        error = tr("KDE Connect is not running");
        return false;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(kService, path, interface, method);
    call.setArguments(arguments);
    call.setAutoStartService(false);
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, method](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                if (w->isError()) {
                    qCWarning(lcKde) << method << "failed:" << w->error().message();
                }
                scheduleRefresh();
            });
    return true;
}

bool KdeConnectBridge::ping(const QString &deviceId, QString &error) {
    if (!checkPaired(deviceId, error)) {
        return false;
    }
    if (!deviceById(deviceId)->canPing) {
        error = tr("the ping plugin is off for this device in KDE Connect");
        return false;
    }
    return sendCall(devicePath(deviceId) + QStringLiteral("/ping"), kPingInterface,
                    QStringLiteral("sendPing"), {}, error);
}

bool KdeConnectBridge::ring(const QString &deviceId, QString &error) {
    if (!checkPaired(deviceId, error)) {
        return false;
    }
    if (!deviceById(deviceId)->canRing) {
        error = tr("this device cannot be rung through KDE Connect");
        return false;
    }
    return sendCall(devicePath(deviceId) + QStringLiteral("/findmyphone"), kFindInterface,
                    QStringLiteral("ring"), {}, error);
}

bool KdeConnectBridge::shareFile(const QString &deviceId, const QString &localPath,
                                 QString &error) {
    if (!checkPaired(deviceId, error)) {
        return false;
    }
    if (!deviceById(deviceId)->canShare) {
        error = tr("file sharing is off for this device in KDE Connect");
        return false;
    }
    const QFileInfo info(localPath);
    if (!info.isFile() || !info.isReadable()) {
        error = tr("not a readable file");
        return false;
    }
    const QString url = QUrl::fromLocalFile(info.absoluteFilePath()).toString();
    return sendCall(devicePath(deviceId) + QStringLiteral("/share"), kShareInterface,
                    QStringLiteral("shareUrl"), {url}, error);
}

bool KdeConnectBridge::shareText(const QString &deviceId, const QString &text, QString &error) {
    if (!checkPaired(deviceId, error)) {
        return false;
    }
    if (!deviceById(deviceId)->canShare) {
        error = tr("sharing is off for this device in KDE Connect");
        return false;
    }
    if (text.trimmed().isEmpty() || text.size() > kMaxTextChars) {
        error = tr("text is empty or too long");
        return false;
    }
    return sendCall(devicePath(deviceId) + QStringLiteral("/share"), kShareInterface,
                    QStringLiteral("shareText"), {text}, error);
}

bool KdeConnectBridge::sendClipboard(const QString &deviceId, QString &error) {
    if (!checkPaired(deviceId, error)) {
        return false;
    }
    if (!deviceById(deviceId)->canClipboard) {
        error = tr("clipboard sharing is off for this device in KDE Connect");
        return false;
    }
    return sendCall(devicePath(deviceId) + QStringLiteral("/clipboard"), kClipboardInterface,
                    QStringLiteral("sendClipboard"), {}, error);
}

bool KdeConnectBridge::requestPairing(const QString &deviceId, QString &error) {
    const KdeConnectDevice *d = deviceById(deviceId);
    if (!d) {
        error = tr("KDE Connect does not know this device");
        return false;
    }
    if (d->paired) {
        error = tr("already paired");
        return false;
    }
    if (!d->reachable) {
        error = tr("%1 is not reachable").arg(d->name);
        return false;
    }
    return sendCall(devicePath(deviceId), kDeviceInterface, QStringLiteral("requestPairing"), {},
                    error);
}

bool KdeConnectBridge::acceptPairing(const QString &deviceId, QString &error) {
    const KdeConnectDevice *d = deviceById(deviceId);
    if (!d || !d->pairRequestedByPeer) {
        error = tr("there is no pairing request from this device");
        return false;
    }
    return sendCall(devicePath(deviceId), kDeviceInterface, QStringLiteral("acceptPairing"), {},
                    error);
}

bool KdeConnectBridge::rejectPairing(const QString &deviceId, QString &error) {
    const KdeConnectDevice *d = deviceById(deviceId);
    if (!d || !(d->pairRequestedByPeer || d->pairRequested)) {
        error = tr("there is no pairing in progress with this device");
        return false;
    }
    // cancelPairing answers "no" to their request, or withdraws ours.
    return sendCall(devicePath(deviceId), kDeviceInterface, QStringLiteral("cancelPairing"), {},
                    error);
}

bool KdeConnectBridge::unpair(const QString &deviceId, QString &error) {
    const KdeConnectDevice *d = deviceById(deviceId);
    if (!d || !d->paired) {
        error = tr("this device is not paired in KDE Connect");
        return false;
    }
    return sendCall(devicePath(deviceId), kDeviceInterface, QStringLiteral("unpair"), {}, error);
}

} // namespace mazeconnect::core
