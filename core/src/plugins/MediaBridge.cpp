#include "mazeconnect/core/MediaBridge.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace mazeconnect::core {

Q_LOGGING_CATEGORY(lcMedia, "maze.connect.media")

namespace {

constexpr QLatin1StringView kMprisPrefix("org.mpris.MediaPlayer2.");
constexpr QLatin1StringView kMprisPath("/org/mpris/MediaPlayer2");
constexpr QLatin1StringView kRootInterface("org.mpris.MediaPlayer2");
constexpr QLatin1StringView kPlayerInterface("org.mpris.MediaPlayer2.Player");
constexpr QLatin1StringView kPropertiesInterface("org.freedesktop.DBus.Properties");
constexpr QLatin1StringView kNoTrack("/org/mpris/MediaPlayer2/TrackList/NoTrack");

constexpr int kChangedDebounceMs = 120;
constexpr int kVolumePollMs = 3000;
constexpr int kVolumeToolTimeoutMs = 2000;

struct ActionEntry {
    MediaAction action;
    const char *name;
};

constexpr ActionEntry kActions[] = {
    {MediaAction::Play, "play"},
    {MediaAction::Pause, "pause"},
    {MediaAction::PlayPause, "playPause"},
    {MediaAction::Next, "next"},
    {MediaAction::Previous, "previous"},
    {MediaAction::Stop, "stop"},
    {MediaAction::Seek, "seek"},
    {MediaAction::SetVolume, "setVolume"},
    {MediaAction::SystemVolume, "systemVolume"},
    {MediaAction::SystemMute, "systemMute"},
};

/// A nested D-Bus dictionary arrives as a QDBusArgument inside the variant;
/// a value built in-process arrives as a plain map. Accept both.
QVariantMap toMap(const QVariant &value) {
    if (value.canConvert<QDBusArgument>()) {
        return qdbus_cast<QVariantMap>(value.value<QDBusArgument>());
    }
    return value.toMap();
}

QStringList toStringList(const QVariant &value) {
    if (value.canConvert<QDBusArgument>()) {
        return qdbus_cast<QStringList>(value.value<QDBusArgument>());
    }
    if (value.typeId() == QMetaType::QString) {
        return {value.toString()};
    }
    return value.toStringList();
}

QString objectPathOf(const QVariant &value) {
    if (value.canConvert<QDBusObjectPath>()) {
        const QString path = value.value<QDBusObjectPath>().path();
        if (!path.isEmpty()) {
            return path;
        }
    }
    return value.toString();
}

QString statusName(const QString &mprisStatus) {
    if (mprisStatus == QLatin1StringView("Playing")) {
        return QStringLiteral("playing");
    }
    if (mprisStatus == QLatin1StringView("Paused")) {
        return QStringLiteral("paused");
    }
    return QStringLiteral("stopped");
}

} // namespace

QString mediaActionName(MediaAction action) {
    for (const ActionEntry &e : kActions) {
        if (e.action == action) {
            return QString::fromLatin1(e.name);
        }
    }
    return {};
}

bool mediaActionFromName(const QString &name, MediaAction &action) {
    for (const ActionEntry &e : kActions) {
        if (name == QLatin1StringView(e.name)) {
            action = e.action;
            return true;
        }
    }
    return false;
}

MediaBridge::MediaBridge(QObject *parent)
    : QObject(parent), m_bus(QString()), m_useSessionBus(true) {
    m_changedDebounce.setSingleShot(true);
    m_changedDebounce.setInterval(kChangedDebounceMs);
    connect(&m_changedDebounce, &QTimer::timeout, this, &MediaBridge::changed);

    m_volumePoll.setInterval(kVolumePollMs);
    connect(&m_volumePoll, &QTimer::timeout, this, &MediaBridge::refreshSystemVolume);
}

MediaBridge::MediaBridge(const QDBusConnection &bus, QObject *parent)
    : MediaBridge(parent) {
    m_bus = bus;
    m_useSessionBus = false;
}

MediaBridge::~MediaBridge() {
    if (m_volumeRead) {
        m_volumeRead->disconnect(this);
        m_volumeRead->kill();
        m_volumeRead->waitForFinished(200);
    }
}

void MediaBridge::setVolumeTool(const QString &program) {
    m_volumeTool = program;
    m_volumeToolSet = true;
}

bool MediaBridge::ensureStarted() {
    if (m_started) {
        return m_available;
    }
    m_started = true;

    if (!m_volumeToolSet) {
        m_volumeTool = QStandardPaths::findExecutable(QStringLiteral("wpctl"));
    }

    if (m_useSessionBus) {
        // Only now, on first use: constructing the bridge must not go looking
        // for a session bus — see the class comment.
        if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS")) {
            m_unavailableReason = tr("no desktop session bus — media players cannot be reached");
            return false;
        }
        m_bus = QDBusConnection::sessionBus();
    }
    if (!m_bus.isConnected()) {
        m_unavailableReason = tr("could not connect to the desktop session bus");
        return false;
    }

    m_bus.connect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                  QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameOwnerChanged"),
                  this, SLOT(onNameOwnerChanged(QString, QString, QString)));
    // Empty service: one subscription for every player, matched to a player
    // by the sender's unique name when it arrives.
    m_bus.connect(QString(), kMprisPath, kPropertiesInterface,
                  QStringLiteral("PropertiesChanged"), this,
                  SLOT(onPropertiesChanged(QDBusMessage)));
    m_bus.connect(QString(), kMprisPath, kPlayerInterface, QStringLiteral("Seeked"), this,
                  SLOT(onSeeked(QDBusMessage)));

    m_available = true;
    listPlayers();
    refreshSystemVolume();
    return true;
}

void MediaBridge::setWatching(bool watching) {
    if (watching && m_available && !m_volumeTool.isEmpty()) {
        if (!m_volumePoll.isActive()) {
            m_volumePoll.start();
        }
    } else {
        m_volumePoll.stop();
    }
}

void MediaBridge::refresh() {
    if (!ensureStarted()) {
        scheduleChanged();
        return;
    }
    const auto names = m_players.keys();
    for (const QString &busName : names) {
        refreshPlayer(busName);
    }
    refreshSystemVolume();
    // Also when there is nothing to wait for, so a request always gets an
    // answer.
    scheduleChanged();
}

// ---- discovery ---------------------------------------------------------

void MediaBridge::listPlayers() {
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("ListNames"));
    auto *watcher =
        new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                const QDBusPendingReply<QStringList> reply = *w;
                if (reply.isError()) {
                    qCWarning(lcMedia) << "ListNames failed:" << reply.error().message();
                    return;
                }
                for (const QString &name : reply.value()) {
                    if (name.startsWith(kMprisPrefix)) {
                        addPlayer(name, QString());
                    }
                }
            });
}

void MediaBridge::addPlayer(const QString &busName, const QString &owner) {
    const QString id = playerIdFromBusName(busName);
    if (id.isEmpty()) {
        return;
    }
    if (!m_players.contains(busName) && m_players.size() >= kMaxPlayers) {
        return;
    }
    Player &player = m_players[busName];
    player.busName = busName;
    player.id = id;
    if (!owner.isEmpty()) {
        player.owner = owner;
    } else {
        QDBusMessage call = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
        call << busName;
        auto *watcher =
            new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, busName](QDBusPendingCallWatcher *w) {
                    w->deleteLater();
                    const QDBusPendingReply<QString> reply = *w;
                    auto it = m_players.find(busName);
                    if (it != m_players.end() && !reply.isError()) {
                        it->owner = reply.value();
                    }
                });
    }

    // The player's display name, once.
    QDBusMessage identity = QDBusMessage::createMethodCall(busName, kMprisPath,
                                                           kPropertiesInterface,
                                                           QStringLiteral("Get"));
    identity << QString(kRootInterface) << QStringLiteral("Identity");
    auto *watcher =
        new QDBusPendingCallWatcher(m_bus.asyncCall(identity, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, busName](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                const QDBusPendingReply<QDBusVariant> reply = *w;
                auto it = m_players.find(busName);
                if (it == m_players.end() || reply.isError()) {
                    return;
                }
                it->identity = cleanText(reply.value().variant().toString()).left(64);
                scheduleChanged();
            });

    refreshPlayer(busName);
}

void MediaBridge::onNameOwnerChanged(const QString &name, const QString &oldOwner,
                                     const QString &newOwner) {
    Q_UNUSED(oldOwner);
    if (!name.startsWith(kMprisPrefix)) {
        return;
    }
    if (newOwner.isEmpty()) {
        if (m_players.remove(name) > 0) {
            scheduleChanged();
        }
        return;
    }
    addPlayer(name, newOwner);
}

QString MediaBridge::busNameForOwner(const QString &owner) const {
    for (auto it = m_players.cbegin(); it != m_players.cend(); ++it) {
        if (it->owner == owner) {
            return it.key();
        }
    }
    return {};
}

void MediaBridge::onPropertiesChanged(const QDBusMessage &message) {
    const QString busName = busNameForOwner(message.service());
    if (busName.isEmpty()) {
        return;
    }
    // Re-read rather than apply the delta. Players differ in what they put in
    // a change notice (some send only "Metadata", some invalidate instead of
    // sending values), and a full read is one small call.
    refreshPlayer(busName);
}

void MediaBridge::onSeeked(const QDBusMessage &message) {
    const QString busName = busNameForOwner(message.service());
    auto it = m_players.find(busName);
    if (it == m_players.end() || message.arguments().isEmpty()) {
        return;
    }
    it->positionUs = std::max<qint64>(0, message.arguments().constFirst().toLongLong());
    scheduleChanged();
}

// ---- reading a player --------------------------------------------------

void MediaBridge::refreshPlayer(const QString &busName) {
    auto it = m_players.find(busName);
    if (it == m_players.end()) {
        return;
    }
    if (it->refreshing) {
        // One read in flight per player; a change during it is picked up by
        // one more read afterwards rather than by stacking calls.
        it->dirty = true;
        return;
    }
    it->refreshing = true;
    it->dirty = false;

    QDBusMessage call = QDBusMessage::createMethodCall(busName, kMprisPath, kPropertiesInterface,
                                                       QStringLiteral("GetAll"));
    call << QString(kPlayerInterface);
    auto *watcher =
        new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, busName](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                auto it = m_players.find(busName);
                if (it == m_players.end()) {
                    return;
                }
                it->refreshing = false;
                const QDBusPendingReply<QVariantMap> reply = *w;
                if (!reply.isError()) {
                    applyPlayerProperties(*it, reply.value());
                    scheduleChanged();
                }
                if (it->dirty) {
                    refreshPlayer(busName);
                }
            });
}

void MediaBridge::applyPlayerProperties(Player &player, const QVariantMap &properties) {
    const QString oldStatus = player.status;
    const QString oldTitle = player.title;

    player.status = statusName(properties.value(QStringLiteral("PlaybackStatus")).toString());
    player.canPlay = properties.value(QStringLiteral("CanPlay")).toBool();
    player.canPause = properties.value(QStringLiteral("CanPause")).toBool();
    player.canNext = properties.value(QStringLiteral("CanGoNext")).toBool();
    player.canPrevious = properties.value(QStringLiteral("CanGoPrevious")).toBool();
    player.canSeek = properties.value(QStringLiteral("CanSeek")).toBool();
    player.canControl = properties.value(QStringLiteral("CanControl"), true).toBool();
    player.positionUs = std::max<qint64>(0, properties.value(QStringLiteral("Position")).toLongLong());

    const QVariant volume = properties.value(QStringLiteral("Volume"));
    player.volume = volume.isValid() ? std::clamp(volume.toDouble(), 0.0, 1.0) : -1.0;

    const QVariantMap metadata = toMap(properties.value(QStringLiteral("Metadata")));
    player.title = cleanText(metadata.value(QStringLiteral("xesam:title")).toString());
    player.artist =
        cleanText(toStringList(metadata.value(QStringLiteral("xesam:artist"))).join(QStringLiteral(", ")));
    player.album = cleanText(metadata.value(QStringLiteral("xesam:album")).toString());
    player.lengthUs = std::max<qint64>(0, metadata.value(QStringLiteral("mpris:length")).toLongLong());
    player.trackId = objectPathOf(metadata.value(QStringLiteral("mpris:trackid")));

    if (player.status != oldStatus || player.title != oldTitle) {
        player.lastActivity = ++m_activityCounter;
    }
}

// ---- system volume -----------------------------------------------------

bool MediaBridge::parseWpctlVolume(const QByteArray &output, int &percent, bool &muted) {
    static const QRegularExpression re(QStringLiteral("Volume:\\s*([0-9]+(?:\\.[0-9]+)?)"));
    const QString text = QString::fromUtf8(output);
    const QRegularExpressionMatch match = re.match(text);
    if (!match.hasMatch()) {
        return false;
    }
    bool ok = false;
    const double value = match.captured(1).toDouble(&ok);
    if (!ok || !std::isfinite(value)) {
        return false;
    }
    percent = static_cast<int>(std::lround(std::clamp(value, 0.0, 1.5) * 100.0));
    muted = text.contains(QLatin1StringView("[MUTED]"));
    return true;
}

void MediaBridge::refreshSystemVolume() {
    if (m_volumeTool.isEmpty() || m_volumeRead) {
        return;
    }
    m_volumeRead = new QProcess(this);
    QProcess *process = m_volumeRead;
    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus status) {
        int percent = -1;
        bool muted = false;
        const bool ok = status == QProcess::NormalExit && exitCode == 0
            && parseWpctlVolume(process->readAllStandardOutput(), percent, muted);
        if (!ok) {
            percent = -1;
            muted = false;
        }
        if (percent != m_systemVolume || muted != m_systemMuted) {
            m_systemVolume = percent;
            m_systemMuted = muted;
            scheduleChanged();
        }
        process->deleteLater();
        m_volumeRead = nullptr;
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return; // finished() still follows
        }
        m_systemVolume = -1;
        process->deleteLater();
        m_volumeRead = nullptr;
    });
    QTimer::singleShot(kVolumeToolTimeoutMs, process, [process]() { process->kill(); });
    process->start(m_volumeTool, {QStringLiteral("get-volume"),
                                  QStringLiteral("@DEFAULT_AUDIO_SINK@")});
}

void MediaBridge::runVolumeTool(const QStringList &arguments, bool readBack) {
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process, readBack]() {
        process->deleteLater();
        if (readBack) {
            refreshSystemVolume();
        }
    });
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            process->deleteLater();
        }
    });
    QTimer::singleShot(kVolumeToolTimeoutMs, process, [process]() { process->kill(); });
    process->start(m_volumeTool, arguments);
}

// ---- commands ------------------------------------------------------------

MediaBridge::Player *MediaBridge::playerById(const QString &id) {
    for (auto it = m_players.begin(); it != m_players.end(); ++it) {
        if (it->id == id) {
            return &it.value();
        }
    }
    return nullptr;
}

void MediaBridge::callPlayer(const Player &player, const QString &method,
                             const QList<QVariant> &arguments) {
    QDBusMessage call =
        QDBusMessage::createMethodCall(player.busName, kMprisPath, kPlayerInterface, method);
    call.setArguments(arguments);
    auto *watcher =
        new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
    const QString busName = player.busName;
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, busName, method](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                if (w->isError()) {
                    qCInfo(lcMedia) << method << "failed:" << w->error().message();
                }
                // Players that change state without a PropertiesChanged
                // (some browsers) still get read back.
                refreshPlayer(busName);
            });
}

bool MediaBridge::command(const QString &playerId, MediaAction action, qint64 value,
                          QString &error) {
    if (!ensureStarted()) {
        error = m_unavailableReason;
        return false;
    }

    if (action == MediaAction::SystemVolume || action == MediaAction::SystemMute) {
        if (m_volumeTool.isEmpty()) {
            error = tr("this computer has no volume control tool (wpctl)");
            return false;
        }
        if (action == MediaAction::SystemVolume) {
            if (value < 0 || value > 100) {
                error = tr("volume out of range");
                return false;
            }
            // -l 1.0: never above 100 %, whatever arrives. Formatted from a
            // clamped integer, so the argument is always a plain decimal.
            runVolumeTool({QStringLiteral("set-volume"), QStringLiteral("-l"), QStringLiteral("1.0"),
                           QStringLiteral("@DEFAULT_AUDIO_SINK@"),
                           QString::number(static_cast<double>(value) / 100.0, 'f', 2)},
                          true);
        } else {
            if (value != 0 && value != 1) {
                error = tr("mute takes 0 or 1");
                return false;
            }
            runVolumeTool({QStringLiteral("set-mute"), QStringLiteral("@DEFAULT_AUDIO_SINK@"),
                           value == 1 ? QStringLiteral("1") : QStringLiteral("0")},
                          true);
        }
        return true;
    }

    Player *player = playerById(playerId);
    if (!player) {
        error = tr("that player is no longer running");
        return false;
    }

    switch (action) {
    case MediaAction::Play:
        if (!player->canPlay) {
            error = tr("%1 cannot play right now").arg(player->identity);
            return false;
        }
        callPlayer(*player, QStringLiteral("Play"));
        return true;
    case MediaAction::Pause:
        if (!player->canPause) {
            error = tr("%1 cannot pause right now").arg(player->identity);
            return false;
        }
        callPlayer(*player, QStringLiteral("Pause"));
        return true;
    case MediaAction::PlayPause:
        if (!player->canPlay && !player->canPause) {
            error = tr("%1 cannot play or pause right now").arg(player->identity);
            return false;
        }
        callPlayer(*player, QStringLiteral("PlayPause"));
        return true;
    case MediaAction::Next:
        if (!player->canNext) {
            error = tr("%1 has no next track").arg(player->identity);
            return false;
        }
        callPlayer(*player, QStringLiteral("Next"));
        return true;
    case MediaAction::Previous:
        if (!player->canPrevious) {
            error = tr("%1 has no previous track").arg(player->identity);
            return false;
        }
        callPlayer(*player, QStringLiteral("Previous"));
        return true;
    case MediaAction::Stop:
        if (!player->canControl) {
            error = tr("%1 cannot be controlled").arg(player->identity);
            return false;
        }
        callPlayer(*player, QStringLiteral("Stop"));
        return true;
    case MediaAction::Seek: {
        if (!player->canSeek) {
            error = tr("%1 cannot seek").arg(player->identity);
            return false;
        }
        if (value < 0) {
            error = tr("position out of range");
            return false;
        }
        qint64 targetUs = value * 1000;
        if (player->lengthUs > 0) {
            targetUs = std::min(targetUs, player->lengthUs);
        }
        targetUs = std::max<qint64>(0, targetUs);
        if (!player->trackId.isEmpty() && player->trackId != kNoTrack
            && player->trackId.startsWith(QLatin1Char('/'))) {
            // SetPosition is absolute and tied to the track, so a seek that
            // lands after a track change is ignored instead of jumping into
            // the wrong song.
            callPlayer(*player, QStringLiteral("SetPosition"),
                       {QVariant::fromValue(QDBusObjectPath(player->trackId)),
                        QVariant::fromValue(static_cast<qlonglong>(targetUs))});
        } else {
            callPlayer(*player, QStringLiteral("Seek"),
                       {QVariant::fromValue(static_cast<qlonglong>(targetUs - player->positionUs))});
        }
        player->positionUs = targetUs;
        return true;
    }
    case MediaAction::SetVolume: {
        if (player->volume < 0) {
            error = tr("%1 has no volume of its own").arg(player->identity);
            return false;
        }
        if (value < 0 || value > 100) {
            error = tr("volume out of range");
            return false;
        }
        QDBusMessage call = QDBusMessage::createMethodCall(player->busName, kMprisPath,
                                                           kPropertiesInterface,
                                                           QStringLiteral("Set"));
        call << QString(kPlayerInterface) << QStringLiteral("Volume")
             << QVariant::fromValue(QDBusVariant(static_cast<double>(value) / 100.0));
        auto *watcher =
            new QDBusPendingCallWatcher(m_bus.asyncCall(call, kCallTimeoutMs), this);
        const QString busName = player->busName;
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, busName](QDBusPendingCallWatcher *w) {
                    w->deleteLater();
                    refreshPlayer(busName);
                });
        return true;
    }
    case MediaAction::SystemVolume:
    case MediaAction::SystemMute:
        break; // handled above
    }
    error = tr("unknown action");
    return false;
}

// ---- output ---------------------------------------------------------------

void MediaBridge::scheduleChanged() {
    m_changedDebounce.start();
}

QJsonObject MediaBridge::snapshot() const {
    QList<const Player *> ordered;
    for (const Player &player : m_players) {
        ordered.append(&player);
    }
    // Playing before paused before stopped, and within each the most recent
    // to change first: the one at the top is the one the user means.
    auto rank = [](const Player *p) {
        if (p->status == QLatin1StringView("playing")) {
            return 0;
        }
        return p->status == QLatin1StringView("paused") ? 1 : 2;
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&](const Player *a, const Player *b) {
        if (rank(a) != rank(b)) {
            return rank(a) < rank(b);
        }
        return a->lastActivity > b->lastActivity;
    });

    QJsonArray players;
    for (const Player *p : ordered) {
        QJsonObject entry;
        entry.insert(QLatin1StringView("id"), p->id);
        entry.insert(QLatin1StringView("name"), p->identity.isEmpty() ? p->id : p->identity);
        entry.insert(QLatin1StringView("status"), p->status.isEmpty() ? QStringLiteral("stopped")
                                                                      : p->status);
        entry.insert(QLatin1StringView("title"), p->title);
        entry.insert(QLatin1StringView("artist"), p->artist);
        entry.insert(QLatin1StringView("album"), p->album);
        entry.insert(QLatin1StringView("lengthMs"), p->lengthUs / 1000);
        entry.insert(QLatin1StringView("positionMs"),
                     p->lengthUs > 0 ? std::min(p->positionUs, p->lengthUs) / 1000
                                     : p->positionUs / 1000);
        entry.insert(QLatin1StringView("canPlay"), p->canPlay);
        entry.insert(QLatin1StringView("canPause"), p->canPause);
        entry.insert(QLatin1StringView("canNext"), p->canNext);
        entry.insert(QLatin1StringView("canPrevious"), p->canPrevious);
        entry.insert(QLatin1StringView("canSeek"), p->canSeek && p->lengthUs > 0);
        if (p->volume >= 0) {
            entry.insert(QLatin1StringView("volume"),
                         static_cast<int>(std::lround(p->volume * 100.0)));
        }
        players.append(entry);
    }

    QJsonObject media;
    media.insert(QLatin1StringView("players"), players);
    if (!ordered.isEmpty()) {
        media.insert(QLatin1StringView("active"), ordered.constFirst()->id);
    }
    if (m_systemVolume >= 0) {
        media.insert(QLatin1StringView("systemVolume"), std::min(m_systemVolume, 150));
        media.insert(QLatin1StringView("systemMuted"), m_systemMuted);
    }
    return media;
}

// ---- helpers ---------------------------------------------------------------

QString MediaBridge::playerIdFromBusName(const QString &busName) {
    if (!busName.startsWith(kMprisPrefix)) {
        return {};
    }
    const QString id = busName.mid(kMprisPrefix.size());
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_.-]{1,64}$"));
    return valid.match(id).hasMatch() ? id : QString();
}

QString MediaBridge::cleanText(const QString &text) {
    QString out;
    out.reserve(std::min<qsizetype>(text.size(), kMaxTextChars));
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        const bool control = u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F);
        out.append(control ? QChar(u' ') : c);
        if (out.size() >= kMaxTextChars) {
            break;
        }
    }
    return out.trimmed();
}

} // namespace mazeconnect::core
