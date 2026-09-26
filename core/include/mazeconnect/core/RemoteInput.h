#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <memory>

namespace mazeconnect::core {

/**
 * Where synthetic input actually goes.
 *
 * An interface so the part that decides *what may be injected* — RemoteInput
 * — can be tested without a desktop, and so the one implementation that
 * touches the session stays small enough to read in one sitting.
 */
class InputBackend : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    /// Ask for an input session. Answered by started().
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool isActive() const = 0;

    virtual void pointerMotion(double dx, double dy) = 0;
    /// @p button is a Linux evdev code (BTN_LEFT = 0x110 …).
    virtual void pointerButton(int button, bool pressed) = 0;
    /// @p axis 0 vertical, 1 horizontal; @p steps in wheel clicks.
    virtual void pointerScroll(int axis, int steps) = 0;
    virtual void keysym(int keysym, bool pressed) = 0;

signals:
    void started(bool ok, const QString &error);
    /// The session ended on the desktop's side — revoked, closed, lost.
    void stopped(const QString &reason);
};

/**
 * The desktop's RemoteDesktop portal (org.freedesktop.portal.RemoteDesktop).
 *
 * On Wayland this is the only sanctioned way for an application to move the
 * pointer or press keys, and that is what makes it the right one: the first
 * session shows the desktop's **own** consent dialog, on this screen, which a
 * phone cannot answer. With persist_mode the consent is remembered
 * (restore_token, kept in QSettings) until revoked in System Settings; KWin
 * also shows its own "being controlled" indicator while a session runs.
 *
 * Pointer and keyboard only — no screen cast, so nothing on this screen is
 * ever captured.
 */
class PortalInputBackend : public InputBackend {
    Q_OBJECT

public:
    explicit PortalInputBackend(QObject *parent = nullptr);
    ~PortalInputBackend() override;

    void start() override;
    void stop() override;
    bool isActive() const override { return m_active; }

    void pointerMotion(double dx, double dy) override;
    void pointerButton(int button, bool pressed) override;
    void pointerScroll(int axis, int steps) override;
    void keysym(int keysym, bool pressed) override;

private slots:
    void onCreateSession(uint response, const QVariantMap &results);
    void onSelectDevices(uint response, const QVariantMap &results);
    void onStart(uint response, const QVariantMap &results);
    void onSessionClosed();

private:
    /// Send a portal request; @p slot receives its Response.
    bool request(const QString &method, const QVariantList &args, const QString &token,
                 const char *slot);
    void notify(const QString &method, const QVariantList &args);
    void fail(const QString &error);

    QString m_session;
    bool m_active = false;
    bool m_starting = false;
    int m_tokenCounter = 0;
};

/**
 * Who may inject what, and when it stops.
 *
 * One controlling device at a time. Two modes:
 *  * **Presenter** — a fixed handful of keys (next, previous, start, end,
 *    blank). Nothing that can type or point.
 *  * **Full** — pointer, buttons, scroll, keys with modifiers, and text.
 *
 * Every event is bounded here before it reaches the backend: motion is
 * clamped, keys are looked up by name in a fixed table (a phone never sends
 * a keysym number), text is capped and control characters other than tab and
 * newline are refused, and at most kMaxEventsPerSecond get through per
 * second. A session ends by itself after kIdleTimeoutMs without input, when
 * the device disconnects, and whenever the desktop ends it.
 */
class RemoteInput : public QObject {
    Q_OBJECT

public:
    enum class Mode { Presenter, Full };
    Q_ENUM(Mode)

    static constexpr int kIdleTimeoutMs = 120000;
    static constexpr int kMaxEventsPerSecond = 240;
    static constexpr double kMaxMotion = 400.0;
    static constexpr int kMaxScrollSteps = 20;
    static constexpr int kMaxTextChars = 256;

    /// Takes ownership of @p backend; null means the portal one.
    explicit RemoteInput(InputBackend *backend = nullptr, QObject *parent = nullptr);

    /// Start (or switch the mode of) a session for @p deviceId. False when
    /// another device holds it. The outcome arrives as stateChanged().
    bool begin(const QString &deviceId, Mode mode);
    /// End it, if @p deviceId holds it.
    void end(const QString &deviceId, const QString &reason = QString());

    bool isActive() const { return m_backend->isActive() && !m_owner.isEmpty(); }
    QString owner() const { return m_owner; }
    Mode mode() const { return m_mode; }

    /**
     * Apply one event from @p deviceId. The event is the `inputEvent`
     * message body; see docs/PROTOCOL.md § remoteInput. Returns false when it
     * was refused — wrong device, not started, not allowed in this mode,
     * malformed, or over the rate limit.
     */
    bool handleEvent(const QString &deviceId, const QVariantMap &event);

    /// Key names a phone may send, and which of them presenter mode allows.
    static bool keysymForName(const QString &name, int &keysym);
    static bool isPresenterKey(const QString &name);

signals:
    void stateChanged(const QString &deviceId, bool active, Mode mode, const QString &error);

private:
    bool allowRate();
    void tap(int keysym, const QList<int> &modifiers);
    void touch();

    std::unique_ptr<InputBackend> m_backend;
    QString m_owner;
    QString m_pending; ///< asked to start, awaiting the backend
    Mode m_mode = Mode::Presenter;
    QTimer m_idle;
    QElapsedTimer m_clock;
    qint64 m_windowStart = 0;
    int m_windowCount = 0;
};

} // namespace mazeconnect::core
