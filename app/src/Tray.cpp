#include "Tray.h"

#include "Backend.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QQuickWindow>
#include <QSystemTrayIcon>

Tray::Tray(QQuickWindow *window, Backend *backend, QObject *parent)
    : QObject(parent), m_window(window) {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    // The installed hicolor icon, by name. Falls back to the ground-free mark
    // if the theme lookup misses — running from a build tree, say — so the
    // tray never ends up with a blank square.
    QIcon icon = QIcon::fromTheme(QStringLiteral("maze-connect"));
    if (icon.isNull()) {
        icon = QIcon(QStringLiteral("/usr/share/pixmaps/maze-connect-mark.png"));
    }

    m_icon = new QSystemTrayIcon(icon, this);
    m_menu = new QMenu();

    auto *show = m_menu->addAction(tr("Show Maze Connect"));
    connect(show, &QAction::triggered, this, [this] {
        m_window->show();
        m_window->raise();
        m_window->requestActivate();
    });

    if (backend != nullptr) {
        auto *sendClipboard = m_menu->addAction(tr("Send clipboard to phone"));
        connect(sendClipboard, &QAction::triggered, backend, &Backend::sendClipboardToPhone);
    }

    m_menu->addSeparator();

    // Named "Quit" rather than "Close": closing the window leaves the link
    // up, and the menu should not use the same word for two different things.
    auto *quit = m_menu->addAction(tr("Quit — disconnects your phone"));
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    m_icon->setContextMenu(m_menu);
    m_icon->setToolTip(tr("Maze Connect"));

    connect(m_icon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger) {
                    toggleWindow();
                }
            });

    m_icon->show();
}

bool Tray::isAvailable() const {
    return m_icon != nullptr;
}

void Tray::setConnectedCount(int count) {
    if (m_icon == nullptr) {
        return;
    }
    m_icon->setToolTip(count > 0 ? tr("Maze Connect — %n device(s) linked", nullptr, count)
                                 : tr("Maze Connect — nothing linked"));
}

void Tray::toggleWindow() {
    if (m_window->isVisible() && m_window->isActive()) {
        m_window->hide();
        return;
    }
    m_window->show();
    m_window->raise();
    m_window->requestActivate();
}
