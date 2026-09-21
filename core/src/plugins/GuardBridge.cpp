#include "mazeconnect/core/GuardBridge.h"

#include <QFileInfo>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QTimer>
#include <QProcessEnvironment>

namespace mazeconnect::core {
namespace {

Q_LOGGING_CATEGORY(lcGuard, "maze.connect.guard")

struct DeviceEntry {
    GuardDevice device;
    const char *name;
};

/**
 * The whole device vocabulary, matching maze-guardd's own DEVICES tuple.
 *
 * A name arriving from a peer is looked up here and converted to an enum. It
 * is never passed onward as text, so the command line this class builds cannot
 * contain anything a peer chose — no spaces to add an argument with, no
 * newline to append a second verb with.
 */
constexpr DeviceEntry kDevices[] = {
    {GuardDevice::Camera, "camera"},
    {GuardDevice::Microphone, "microphone"},
    {GuardDevice::Bluetooth, "bluetooth"},
    {GuardDevice::Wifi, "wifi"},
    {GuardDevice::Usb, "usb"},
};

} // namespace

QString guardDeviceName(GuardDevice device) {
    for (const DeviceEntry &entry : kDevices) {
        if (entry.device == device) {
            return QString::fromLatin1(entry.name);
        }
    }
    return {};
}

bool guardDeviceFromName(const QString &name, GuardDevice &out) {
    for (const DeviceEntry &entry : kDevices) {
        if (name == QLatin1StringView(entry.name)) {
            out = entry.device;
            return true;
        }
    }
    // Not guessed at, not partially matched. An unrecognised device name is
    // simply not a device.
    return false;
}

QString guardStateName(GuardState state) {
    switch (state) {
    case GuardState::On:
        return QStringLiteral("on");
    case GuardState::Off:
        return QStringLiteral("off");
    case GuardState::Unavailable:
        return QStringLiteral("none");
    case GuardState::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

GuardBridge::GuardBridge(QObject *parent) : QObject(parent) {
    qRegisterMetaType<GuardDevice>();
    qRegisterMetaType<GuardState>();
}

GuardBridge::~GuardBridge() {
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
    }
}

QString GuardBridge::socketPath() {
    const QString override = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("MAZECONNECT_GUARD_SOCKET"));
    if (!override.isEmpty()) {
        return override;
    }
    // maze-guardd (maze-tools) — the privileged-action broker that owns panic and
    // the hardware killswitches. NOT /run/maze/maze.sock: that belongs to a
    // DIFFERENT daemon, maze-guard's own helper, which does MAC randomisation and
    // does not speak this STATUS/KILL protocol. Pointing here at maze.sock makes
    // the connection succeed and then hang forever with no reply.
    return QStringLiteral("/run/maze/guard.sock");
}

bool GuardBridge::isAvailable() const {
    return QFileInfo::exists(socketPath());
}

QMap<GuardDevice, GuardState> GuardBridge::parseStatus(const QString &reply) const {
    // "OK camera=on microphone=off bluetooth=none wifi=on usb=on"
    QMap<GuardDevice, GuardState> states;
    const QStringList parts = reply.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const int equals = part.indexOf(QLatin1Char('='));
        if (equals <= 0) {
            continue;
        }
        GuardDevice device{};
        if (!guardDeviceFromName(part.left(equals), device)) {
            continue;
        }
        const QString value = part.mid(equals + 1);
        if (value == QLatin1StringView("on")) {
            states.insert(device, GuardState::On);
        } else if (value == QLatin1StringView("off")) {
            states.insert(device, GuardState::Off);
        } else if (value == QLatin1StringView("none")) {
            // The machine has no such device. Its own state, not folded into
            // "off" — a laptop with no Bluetooth is not a laptop whose
            // Bluetooth is blocked.
            states.insert(device, GuardState::Unavailable);
        }
    }
    return states;
}

void GuardBridge::requestStatus() {
    if (!isAvailable()) {
        emit statusFailed(QStringLiteral("maze-guardd is not available on this machine"));
        return;
    }
    // A literal verb with no argument: there is nothing here a caller could
    // have influenced.
    enqueue({QStringLiteral("STATUS"), false, false, {}, false});
}

void GuardBridge::setDeviceEnabled(GuardDevice device, bool enabled) {
    const QString name = guardDeviceName(device);
    if (name.isEmpty()) {
        // Unreachable through the public API — it takes an enum — but a
        // new enumerator added without a table entry would land here rather
        // than sending "KILL  on".
        emit killApplied(device, enabled, GuardState::Unknown, QStringLiteral("unknown device"));
        return;
    }
    if (!isAvailable()) {
        emit killApplied(device, enabled, GuardState::Unknown,
                         QStringLiteral("maze-guardd is not available on this machine"));
        return;
    }

    // Built entirely from the fixed table and a bool. Note what cannot be
    // expressed here: PANIC, RESTORE, a second line, an extra argument, or a
    // device the enum does not name.
    //
    // "on" enables the device and "off" blocks it — maze-guardd's own
    // vocabulary, and the reason the parameter is named after the device.
    Pending pending;
    pending.command = QStringLiteral("KILL %1 %2")
                          .arg(name, enabled ? QStringLiteral("on") : QStringLiteral("off"));
    pending.isKill = true;
    pending.device = device;
    pending.requestedEnabled = enabled;
    enqueue(pending);
}

void GuardBridge::enqueue(const Pending &pending) {
    m_queue.enqueue(pending);
    if (!m_socket) {
        startNext();
    }
}

void GuardBridge::startNext() {
    if (m_queue.isEmpty()) {
        return;
    }
    m_current = m_queue.dequeue();
    m_buffer.clear();

    m_socket = new QLocalSocket(this);

    connect(m_socket, &QLocalSocket::connected, this, [this] {
        // One line, terminated. maze-guardd reads a single line per
        // connection and ignores anything after the first.
        m_socket->write(m_current.command.toLatin1() + '\n');
    });

    connect(m_socket, &QLocalSocket::readyRead, this, [this] {
        m_buffer.append(m_socket->readAll());
        if (m_buffer.size() > kMaxReplyBytes) {
            failCurrent(QStringLiteral("maze-guardd answered with more than we can read"));
            return;
        }
        const int newline = m_buffer.indexOf('\n');
        if (newline < 0) {
            return;
        }
        handleReply(QString::fromUtf8(m_buffer.left(newline)).trimmed());
    });

    connect(m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        // A close after a complete reply is normal; only an error before one
        // is a failure.
        if (m_socket) {
            failCurrent(QStringLiteral("maze-guardd is not running"));
        }
    });

    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        qCWarning(lcGuard) << "maze-guardd did not answer in time";
        failCurrent(QStringLiteral("maze-guardd did not answer"));
    });
    m_timeout->start(kTimeoutMs);

    m_socket->connectToServer(socketPath());
}

void GuardBridge::handleReply(const QString &reply) {
    const Pending current = m_current;
    QString error;
    if (reply.startsWith(QLatin1StringView("ERR"))) {
        error = reply.mid(4).trimmed();
        if (error.isEmpty()) {
            error = QStringLiteral("maze-guardd refused the request");
        }
    } else if (!reply.startsWith(QLatin1StringView("OK"))) {
        error = QStringLiteral("unexpected answer from maze-guardd");
    }

    finishCurrent();

    if (!error.isEmpty()) {
        if (current.isKill || current.isKillFollowUp) {
            qCWarning(lcGuard) << "killswitch change refused:" << error;
            emit killApplied(current.device, current.requestedEnabled, GuardState::Unknown, error);
        } else {
            emit statusFailed(error);
        }
        startNext();
        return;
    }

    if (current.isKill) {
        // Re-read rather than assume: a request the broker accepted is not
        // proof the hardware complied, and reporting the state we asked for
        // would be reporting a protection we have not confirmed.
        Pending followUp;
        followUp.command = QStringLiteral("STATUS");
        followUp.isKillFollowUp = true;
        followUp.device = current.device;
        followUp.requestedEnabled = current.requestedEnabled;
        m_queue.prepend(followUp);
    } else if (current.isKillFollowUp) {
        const GuardState state = parseStatus(reply).value(current.device, GuardState::Unknown);
        qCInfo(lcGuard) << "device" << guardDeviceName(current.device)
                        << (current.requestedEnabled ? "allowed" : "blocked") << "->"
                        << guardStateName(state);
        emit killApplied(current.device, current.requestedEnabled, state, QString());
    } else {
        emit statusReady(parseStatus(reply));
    }

    startNext();
}

void GuardBridge::failCurrent(const QString &error) {
    const Pending current = m_current;
    finishCurrent();

    if (current.isKill || current.isKillFollowUp) {
        emit killApplied(current.device, current.requestedEnabled, GuardState::Unknown, error);
    } else {
        emit statusFailed(error);
    }
    startNext();
}

void GuardBridge::finishCurrent() {
    if (m_timeout) {
        m_timeout->stop();
        m_timeout->deleteLater();
        m_timeout = nullptr;
    }
    if (m_socket) {
        QLocalSocket *socket = m_socket;
        m_socket = nullptr;
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    m_current = {};
}

} // namespace mazeconnect::core
