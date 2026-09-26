#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickWindow>

#include "Autostart.h"
#include "Backend.h"
#include "SingleInstance.h"
#include "Tray.h"
#include "WindowEffects.h"

int main(int argc, char *argv[]) {
    // QApplication rather than QGuiApplication: QSystemTrayIcon lives in
    // QtWidgets, and a link that vanishes when its window closes is not a
    // link. Nothing else here uses a widget.
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Maze Connect"));
    QApplication::setOrganizationName(QStringLiteral("Maze Linux"));
    QApplication::setOrganizationDomain(QStringLiteral("mazelinux.org"));
    QApplication::setDesktopFileName(QStringLiteral("maze-connect"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("maze-connect")));

    // On the first run ever, and never again — see Autostart.h.
    autostart::applyDefaultOnFirstRun();

    // One copy per session. Two would fight over the discovery port and
    // present the same identity from two listeners.
    SingleInstance instance(QStringLiteral("maze-connect.%1").arg(qgetenv("USER").constData()));
    if (!instance.tryAcquire()) {
        return 0; // the running copy has been asked to show itself
    }

    QQmlApplicationEngine engine;

    // Created here rather than by QML so networking is up before the first
    // frame, and so the QML engine never owns the object holding key
    // material.
    auto *backend = new Backend(&app);
    backend->start();
    qmlRegisterSingletonInstance("MazeConnect.App", 1, 0, "Backend", backend);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("MazeConnect.App", "Main");

    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (window != nullptr) {
        auto *tray = new Tray(window, backend, &app);
        if (tray->isAvailable()) {
            // Closing hides; only the tray's Quit stops the service. Without
            // this the window's close button would end the app and take the
            // phone's link with it.
            QApplication::setQuitOnLastWindowClosed(false);
            tray->setConnectedCount(backend->connectedCount());
            QObject::connect(backend, &Backend::devicesChanged, tray, [tray, backend] {
                tray->setConnectedCount(backend->connectedCount());
            });
            QObject::connect(backend, &Backend::phonesChanged, tray, [tray, backend] {
                tray->setPhoneSummary(backend->phoneSummary());
            });
            QObject::connect(backend, &Backend::notificationRequested, tray, &Tray::notify);

        }

        // `--tray` is how the autostart entry launches us: at login the point
        // is to have the link up, not to put a window in front of whatever the
        // user actually logged in to do. The window starts hidden either way,
        // so this decides whether it is ever shown.
        //
        // Only honoured when there is a tray to find it in again — otherwise
        // starting hidden would mean starting unreachable.
        const bool startHidden =
            tray->isAvailable() && QApplication::arguments().contains(QStringLiteral("--tray"));
        if (!startHidden) {
            window->show();
        }

        // Matches Theme.radiusPanel; the blur region has to follow the
        // rounded panel or the corners read as bright squares.
        WindowEffects::applyBlur(window, 22);

        QObject::connect(&instance, &SingleInstance::raiseRequested, window, [window]() {
            window->show();
            window->raise();
            window->requestActivate();
        });
    }

    return app.exec();
}
