#include "Tray.h"

#include "Backend.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QIcon>
#include <QMenu>
#include <QQuickWindow>
#include <QSystemTrayIcon>

#include <utility>

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

        // The one action worth having without opening a window: the phone is
        // lost somewhere in the house, and the window is one more step.
        auto *findPhone = m_menu->addAction(tr("Find my phone"));
        connect(findPhone, &QAction::triggered, backend, &Backend::ringAllPhones);
    }

    m_menu->addSeparator();

    // Named "Quit" rather than "Close": closing the window leaves the link
    // up, and the menu should not use the same word for two different things.
    auto *quit = m_menu->addAction(tr("Quit — disconnects your phone"));
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    m_icon->setContextMenu(m_menu);
    m_icon->setToolTip(tr("Maze Connect"));

    connect(m_icon, &QSystemTrayIcon::messageClicked, this, [this] {
        // Taken, not peeked: a second click on an older notification must
        // not open a link that has since been replaced or already opened.
        const QUrl url = std::exchange(m_pendingUrl, QUrl());
        const QString scheme = url.scheme().toLower();
        if (url.isValid()
            && (scheme == QLatin1StringView("http") || scheme == QLatin1StringView("https"))) {
            QDesktopServices::openUrl(url);
        }
    });

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
    m_connectedCount = count;
    updateToolTip();
}

void Tray::setPhoneSummary(const QString &summary) {
    m_phoneSummary = summary;
    updateToolTip();
}

void Tray::updateToolTip() {
    if (m_icon == nullptr) {
        return;
    }
    QString tip = m_connectedCount > 0
        ? tr("Maze Connect — %n device(s) linked", nullptr, m_connectedCount)
        : tr("Maze Connect — nothing linked");
    if (!m_phoneSummary.isEmpty()) {
        tip += QLatin1Char('\n') + m_phoneSummary;
    }
    m_icon->setToolTip(tip);
}

void Tray::notify(const QString &title, const QString &body, const QUrl &url) {
    if (m_icon == nullptr) {
        return;
    }
    m_pendingUrl = url;
    m_icon->showMessage(title, body, m_icon->icon(), 8000);
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
