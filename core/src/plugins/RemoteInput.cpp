#include "mazeconnect/core/RemoteInput.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QLoggingCategory>
#include <QSettings>

#include <cmath>

Q_LOGGING_CATEGORY(lcInput, "maze.connect.input")

namespace mazeconnect::core {
namespace {

constexpr auto kPortalService = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kRemoteDesktop = "org.freedesktop.portal.RemoteDesktop";
constexpr auto kRestoreTokenKey = "remoteInput/restoreToken";

// evdev button codes, as the portal takes them.
constexpr int kBtnLeft = 0x110;
constexpr int kBtnRight = 0x111;
constexpr int kBtnMiddle = 0x112;

struct KeyEntry {
    const char *name;
    int keysym;
    bool presenter;
};

// Every key a phone can name. A phone never sends a keysym number: it sends
// one of these names, and anything else is refused.
constexpr KeyEntry kKeys[] = {
    {"escape", 0xff1b, true},    {"tab", 0xff09, false},      {"enter", 0xff0d, false},
    {"backspace", 0xff08, false}, {"delete", 0xffff, false},  {"space", 0x20, false},
    {"up", 0xff52, false},       {"down", 0xff54, false},     {"left", 0xff51, true},
    {"right", 0xff53, true},     {"home", 0xff50, true},      {"end", 0xff57, true},
    {"pageUp", 0xff55, true},    {"pageDown", 0xff56, true},  {"insert", 0xff63, false},
    {"f1", 0xffbe, false},       {"f2", 0xffbf, false},       {"f3", 0xffc0, false},
    {"f4", 0xffc1, false},       {"f5", 0xffc2, true},        {"f6", 0xffc3, false},
    {"f7", 0xffc4, false},       {"f8", 0xffc5, false},       {"f9", 0xffc6, false},
    {"f10", 0xffc7, false},      {"f11", 0xffc8, false},      {"f12", 0xffc9, false},
    {"super", 0xffeb, false},    {"menu", 0xff67, false},     {"print", 0xff61, false},
    {"volumeUp", 0x1008ff13, false}, {"volumeDown", 0x1008ff11, false},
    {"mute", 0x1008ff12, false}, {"playPause", 0x1008ff14, false},
    {"nextTrack", 0x1008ff17, false}, {"previousTrack", 0x1008ff16, false},
    // Presenting: B blanks the screen in LibreOffice Impress, Okular and most
    // browser slide decks.
    {"blank", 0x62, true},
};

struct ModEntry {
    const char *name;
    int keysym;
};
constexpr ModEntry kMods[] = {
    {"ctrl", 0xffe3}, {"alt", 0xffe9}, {"shift", 0xffe1}, {"super", 0xffeb},
};

/// A keysym for one character: Latin-1 maps to itself, everything else to
/// the Unicode keysym range. Control characters other than tab and newline
/// have no business in typed text and are refused.
bool keysymForChar(char32_t c, int &out) {
    if (c == U'\n') {
        out = 0xff0d;
        return true;
    }
    if (c == U'\t') {
        out = 0xff09;
        return true;
    }
    if (c < 0x20 || c == 0x7f || (c >= 0x80 && c < 0xa0)) {
        return false;
    }
    if ((c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069)) {
        return false;
    }
    if (c > 0x10ffff) {
        return false;
    }
    out = c <= 0xff ? static_cast<int>(c) : static_cast<int>(0x01000000 | c);
    return true;
}

bool finite(const QVariant &v, double &out) {
    bool ok = false;
    const double d = v.toDouble(&ok);
    if (!ok || !std::isfinite(d) || v.typeId() == QMetaType::QString) {
        return false;
    }
    out = d;
    return true;
}

} // namespace

// ---- Portal backend ---------------------------------------------------------

/// Receives one portal Request's Response and forwards it to @p target.
class PortalResponse : public QObject {
    Q_OBJECT
public:
    PortalResponse(QObject *target, const char *slot, QObject *parent)
        : QObject(parent), m_target(target), m_slot(slot) {}
public slots:
    void onResponse(uint response, const QVariantMap &results) {
        // Queued-by-name so the target's private slots stay private.
        QByteArray name(m_slot);
        name = name.mid(1, name.indexOf('(') - 1);
        QMetaObject::invokeMethod(m_target, name.constData(), Qt::QueuedConnection,
                                  Q_ARG(uint, response), Q_ARG(QVariantMap, results));
        deleteLater();
    }
private:
    QObject *m_target;
    const char *m_slot;
};

PortalInputBackend::PortalInputBackend(QObject *parent) : InputBackend(parent) {
    // A non-sandboxed app has to say who it is before the portal can name it
    // in the consent dialog, or remember the consent. Best effort: an older
    // portal without the host Registry still works, it just asks each time.
    QDBusMessage reg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
        QStringLiteral("org.freedesktop.host.portal.Registry"), QStringLiteral("Register"));
    reg << QStringLiteral("maze-connect") << QVariantMap();
    QDBusConnection::sessionBus().asyncCall(reg);
}

PortalInputBackend::~PortalInputBackend() {
    if (!m_session.isEmpty()) {
        stop();
    }
}

bool PortalInputBackend::request(const QString &method, const QVariantList &args,
                                 const QString &token, const char *slot) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return false;
    }
    // The Request object's path is predictable from our unique name and the
    // token, and the signal is subscribed to *before* the call — the portal
    // may answer before the call's own reply arrives.
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString path = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
                             .arg(sender, token);
    auto *response = new PortalResponse(this, slot, this);
    bus.connect(QString::fromLatin1(kPortalService), path,
                QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                response, SLOT(onResponse(uint, QVariantMap)));

    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
        QString::fromLatin1(kRemoteDesktop), method);
    call.setArguments(args);
    bus.asyncCall(call);
    return true;
}

void PortalInputBackend::start() {
    if (m_active || m_starting) {
        if (m_active) {
            emit started(true, QString());
        }
        return;
    }
    m_starting = true;
    const QString token = QStringLiteral("mazeconnect%1").arg(++m_tokenCounter);
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    options.insert(QStringLiteral("session_handle_token"),
                   QStringLiteral("mazeconnectsession%1").arg(m_tokenCounter));
    if (!request(QStringLiteral("CreateSession"), {options}, token,
                 SLOT(onCreateSession(uint, QVariantMap)))) {
        fail(tr("no desktop session bus"));
    }
}

void PortalInputBackend::onCreateSession(uint response, const QVariantMap &results) {
    if (response != 0) {
        fail(tr("the desktop refused a remote-input session"));
        return;
    }
    // A string in the spec, an object path from some implementations.
    const QVariant handle = results.value(QStringLiteral("session_handle"));
    m_session = handle.metaType() == QMetaType::fromType<QDBusObjectPath>()
        ? handle.value<QDBusObjectPath>().path()
        : handle.toString();
    if (m_session.isEmpty()) {
        fail(tr("the desktop did not open a session"));
        return;
    }
    QDBusConnection::sessionBus().connect(
        QString::fromLatin1(kPortalService), m_session,
        QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Closed"), this,
        SLOT(onSessionClosed()));

    const QString token = QStringLiteral("mazeconnect%1").arg(++m_tokenCounter);
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    options.insert(QStringLiteral("types"), QVariant::fromValue(uint(1 | 2))); // keyboard, pointer
    // Remember the consent until it is revoked in System Settings.
    options.insert(QStringLiteral("persist_mode"), QVariant::fromValue(uint(2)));
    const QString restore = QSettings().value(QString::fromLatin1(kRestoreTokenKey)).toString();
    if (!restore.isEmpty()) {
        options.insert(QStringLiteral("restore_token"), restore);
    }
    request(QStringLiteral("SelectDevices"), {QVariant::fromValue(QDBusObjectPath(m_session)), options},
            token, SLOT(onSelectDevices(uint, QVariantMap)));
}

void PortalInputBackend::onSelectDevices(uint response, const QVariantMap &) {
    if (response != 0) {
        fail(tr("the desktop refused keyboard and pointer access"));
        return;
    }
    const QString token = QStringLiteral("mazeconnect%1").arg(++m_tokenCounter);
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    request(QStringLiteral("Start"),
            {QVariant::fromValue(QDBusObjectPath(m_session)), QString(), options}, token,
            SLOT(onStart(uint, QVariantMap)));
}

void PortalInputBackend::onStart(uint response, const QVariantMap &results) {
    m_starting = false;
    if (response != 0) {
        fail(response == 1 ? tr("declined on the computer") : tr("the desktop could not start it"));
        return;
    }
    const QString restore = results.value(QStringLiteral("restore_token")).toString();
    if (!restore.isEmpty()) {
        QSettings().setValue(QString::fromLatin1(kRestoreTokenKey), restore);
    }
    m_active = true;
    qCInfo(lcInput) << "remote input session started";
    emit started(true, QString());
}

void PortalInputBackend::onSessionClosed() {
    const bool was = m_active;
    m_active = false;
    m_starting = false;
    m_session.clear();
    if (was) {
        emit stopped(tr("ended on the computer"));
    }
}

void PortalInputBackend::fail(const QString &error) {
    qCWarning(lcInput) << "remote input:" << error;
    m_starting = false;
    m_active = false;
    if (!m_session.isEmpty()) {
        stop();
    }
    emit started(false, error);
}

void PortalInputBackend::stop() {
    if (!m_session.isEmpty()) {
        QDBusMessage close = QDBusMessage::createMethodCall(
            QString::fromLatin1(kPortalService), m_session,
            QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Close"));
        QDBusConnection::sessionBus().asyncCall(close);
    }
    m_session.clear();
    m_active = false;
    m_starting = false;
}

void PortalInputBackend::notify(const QString &method, const QVariantList &args) {
    if (!m_active || m_session.isEmpty()) {
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
        QString::fromLatin1(kRemoteDesktop), method);
    QVariantList full{QVariant::fromValue(QDBusObjectPath(m_session)), QVariantMap()};
    full.append(args);
    call.setArguments(full);
    QDBusConnection::sessionBus().asyncCall(call);
}

void PortalInputBackend::pointerMotion(double dx, double dy) {
    notify(QStringLiteral("NotifyPointerMotion"), {dx, dy});
}

void PortalInputBackend::pointerButton(int button, bool pressed) {
    notify(QStringLiteral("NotifyPointerButton"), {button, uint(pressed ? 1 : 0)});
}

void PortalInputBackend::pointerScroll(int axis, int steps) {
    notify(QStringLiteral("NotifyPointerAxisDiscrete"), {uint(axis), steps});
}

void PortalInputBackend::keysym(int keysym, bool pressed) {
    notify(QStringLiteral("NotifyKeyboardKeysym"), {keysym, uint(pressed ? 1 : 0)});
}

// ---- Policy -----------------------------------------------------------------

RemoteInput::RemoteInput(InputBackend *backend, QObject *parent)
    : QObject(parent), m_backend(backend ? backend : new PortalInputBackend()) {
    m_backend->setParent(nullptr);
    m_clock.start();
    m_idle.setSingleShot(true);
    m_idle.setInterval(kIdleTimeoutMs);
    connect(&m_idle, &QTimer::timeout, this, [this] {
        end(m_owner, tr("no input for two minutes"));
    });
    connect(m_backend.get(), &InputBackend::started, this, [this](bool ok, const QString &error) {
        const QString who = std::exchange(m_pending, QString());
        if (who.isEmpty()) {
            return;
        }
        if (!ok) {
            m_owner.clear();
            emit stateChanged(who, false, m_mode, error);
            return;
        }
        m_owner = who;
        touch();
        emit stateChanged(who, true, m_mode, QString());
    });
    connect(m_backend.get(), &InputBackend::stopped, this, [this](const QString &reason) {
        const QString who = std::exchange(m_owner, QString());
        m_idle.stop();
        if (!who.isEmpty()) {
            emit stateChanged(who, false, m_mode, reason);
        }
    });
}

bool RemoteInput::begin(const QString &deviceId, Mode mode) {
    if (deviceId.isEmpty()) {
        return false;
    }
    if ((!m_owner.isEmpty() && m_owner != deviceId)
        || (!m_pending.isEmpty() && m_pending != deviceId)) {
        return false; // someone else is driving
    }
    m_mode = mode;
    if (m_owner == deviceId && m_backend->isActive()) {
        touch();
        emit stateChanged(deviceId, true, m_mode, QString());
        return true;
    }
    m_pending = deviceId;
    m_backend->start();
    return true;
}

void RemoteInput::end(const QString &deviceId, const QString &reason) {
    if (deviceId.isEmpty()) {
        return;
    }
    if (m_pending == deviceId) {
        m_pending.clear();
    }
    if (m_owner != deviceId) {
        return;
    }
    m_owner.clear();
    m_idle.stop();
    m_backend->stop();
    emit stateChanged(deviceId, false, m_mode, reason);
}

void RemoteInput::touch() {
    m_idle.start();
}

bool RemoteInput::allowRate() {
    const qint64 now = m_clock.elapsed();
    if (now - m_windowStart >= 1000) {
        m_windowStart = now;
        m_windowCount = 0;
    }
    return ++m_windowCount <= kMaxEventsPerSecond;
}

bool RemoteInput::keysymForName(const QString &name, int &keysym) {
    for (const KeyEntry &k : kKeys) {
        if (name == QLatin1StringView(k.name)) {
            keysym = k.keysym;
            return true;
        }
    }
    // Single letters and digits, for shortcuts like Ctrl+C.
    if (name.size() == 1) {
        const QChar c = name.at(0);
        if ((c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9')) {
            keysym = c.unicode();
            return true;
        }
    }
    return false;
}

bool RemoteInput::isPresenterKey(const QString &name) {
    for (const KeyEntry &k : kKeys) {
        if (name == QLatin1StringView(k.name)) {
            return k.presenter;
        }
    }
    return false;
}

void RemoteInput::tap(int keysym, const QList<int> &modifiers) {
    for (int m : modifiers) {
        m_backend->keysym(m, true);
    }
    m_backend->keysym(keysym, true);
    m_backend->keysym(keysym, false);
    for (auto it = modifiers.crbegin(); it != modifiers.crend(); ++it) {
        m_backend->keysym(*it, false);
    }
}

bool RemoteInput::handleEvent(const QString &deviceId, const QVariantMap &event) {
    if (deviceId.isEmpty() || deviceId != m_owner || !m_backend->isActive()) {
        return false;
    }
    if (!allowRate()) {
        return false;
    }
    const QString kind = event.value(QStringLiteral("kind")).toString();

    if (kind == QLatin1StringView("key")) {
        const QString name = event.value(QStringLiteral("key")).toString();
        int keysym = 0;
        if (!keysymForName(name, keysym)) {
            return false;
        }
        const QString action = event.value(QStringLiteral("action"), QStringLiteral("tap")).toString();
        const QVariantList mods = event.value(QStringLiteral("mods")).toList();
        if (m_mode == Mode::Presenter) {
            // A fixed set of keys, tapped, with no modifiers: nothing that
            // can type, open a menu or reach a shortcut.
            if (!isPresenterKey(name) || action != QLatin1StringView("tap") || !mods.isEmpty()) {
                return false;
            }
        }
        QList<int> modifiers;
        if (mods.size() > 4) {
            return false;
        }
        for (const QVariant &m : mods) {
            bool found = false;
            for (const ModEntry &e : kMods) {
                if (m.toString() == QLatin1StringView(e.name) && !modifiers.contains(e.keysym)) {
                    modifiers.append(e.keysym);
                    found = true;
                }
            }
            if (!found) {
                return false;
            }
        }
        if (action == QLatin1StringView("tap")) {
            tap(keysym, modifiers);
        } else if (action == QLatin1StringView("press") && modifiers.isEmpty()) {
            m_backend->keysym(keysym, true);
        } else if (action == QLatin1StringView("release") && modifiers.isEmpty()) {
            m_backend->keysym(keysym, false);
        } else {
            return false;
        }
        touch();
        return true;
    }

    // Everything below is full control only.
    if (m_mode != Mode::Full) {
        return false;
    }

    if (kind == QLatin1StringView("move")) {
        double dx = 0;
        double dy = 0;
        if (!finite(event.value(QStringLiteral("dx")), dx)
            || !finite(event.value(QStringLiteral("dy")), dy)) {
            return false;
        }
        m_backend->pointerMotion(qBound(-kMaxMotion, dx, kMaxMotion),
                                 qBound(-kMaxMotion, dy, kMaxMotion));
        touch();
        return true;
    }

    if (kind == QLatin1StringView("button")) {
        const QString name = event.value(QStringLiteral("button")).toString();
        const int button = name == QLatin1StringView("left") ? kBtnLeft
                         : name == QLatin1StringView("right") ? kBtnRight
                         : name == QLatin1StringView("middle") ? kBtnMiddle
                         : 0;
        if (button == 0) {
            return false;
        }
        const QString action = event.value(QStringLiteral("action"), QStringLiteral("click")).toString();
        if (action == QLatin1StringView("click")) {
            m_backend->pointerButton(button, true);
            m_backend->pointerButton(button, false);
        } else if (action == QLatin1StringView("press")) {
            m_backend->pointerButton(button, true);
        } else if (action == QLatin1StringView("release")) {
            m_backend->pointerButton(button, false);
        } else {
            return false;
        }
        touch();
        return true;
    }

    if (kind == QLatin1StringView("scroll")) {
        double dx = 0;
        double dy = 0;
        if (!finite(event.value(QStringLiteral("dx"), 0.0), dx)
            || !finite(event.value(QStringLiteral("dy"), 0.0), dy)) {
            return false;
        }
        const int vx = qBound(-kMaxScrollSteps, static_cast<int>(std::lround(dx)), kMaxScrollSteps);
        const int vy = qBound(-kMaxScrollSteps, static_cast<int>(std::lround(dy)), kMaxScrollSteps);
        if (vy != 0) {
            m_backend->pointerScroll(0, vy);
        }
        if (vx != 0) {
            m_backend->pointerScroll(1, vx);
        }
        touch();
        return true;
    }

    if (kind == QLatin1StringView("text")) {
        const QVariant raw = event.value(QStringLiteral("text"));
        if (raw.typeId() != QMetaType::QString) {
            return false;
        }
        const QString text = raw.toString();
        if (text.isEmpty() || text.size() > kMaxTextChars) {
            return false;
        }
        // All or nothing: a string with one refused character is refused
        // whole, rather than typed with a gap nobody asked for.
        QList<int> keysyms;
        for (const char32_t c : text.toUcs4()) {
            int ks = 0;
            if (!keysymForChar(c, ks)) {
                return false;
            }
            keysyms.append(ks);
        }
        for (int ks : keysyms) {
            m_backend->keysym(ks, true);
            m_backend->keysym(ks, false);
        }
        touch();
        return true;
    }

    return false;
}

} // namespace mazeconnect::core

#include "RemoteInput.moc"
