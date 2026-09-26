#include "Backend.h"

#include "Autostart.h"
#include "mazeconnect/core/Limits.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QUrl>

using namespace mazeconnect::core;

namespace {

/// Shared with the daemon so both binaries agree on where the identity
/// lives — see DeviceManager::defaultDataDir() for why this is not
/// QStandardPaths::AppDataLocation.
QString dataDirectory() {
    return DeviceManager::defaultDataDir();
}

} // namespace

Backend::Backend(QObject *parent) : QObject(parent), m_manager(dataDirectory()) {
    connect(&m_manager, &DeviceManager::pairingRequested, this,
            [this](const PendingPairing &pairing) {
                m_pairing = pairing;
                m_pairingActive = true;
                m_pairingAnswered = false;
                emit pairingChanged();
            });
    connect(&m_manager, &DeviceManager::pairingCompleted, this,
            [this](const QString &deviceId, bool accepted) {
                m_activity.append(ActivityLog::Pairing,
                                  accepted ? tr("Device paired") : tr("Pairing cancelled"),
                                  deviceName(deviceId));
                m_pairingActive = false;
                m_pairingAnswered = false;
                m_pairing = {};
                emit pairingChanged();
                setStatus(accepted ? tr("Device paired.") : tr("Pairing cancelled."));
            });
    connect(&m_manager, &DeviceManager::pairingFailed, this,
            [this](const QString &deviceId, const QString &reason) {
                m_activity.append(ActivityLog::Pairing, tr("Pairing failed"), reason);
                Q_UNUSED(deviceId);
                m_pairingActive = false;
                m_pairingAnswered = false;
                m_pairing = {};
                emit pairingChanged();
                setStatus(tr("Pairing failed: %1").arg(reason));
            });

    connect(&m_manager, &DeviceManager::fileReceived, this,
            [this](const QString &deviceId, quint32 transferId, const QString &path) {
                const QString name = QFileInfo(path).fileName();
                // By id: the saved name can differ from the offered one when
                // it was de-duplicated ("photo (1).jpg"), and matching by name
                // left such rows spinning forever.
                m_transfers.finished(transferId, path);
                m_activity.append(ActivityLog::Transfer, tr("Received %1").arg(name),
                                  deviceName(deviceId));
                setStatus(tr("Received %1").arg(name));
            });
    connect(&m_manager, &DeviceManager::fileFailed, this,
            [this](const QString &deviceId, quint32 transferId, const QString &reason) {
                m_transfers.failed(transferId, reason);
                m_activity.append(ActivityLog::Transfer, tr("Transfer failed"),
                                  QStringLiteral("%1 — %2").arg(deviceName(deviceId), reason));
                setStatus(tr("Transfer failed: %1").arg(reason));
            });
    connect(&m_manager, &DeviceManager::fileOffered, this,
            [this](const PendingFileOffer &offer) {
                emit fileOffered(offer.deviceId, static_cast<int>(offer.transferId),
                                 offer.filename, offer.sizeBytes);
            });
    connect(&m_manager, &DeviceManager::securityAlert, this,
            [this](const QString &summary, const QString &detail) {
                m_activity.append(ActivityLog::Security, summary, detail);
                emit securityAlert(summary, detail);
            });

    // ---- Phones ---------------------------------------------------------
    connect(&m_manager, &DeviceManager::phoneStatusReceived, this, &Backend::onPhoneStatus);
    connect(&m_manager, &DeviceManager::textShared, this, &Backend::onTextShared);
    connect(&m_manager, &DeviceManager::deviceReady, this, [this](const QString &deviceId) {
        // The first reading as soon as the phone can be asked, rather than at
        // the next tick — a dashboard that fills in a minute after the phone
        // connects looks broken for that minute.
        m_manager.requestPhoneStatus(deviceId);
        emit phonesChanged();
    });
    connect(&m_manager, &DeviceManager::findPhoneAnswered, this,
            [this](const QString &deviceId, bool ringing, const QString &error) {
                PhoneState &phone = m_phoneStates[deviceId];
                phone.ringing = ringing;
                phone.ringNotice = error;
                if (!error.isEmpty()) {
                    setStatus(tr("%1: %2").arg(deviceName(deviceId), error));
                }
                emit phonesChanged();
            });

    m_phonePoll.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_phonePoll, &QTimer::timeout, this, &Backend::refreshPhones);
    updatePhonePollInterval();
    // ---- maze-guard -----------------------------------------------------
    connect(m_manager.guardBridge(), &GuardBridge::statusReady, this,
            [this](const QMap<GuardDevice, GuardState> &states) {
                QVariantList list;
                for (auto it = states.cbegin(); it != states.cend(); ++it) {
                    QVariantMap entry;
                    entry.insert(QStringLiteral("device"), guardDeviceName(it.key()));
                    entry.insert(QStringLiteral("state"), guardStateName(it.value()));
                    list.append(entry);
                }
                m_guardDevices = list;
                m_guardError.clear();
                emit guardChanged();
            });
    connect(m_manager.guardBridge(), &GuardBridge::statusFailed, this,
            [this](const QString &reason) {
                m_guardDevices.clear();
                m_guardError = reason;
                emit guardChanged();
            });
    connect(m_manager.guardBridge(), &GuardBridge::killApplied, this,
            [this](GuardDevice device, bool requestedEnabled, GuardState state,
                   const QString &error) {
                const QString name = guardDeviceName(device);
                // Every privileged action is logged, whether it came from
                // this window or from a phone. The Activity page is where a
                // user answers "what changed, and who asked".
                m_activity.append(ActivityLog::Security,
                                  error.isEmpty()
                                      ? tr("%1 is now %2")
                                            .arg(name, state == GuardState::Off ? tr("blocked")
                                                                                : tr("allowed"))
                                      : tr("Could not change %1").arg(name),
                                  error.isEmpty()
                                      ? tr("asked to %1")
                                            .arg(requestedEnabled ? tr("allow") : tr("block"))
                                      : error);
                refreshGuard();
            });

    connect(&m_manager, &DeviceManager::guardChangedRemotely, this,
            [this](const QString &deviceId, const QString &device, bool requestedEnabled,
                   const QString &state, const QString &error) {
                // A banner, not just a log line. A killswitch changing from
                // somewhere else in the house is exactly the thing that must
                // not be quiet on the machine it happened to.
                m_guardBanner = error.isEmpty()
                    ? tr("%1 %2 %3")
                          .arg(deviceName(deviceId),
                               state == QLatin1StringView("off") ? tr("blocked") : tr("allowed"),
                               device)
                    : tr("%1 tried to change %2 — %3").arg(deviceName(deviceId), device, error);
                emit guardBannerChanged();

                m_activity.append(ActivityLog::Security,
                                  tr("Remote killswitch change: %1").arg(device),
                                  tr("%1 asked to %2 %3 — it is now %4")
                                      .arg(deviceName(deviceId),
                                           requestedEnabled ? tr("allow") : tr("block"),
                                           device,
                                           state == QLatin1StringView("off") ? tr("blocked")
                                                                             : tr("allowed")));
                emit securityAlert(tr("Killswitch changed remotely"), m_guardBanner);
            });

    // ---- Commands -------------------------------------------------------
    connect(m_manager.commandRunner(), &CommandRunner::finished, this,
            [this](quint32 requestId, const QString &id, int exitCode, const QString &output,
                   bool timedOut) {
                Q_UNUSED(timedOut);
                // Only runs started from this window. A run a phone asked for
                // has its own answer to deliver and no business appearing
                // here as though the user had pressed something.
                if (!m_localRuns.remove(requestId)) {
                    return;
                }
                m_activity.append(ActivityLog::Info, tr("Ran “%1”").arg(id),
                                  tr("exit %1").arg(exitCode));
                emit commandFinished(id, exitCode, output);
            });

    // ---- Activity + transfer bookkeeping --------------------------------
    connect(&m_manager, &DeviceManager::deviceListChanged, this, &Backend::devicesChanged);
    connect(&m_manager, &DeviceManager::deviceListChanged, this, &Backend::phonesChanged);
    connect(&m_manager, &DeviceManager::deviceConnected, this,
            [this](const QString &deviceId) {
                m_activity.append(ActivityLog::Info, tr("Connected"), deviceName(deviceId));
                emit devicesChanged();
            });
    connect(&m_manager, &DeviceManager::deviceDisconnected, this,
            [this](const QString &deviceId) {
                m_activity.append(ActivityLog::Info, tr("Disconnected"), deviceName(deviceId));
                // The last reading stays — "82% a minute ago" is still worth
                // showing for a phone that just walked out of Wi-Fi range —
                // but it can no longer be ringing as far as this side knows.
                auto it = m_phoneStates.find(deviceId);
                if (it != m_phoneStates.end()) {
                    it->ringing = false;
                }
                emit devicesChanged();
                emit phonesChanged();
            });
    connect(&m_manager, &DeviceManager::fileOffered, this,
            [this](const PendingFileOffer &offer) {
                m_transfers.started(offer.transferId, deviceName(offer.deviceId),
                                    offer.filename, offer.sizeBytes, true);
            });
    connect(&m_manager, &DeviceManager::fileProgress, this,
            [this](const QString &, quint32 transferId, qint64 received, qint64 total) {
                m_transfers.progressed(transferId, received, total);
            });
}

bool Backend::start() {
    const QString name = QHostInfo::localHostName();
    if (!m_manager.start(name, QStringLiteral("desktop"))) {
        setStatus(tr("Could not start. Check that %1 is readable and not corrupt.")
                      .arg(dataDirectory()));
        return false;
    }
    m_devices.setManager(&m_manager);
    emit listenAddressChanged();
    return true;
}

QString Backend::deviceName() const {
    return m_manager.deviceName();
}

QString Backend::fingerprint() const {
    return m_manager.fingerprint();
}

QString Backend::inboxPath() const {
    return m_manager.inboxPath();
}

QString Backend::listenAddress() const {
    const quint16 port = m_manager.listenPort();
    if (port == 0) {
        return {};
    }
    // Prefer a real LAN address over loopback: this string exists to be
    // typed into a phone on the same network. A VPN's tunnel address is not
    // one — it used to be offered first whenever the VPN was up, and a phone
    // on the Wi-Fi cannot reach 10.x.x.x inside someone's tunnel.
    static const char *const kVirtual[] = {"tun", "tap", "wg", "ppp", "pvpn", "proton",
                                           "nordlynx", "vpn", "docker", "veth", "virbr",
                                           "br-", "vmnet", "tailscale", "zt"};
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || flags.testFlag(QNetworkInterface::IsLoopBack)
            || flags.testFlag(QNetworkInterface::IsPointToPoint)) {
            continue;
        }
        bool virtualIface = false;
        for (const char *prefix : kVirtual) {
            virtualIface = virtualIface || iface.name().startsWith(QLatin1StringView(prefix));
        }
        if (virtualIface) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLinkLocal()) {
                return QStringLiteral("%1:%2").arg(address.toString()).arg(port);
            }
        }
    }
    return QStringLiteral("127.0.0.1:%1").arg(port);
}

void Backend::setStatus(const QString &message) {
    m_statusMessage = message;
    emit statusMessageChanged();
}

void Backend::requestPairing(const QString &deviceId) {
    using Start = DeviceManager::PairingStart;
    switch (m_manager.requestPairing(deviceId)) {
    case Start::Started:
        setStatus(tr("Asking that device to pair…"));
        break;
    case Start::NotDiscovered:
        // The common cause is a phone that has stopped announcing itself —
        // so name the thing to check rather than blaming the network.
        setStatus(tr("That device is not announcing itself. Open Maze Connect "
                     "on it and check you are both on the same network."));
        break;
    case Start::AlreadyPaired:
        setStatus(tr("Already paired with that device."));
        break;
    case Start::AlreadyPending:
        setStatus(tr("Already pairing with that device."));
        break;
    }
}

void Backend::acceptPairing() {
    if (m_pairingActive && !m_pairingAnswered) {
        // Marked before the call: pairing only completes once *both* sides
        // have answered, and until then this dialog has to show that the
        // press landed rather than sit there looking inert.
        m_pairingAnswered = true;
        emit pairingChanged();
        m_manager.respondToPairing(m_pairing.deviceId, true);
    }
}

void Backend::rejectPairing() {
    if (m_pairingActive) {
        m_pairingAnswered = true;
        emit pairingChanged();
        m_manager.respondToPairing(m_pairing.deviceId, false);
    }
}

void Backend::unpair(const QString &deviceId) {
    if (m_manager.unpair(deviceId)) {
        setStatus(tr("Device removed."));
    }
}




void Backend::sendFile(const QString &deviceId, const QString &fileUrl) {
    const QString path = QUrl(fileUrl).isLocalFile() ? QUrl(fileUrl).toLocalFile() : fileUrl;
    if (m_manager.sendFile(deviceId, path)) {
        setStatus(tr("Waiting for the other device to accept."));
    } else {
        setStatus(tr("Could not send that file."));
    }
}

void Backend::setCapability(const QString &deviceId, const QString &capability, bool enabled) {
    const Capability cap = capabilityFromName(capability);
    if (cap != Capability::None) {
        m_manager.setCapabilityEnabled(deviceId, cap, enabled);
    }
}

int Backend::connectedCount() const {
    int connected = 0;
    for (const PairedDevice &device : m_manager.pairedDevices()) {
        if (m_manager.isConnected(device.deviceId)) {
            connected += 1;
        }
    }
    return connected;
}

int Backend::pairedCount() const {
    return m_manager.pairedDevices().size();
}

QString Backend::deviceName(const QString &deviceId) const {
    const PairedDevice device = m_manager.store().deviceForId(deviceId);
    return device.isValid() ? device.deviceName : deviceId;
}


bool Backend::hasCapability(const QString &deviceId, const QString &capability) const {
    const Capability cap = capabilityFromName(capability);
    if (cap == Capability::None) {
        return false;
    }
    const PairedDevice device = m_manager.store().deviceForId(deviceId);
    return device.isValid() && device.enabledCapabilities.testFlag(cap);
}

// ---- Phones --------------------------------------------------------------

namespace {

/// An http(s) link and nothing else: no whitespace, strict parse, a host.
/// Anything that fails is treated as plain text — copied, never opened.
QUrl webLink(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 2048) {
        return {};
    }
    for (const QChar c : trimmed) {
        if (c.isSpace()) {
            return {};
        }
    }
    const QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()) {
        return {};
    }
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1StringView("http") && scheme != QLatin1StringView("https")) {
        return {};
    }
    return url;
}

/// One line of preview: newlines folded, long text cut with an ellipsis.
QString preview(const QString &text, int maxChars = 80) {
    QString line = text.simplified();
    if (line.size() > maxChars) {
        line = line.left(maxChars - 1) + QChar(0x2026);
    }
    return line;
}

constexpr int kLowBatteryPercent = 15;
constexpr int kLowBatteryResetPercent = 20;
constexpr int kPollVisibleMs = 5000;
constexpr int kPollHiddenMs = 60000;

} // namespace

QVariantList Backend::phones() const {
    QVariantList list;
    for (const PairedDevice &device : m_manager.pairedDevices()) {
        // Computers are paired with each other too; only phones have a
        // battery reading to show.
        if (device.deviceType == QLatin1StringView("desktop")) {
            continue;
        }
        const PhoneState state = m_phoneStates.value(device.deviceId);
        QVariantMap entry;
        entry.insert(QStringLiteral("deviceId"), device.deviceId);
        entry.insert(QStringLiteral("name"), device.deviceName);
        entry.insert(QStringLiteral("connected"), m_manager.isConnected(device.deviceId));
        entry.insert(QStringLiteral("status"), state.status);
        entry.insert(QStringLiteral("error"), state.error);
        entry.insert(QStringLiteral("updatedMs"), static_cast<double>(state.updatedMs));
        entry.insert(QStringLiteral("ringing"), state.ringing);
        entry.insert(QStringLiteral("ringNotice"), state.ringNotice);
        entry.insert(QStringLiteral("canStatus"),
                     m_manager.allows(device.deviceId, Capability::PhoneStatus));
        entry.insert(QStringLiteral("canRing"),
                     m_manager.allows(device.deviceId, Capability::FindPhone));
        entry.insert(QStringLiteral("canOpen"),
                     m_manager.allows(device.deviceId, Capability::OpenOnPhone));
        entry.insert(QStringLiteral("canFiles"),
                     m_manager.allows(device.deviceId, Capability::FileTransfer));
        list.append(entry);
    }
    return list;
}

QString Backend::phoneSummary() const {
    QStringList parts;
    for (const PairedDevice &device : m_manager.pairedDevices()) {
        if (device.deviceType == QLatin1StringView("desktop")
            || !m_manager.isConnected(device.deviceId)) {
            continue;
        }
        const QVariantMap status = m_phoneStates.value(device.deviceId).status;
        if (status.contains(QStringLiteral("batteryLevel"))) {
            parts << tr("%1 · %2%%3")
                         .arg(device.deviceName)
                         .arg(status.value(QStringLiteral("batteryLevel")).toInt())
                         .arg(status.value(QStringLiteral("charging")).toBool()
                                  ? tr(" charging") : QString());
        } else {
            parts << device.deviceName;
        }
    }
    return parts.join(QStringLiteral("\n"));
}

void Backend::setDashboardVisible(bool visible) {
    if (m_dashboardVisible == visible) {
        return;
    }
    m_dashboardVisible = visible;
    updatePhonePollInterval();
    if (visible) {
        refreshPhones();
    }
    emit dashboardVisibleChanged();
}

void Backend::updatePhonePollInterval() {
    // Fast only while someone is looking. In the background the reading is
    // for the tray tooltip and the low-battery warning, and a battery does
    // not fall far in a minute.
    m_phonePoll.start(m_dashboardVisible ? kPollVisibleMs : kPollHiddenMs);
}

void Backend::refreshPhones() {
    for (const PairedDevice &device : m_manager.pairedDevices()) {
        if (m_manager.isConnected(device.deviceId)) {
            m_manager.requestPhoneStatus(device.deviceId);
        }
    }
}

void Backend::onPhoneStatus(const QString &deviceId, const QVariantMap &status,
                            const QString &error) {
    PhoneState &phone = m_phoneStates[deviceId];
    if (!error.isEmpty()) {
        // The previous reading stays on screen, with the reason beside it.
        phone.error = error;
        emit phonesChanged();
        return;
    }
    phone.status = status;
    phone.error.clear();
    phone.updatedMs = QDateTime::currentMSecsSinceEpoch();

    const bool hasLevel = status.contains(QStringLiteral("batteryLevel"));
    const int level = status.value(QStringLiteral("batteryLevel")).toInt();
    const bool charging = status.value(QStringLiteral("charging")).toBool();
    const QString name = deviceName(deviceId);

    if (hasLevel) {
        // Once per discharge: re-armed only by charging or by climbing back
        // clear of the threshold, so a phone hovering at 15% is not announced
        // every minute.
        if (!charging && level <= kLowBatteryPercent && !phone.lowBatteryNotified) {
            phone.lowBatteryNotified = true;
            emit notificationRequested(tr("%1 battery low").arg(name),
                                       tr("%1% left — time to charge it.").arg(level), QUrl());
            m_activity.append(ActivityLog::Info, tr("Battery low"),
                              tr("%1 · %2%").arg(name).arg(level));
        } else if (charging || level >= kLowBatteryResetPercent) {
            phone.lowBatteryNotified = false;
        }

        if (charging && level >= 100 && !phone.fullNotified && phone.lastLevel >= 0
            && phone.lastLevel < 100) {
            phone.fullNotified = true;
            emit notificationRequested(tr("%1 is fully charged").arg(name),
                                       tr("You can unplug it."), QUrl());
        } else if (!charging || level < 95) {
            phone.fullNotified = false;
        }
        phone.lastLevel = level;
    }
    emit phonesChanged();
}

void Backend::onTextShared(const QString &deviceId, const QString &text) {
    const QString name = deviceName(deviceId);
    QGuiApplication::clipboard()->setText(text);

    const QUrl link = webLink(text);
    m_activity.append(ActivityLog::Transfer,
                      link.isValid() ? tr("Link from %1").arg(name) : tr("Text from %1").arg(name),
                      preview(text));
    if (link.isValid()) {
        // Shown by host first: the part of a link that says where it goes is
        // the part worth reading before clicking it.
        emit notificationRequested(tr("Link from %1").arg(name),
                                   tr("%1\nCopied. Click to open %2.")
                                       .arg(preview(link.toDisplayString(), 120), link.host()),
                                   link);
    } else {
        emit notificationRequested(tr("Text from %1").arg(name),
                                   tr("%1\nCopied to the clipboard.").arg(preview(text)), QUrl());
    }
    setStatus(tr("Copied what %1 shared.").arg(name));
}

void Backend::ringPhone(const QString &deviceId) {
    if (!m_manager.ringPhone(deviceId, true)) {
        setStatus(tr("%1 cannot be rung right now.").arg(deviceName(deviceId)));
        return;
    }
    PhoneState &phone = m_phoneStates[deviceId];
    phone.ringNotice.clear();
    m_activity.append(ActivityLog::Info, tr("Ringing"), deviceName(deviceId));
    setStatus(tr("Ringing %1…").arg(deviceName(deviceId)));
    emit phonesChanged();
}

void Backend::stopRinging(const QString &deviceId) {
    m_manager.ringPhone(deviceId, false);
}

int Backend::ringAllPhones() {
    int asked = 0;
    for (const PairedDevice &device : m_manager.pairedDevices()) {
        if (m_manager.allows(device.deviceId, Capability::FindPhone)) {
            ringPhone(device.deviceId);
            ++asked;
        }
    }
    if (asked == 0) {
        setStatus(tr("No phone that can ring is linked right now."));
        emit notificationRequested(tr("Maze Connect"),
                                   tr("No phone that can ring is linked right now."), QUrl());
    }
    return asked;
}

bool Backend::sendClipboardTo(const QString &deviceId) {
    const QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty()) {
        setStatus(tr("Clipboard is empty."));
        return false;
    }
    if (text.size() > limits::kMaxOpenTextChars) {
        setStatus(tr("Clipboard text is too long to send."));
        return false;
    }
    if (!m_manager.sendOpenOnPhone(deviceId, text)) {
        setStatus(tr("%1 is not reachable right now.").arg(deviceName(deviceId)));
        return false;
    }
    setStatus(tr("Sent to %1.").arg(deviceName(deviceId)));
    return true;
}

// ---- maze-guard ------------------------------------------------------------

bool Backend::guardAvailable() const {
    return m_manager.guardBridge()->isAvailable();
}

void Backend::refreshGuard() {
    m_manager.guardBridge()->requestStatus();
}

void Backend::rescanDevices() {
    m_manager.rescan();
    setStatus(tr("Scanning the network\u2026"));
}

void Backend::setGuardKill(const QString &device, bool enabled) {
    GuardDevice which{};
    // Translated through the same fixed table the link uses. QML is closer to
    // hand than a phone is, but it is still a string arriving from outside
    // C++, and it gets the same treatment.
    if (!guardDeviceFromName(device, which)) {
        setStatus(tr("Unknown device: %1").arg(device));
        return;
    }
    m_manager.guardBridge()->setDeviceEnabled(which, enabled);
}

QString Backend::commandsPath() const {
    return CommandRunner::commandsFilePath();
}

void Backend::refreshCommands() {
    CommandRunner *runner = m_manager.commandRunner();
    const QList<Command> catalog = runner->catalog();

    QVariantList list;
    list.reserve(catalog.size());
    for (const Command &command : catalog) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), command.id);
        entry.insert(QStringLiteral("label"), command.label);
        entry.insert(QStringLiteral("confirm"), command.confirm);
        entry.insert(QStringLiteral("pinned"), command.pinned);
        // Shown here, and only here. The argv is what makes this window
        // useful for checking the file — and it is exactly what never goes
        // to a phone.
        entry.insert(QStringLiteral("argv"), command.argv.join(QLatin1Char(' ')));
        list.append(entry);
    }

    m_commands = list;
    m_commandsError = runner->lastError();
    emit commandsChanged();
}

void Backend::runCommand(const QString &id) {
    const quint32 handle = m_manager.commandRunner()->run(id);
    if (handle == 0) {
        setStatus(tr("Could not run “%1”.").arg(id));
        return;
    }
    m_localRuns.insert(handle, id);
    setStatus(tr("Running “%1”…").arg(id));
}

bool Backend::createExampleCommands() {
    const QString path = CommandRunner::commandsFilePath();
    if (QFile::exists(path)) {
        setStatus(tr("A command file already exists; leaving it alone."));
        return false;
    }

    const QFileInfo info(path);
    QDir().mkpath(info.absolutePath());

    // A worked example of the format, not a set of defaults nobody chose.
    // The file is the user's from the moment it is written: entries are meant
    // to be deleted, renamed and added to.
    //
    // Two rules the list follows, and which anything added should keep:
    //
    //  * **Anything with a consequence carries "confirm".** Reading the disk
    //    usage does not; suspending, rebooting and powering off do.
    //  * **`unlock` is confirmed even though `lock` is not.** Locking adds a
    //    protection, unlocking removes one — and it is the entry most worth
    //    deleting outright if the machine sits somewhere other people can
    //    reach it.
    //
    // Four entries start "pinned" — lock, uptime, disk, mute — so the
    // phone's home-screen widget has something the moment this file is
    // written, rather than sitting empty until someone opens the app and
    // pins one by hand. Deliberately none of the ones above that carry
    // "confirm": a home-screen button is easier to hit by accident than a
    // menu item, so nothing pinned by default has a consequence worth a
    // second thought.
    //
    // None of these go through a shell, so an argument containing spaces or
    // `@DEFAULT_AUDIO_SINK@` is one argument and nothing interprets it.
    static const char kExample[] = R"([
  {
    "id": "lock",
    "label": "Lock screen",
    "argv": ["loginctl", "lock-session"],
    "confirm": false,
    "pinned": true
  },
  {
    "id": "unlock",
    "label": "Unlock screen",
    "argv": ["loginctl", "unlock-session"],
    "confirm": true
  },
  {
    "id": "uptime",
    "label": "Uptime",
    "argv": ["uptime", "-p"],
    "confirm": false,
    "pinned": true
  },
  {
    "id": "disk",
    "label": "Disk usage",
    "argv": ["df", "-h", "/"],
    "confirm": false,
    "pinned": true
  },
  {
    "id": "memory",
    "label": "Memory",
    "argv": ["free", "-h"],
    "confirm": false
  },
  {
    "id": "top",
    "label": "Busiest processes",
    "argv": ["ps", "-eo", "pcpu,comm", "--sort=-pcpu", "--no-headers"],
    "confirm": false
  },
  {
    "id": "updates",
    "label": "Pending updates",
    "argv": ["checkupdates"],
    "confirm": false
  },
  {
    "id": "ip",
    "label": "Network addresses",
    "argv": ["ip", "-brief", "address"],
    "confirm": false
  },
  {
    "id": "who",
    "label": "Logged in",
    "argv": ["who"],
    "confirm": false
  },
  {
    "id": "mute",
    "label": "Mute audio",
    "argv": ["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "1"],
    "confirm": false,
    "pinned": true
  },
  {
    "id": "unmute",
    "label": "Unmute audio",
    "argv": ["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "0"],
    "confirm": false
  },
  {
    "id": "suspend",
    "label": "Suspend",
    "argv": ["systemctl", "suspend"],
    "confirm": true
  },
  {
    "id": "reboot",
    "label": "Reboot",
    "argv": ["systemctl", "reboot"],
    "confirm": true
  },
  {
    "id": "poweroff",
    "label": "Power off",
    "argv": ["systemctl", "poweroff"],
    "confirm": true
  }
]
)";

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(tr("Could not write %1").arg(path));
        return false;
    }
    file.write(kExample);
    file.close();
    // Owner-only, because CommandRunner refuses anything looser — this file
    // decides what runs as you.
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);

    refreshCommands();
    setStatus(tr("Wrote a starter command file to %1").arg(path));
    return true;
}

bool Backend::addCommand(const QString &id, const QString &label, const QString &argv,
                         bool confirm, bool pinned) {
    Command command;
    command.id = id;
    command.label = label;
    command.argv = argv.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    command.confirm = confirm;
    command.pinned = pinned;

    CommandRunner *runner = m_manager.commandRunner();
    if (!runner->addCommand(command)) {
        setStatus(tr("Could not add “%1”: %2").arg(id, runner->lastError()));
        refreshCommands(); // surface the error on the property too
        return false;
    }
    refreshCommands();
    setStatus(tr("Added “%1”.").arg(command.label.isEmpty() ? id : command.label));
    return true;
}

bool Backend::updateCommand(const QString &id, const QString &newId, const QString &label,
                            const QString &argv, bool confirm, bool pinned) {
    Command updated;
    updated.id = newId;
    updated.label = label;
    updated.argv = argv.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    updated.confirm = confirm;
    updated.pinned = pinned;

    CommandRunner *runner = m_manager.commandRunner();
    if (!runner->updateCommand(id, updated)) {
        setStatus(tr("Could not update “%1”: %2").arg(id, runner->lastError()));
        refreshCommands();
        return false;
    }
    refreshCommands();
    setStatus(tr("Updated “%1”.").arg(updated.label.isEmpty() ? newId : updated.label));
    return true;
}

bool Backend::removeCommand(const QString &id) {
    CommandRunner *runner = m_manager.commandRunner();
    if (!runner->removeCommand(id)) {
        setStatus(tr("Could not remove “%1”: %2").arg(id, runner->lastError()));
        refreshCommands();
        return false;
    }
    refreshCommands();
    setStatus(tr("Removed “%1”.").arg(id));
    return true;
}

bool Backend::sendClipboardToPhone() {
    const QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty()) {
        setStatus(tr("Clipboard is empty."));
        return false;
    }
    if (text.size() > limits::kMaxOpenTextChars) {
        setStatus(tr("Clipboard text is too long to send."));
        return false;
    }
    if (!m_manager.sendOpenOnPhone(text)) {
        setStatus(tr("No paired phone is reachable right now."));
        return false;
    }
    setStatus(tr("Sent to your phone."));
    return true;
}

void Backend::respondToFileOffer(const QString &deviceId, int transferId, bool accept) {
    if (transferId < 0) {
        return;
    }
    m_manager.respondToFileOffer(deviceId, static_cast<quint32>(transferId), accept);
}

QString Backend::shortFingerprint(const QString &fingerprint) const {
    // Grouped hex is far easier to read off a screen than 64 unbroken
    // characters, and the first 16 are plenty for a visual sanity check
    // (the full value is what the protocol actually compares).
    QString grouped;
    for (int i = 0; i < qMin(16, static_cast<int>(fingerprint.size())); i += 4) {
        if (!grouped.isEmpty()) {
            grouped += u' ';
        }
        grouped += fingerprint.mid(i, 4);
    }
    return grouped.toUpper();
}

// --- Startup -------------------------------------------------------------

bool Backend::autostartEnabled() const {
    // Read the entry rather than cache it: the user may well have removed it
    // from their desktop's own startup settings while this window was open.
    return autostart::isEnabled();
}

void Backend::setAutostartEnabled(bool enabled) {
    if (autostart::isEnabled() == enabled) {
        return;
    }
    if (!autostart::setEnabled(enabled)) {
        m_statusMessage = tr("Could not write the autostart entry");
        emit statusMessageChanged();
    }
    emit autostartChanged();
}
