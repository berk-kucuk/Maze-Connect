#include "Autostart.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace autostart {
namespace {

QString configHome() {
    const QString fromEnv = QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME"));
    if (!fromEnv.isEmpty()) {
        return fromEnv;
    }
    return QDir::homePath() + QStringLiteral("/.config");
}

/// Records that the default has been applied. Separate from the entry itself
/// so that deleting the entry is a decision we respect rather than a state we
/// "correct" on the next launch.
QString markerPath() {
    return configHome() + QStringLiteral("/mazeconnect/autostart-decided");
}

} // namespace

QString entryPath() {
    return configHome() + QStringLiteral("/autostart/maze-connect.desktop");
}

bool isEnabled() {
    return QFileInfo::exists(entryPath());
}

bool setEnabled(bool enabled) {
    const QString path = entryPath();
    if (!enabled) {
        QFile::remove(path);
        return true;
    }

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }

    QTextStream out(&file);
    out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=Maze Connect\n"
        << "Comment=Keep the link to your phone up\n"
        << "Exec=maze-connect --tray\n"
        << "Icon=maze-connect\n"
        << "Terminal=false\n"
        // Starts hidden in the tray. Autostart exists so the link is up, not
        // so a window is in the way of whatever the user actually logged in
        // to do.
        << "X-GNOME-Autostart-enabled=true\n"
        << "X-KDE-autostart-after=panel\n";
    return true;
}

void applyDefaultOnFirstRun() {
    const QString marker = markerPath();
    if (QFileInfo::exists(marker)) {
        // Already decided — but if the entry is still there, rewrite it. The
        // command line inside it belongs to the version that wrote it, and
        // that has already gone wrong once: an entry written before `--tray`
        // existed kept launching the window at every login, and no upgrade
        // path touched it because the decision had been made. Refreshing
        // content is not the same as re-enabling; an entry the user removed
        // stays removed.
        if (isEnabled()) {
            setEnabled(true);
        }
        return;
    }

    setEnabled(true);

    QDir().mkpath(QFileInfo(marker).absolutePath());
    QFile file(marker);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write("Autostart was enabled on first run. Delete the entry in\n"
                   "~/.config/autostart to turn it off; this file only stops it\n"
                   "being turned back on.\n");
    }
}

} // namespace autostart
