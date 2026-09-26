#pragma once

#include <QObject>
#include <QUrl>

class QQuickWindow;
class QSystemTrayIcon;
class QMenu;
class Backend;

/**
 * Tray icon, and the reason the window can be closed without quitting.
 *
 * Maze Connect is a link, not a document: closing its window should leave the
 * phone connected, the same way closing a chat client's window does. So the
 * close button hides, the tray icon brings it back, and **Quit is the only
 * thing that actually stops the service** — spelled out in the menu rather
 * than implied, because a link that keeps running after its window is gone is
 * exactly the kind of thing a user should be able to find and stop.
 *
 * If the desktop has no system tray this hides nothing: without somewhere to
 * bring the window back from, closing has to mean quitting.
 */
class Tray : public QObject {
    Q_OBJECT

public:
    /// @p backend backs the "Send clipboard to phone" action. Never null in
    /// practice — main.cpp always has one by the time a Tray is created —
    /// but the menu item is simply omitted if it were.
    explicit Tray(QQuickWindow *window, Backend *backend, QObject *parent = nullptr);

    /// False when the desktop offers no tray. The caller then leaves the
    /// close button quitting, rather than hiding the window forever.
    bool isAvailable() const;

    /// Reflect the link state in the tooltip, so hovering answers "is my
    /// phone connected" without opening anything.
    void setConnectedCount(int count);

    /// Add the linked phones and their battery to the tooltip.
    void setPhoneSummary(const QString &summary);

    /**
     * Raise a desktop notification. When @p url is set a click on the
     * notification opens it — the only way a link from a phone is ever
     * opened, so nothing reaches the browser without a person choosing it.
     */
    void notify(const QString &title, const QString &body, const QUrl &url);

private:
    void toggleWindow();
    void updateToolTip();

    int m_connectedCount = 0;
    QString m_phoneSummary;
    QUrl m_pendingUrl;

    QQuickWindow *m_window = nullptr;
    QSystemTrayIcon *m_icon = nullptr;
    QMenu *m_menu = nullptr;
};
