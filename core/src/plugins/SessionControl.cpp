#include "mazeconnect/core/SessionControl.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>

#include <unistd.h>

namespace mazeconnect::core {
namespace {

constexpr auto kService = "org.freedesktop.login1";
constexpr auto kManagerPath = "/org/freedesktop/login1";
constexpr auto kManager = "org.freedesktop.login1.Manager";
constexpr auto kUser = "org.freedesktop.login1.User";
constexpr auto kSession = "org.freedesktop.login1.Session";
constexpr auto kProperties = "org.freedesktop.DBus.Properties";
constexpr int kTimeoutMs = 5000;

QString tr(const char *text) {
    return QCoreApplication::translate("SessionControl", text);
}

QDBusMessage getProperty(const QString &path, const char *interface, const char *name) {
    QDBusMessage call = QDBusMessage::createMethodCall(QLatin1StringView(kService), path,
                                                       QLatin1StringView(kProperties),
                                                       QStringLiteral("Get"));
    call << QString::fromLatin1(interface) << QString::fromLatin1(name);
    return QDBusConnection::systemBus().call(call, QDBus::Block, kTimeoutMs);
}

/// The object path out of a `(so)` property — User.Display.
QString pathFromStruct(const QDBusMessage &reply) {
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return {};
    }
    const QVariant variant = reply.arguments().first().value<QDBusVariant>().variant();
    if (!variant.canConvert<QDBusArgument>()) {
        return {};
    }
    const QDBusArgument arg = variant.value<QDBusArgument>();
    QString id;
    QDBusObjectPath path;
    arg.beginStructure();
    arg >> id >> path;
    arg.endStructure();
    return path.path();
}

} // namespace

QString LogindSessionControl::sessionPath(QString *error) {
    if (m_path.isEmpty()) {
        m_path = resolveSessionPath(error);
    }
    return m_path;
}

QString LogindSessionControl::resolveSessionPath(QString *error) {
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        *error = tr("no system bus");
        return {};
    }
    QDBusInterface manager(QLatin1StringView(kService), QLatin1StringView(kManagerPath),
                           QLatin1StringView(kManager), bus);
    manager.setTimeout(kTimeoutMs);

    // The session this process belongs to, when it belongs to one — the GUI.
    const QString sessionId = qEnvironmentVariable("XDG_SESSION_ID");
    if (!sessionId.isEmpty()) {
        const QDBusReply<QDBusObjectPath> reply =
            manager.call(QStringLiteral("GetSession"), sessionId);
        if (reply.isValid()) {
            return reply.value().path();
        }
    }

    // Otherwise this user's display session — the daemon's case.
    const QDBusReply<QDBusObjectPath> user =
        manager.call(QStringLiteral("GetUser"), static_cast<quint32>(::getuid()));
    if (!user.isValid()) {
        *error = tr("logind does not know this user");
        return {};
    }
    const QString display = pathFromStruct(getProperty(user.value().path(), kUser, "Display"));
    if (display.isEmpty() || display == QLatin1StringView("/")) {
        *error = tr("no graphical session to unlock");
        return {};
    }
    return display;
}

SessionControl::State LogindSessionControl::state(QString *error) {
    QString ignored;
    QString &err = error ? *error : ignored;
    const QString path = sessionPath(&err);
    if (path.isEmpty()) {
        return State::Unknown;
    }
    QDBusMessage reply = getProperty(path, kSession, "LockedHint");
    if (reply.type() != QDBusMessage::ReplyMessage) {
        // The cached session may be gone; look it up once more.
        m_path.clear();
        const QString fresh = sessionPath(&err);
        if (fresh.isEmpty()) {
            return State::Unknown;
        }
        reply = getProperty(fresh, kSession, "LockedHint");
    }
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        err = tr("could not read the session's lock state");
        return State::Unknown;
    }
    const QVariant locked = reply.arguments().first().value<QDBusVariant>().variant();
    if (locked.typeId() != QMetaType::Bool) {
        err = tr("could not read the session's lock state");
        return State::Unknown;
    }
    return locked.toBool() ? State::Locked : State::Unlocked;
}

bool LogindSessionControl::unlock(QString *error) {
    QString ignored;
    QString &err = error ? *error : ignored;
    const QString path = sessionPath(&err);
    if (path.isEmpty()) {
        return false;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(QLatin1StringView(kService), path,
                                                       QLatin1StringView(kSession),
                                                       QStringLiteral("Unlock"));
    const QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, kTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        m_path.clear();
        err = reply.errorMessage().isEmpty() ? tr("logind refused to unlock the session")
                                             : reply.errorMessage().left(200);
        return false;
    }
    return true;
}

} // namespace mazeconnect::core
