#pragma once

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

namespace mazeconnect::core {

/// One entry from the user's command file.
struct Command {
    QString id;        ///< what a phone sends; never anything else
    QString label;     ///< what the phone shows
    QStringList argv;  ///< program + arguments, taken only from this file
    bool confirm = false; ///< phone asks the user again before sending the run
    /// Shown on the phone's home-screen widget. Set only from this
    /// computer's own editor — a phone can run a pinned entry, never pin
    /// or unpin one.
    bool pinned = false;

    bool isValid() const { return !id.isEmpty() && !argv.isEmpty(); }
};

/**
 * Runs commands the user defined on this machine, and only those.
 *
 * The whole design is one sentence: **the phone sends an id, never a command.**
 * A remote device picks a row out of a list the user wrote by hand; it cannot
 * compose, extend, quote or parameterise anything. If an id does not match an
 * entry exactly, nothing runs.
 *
 * That is why there is no shell anywhere in this class. `argv` is passed to
 * QProcess as a program and a list of arguments, so quoting, globbing,
 * `$(...)`, `;` and `&&` have no meaning at any point — not because they are
 * filtered, but because no shell is ever invoked to interpret them. Filtering
 * a string for shell metacharacters is a fight nobody wins; not having a
 * shell is not a fight at all.
 *
 * The catalogue is re-read from disk each time it is asked for, so editing
 * the file does not require restarting anything.
 */
class CommandRunner : public QObject {
    Q_OBJECT

public:
    explicit CommandRunner(QObject *parent = nullptr);
    ~CommandRunner() override;

    /**
     * `~/.config/mazeconnect/commands.json`, honouring $XDG_CONFIG_HOME.
     * $MAZECONNECT_COMMANDS_FILE overrides it for tests.
     */
    static QString commandsFilePath();

    /**
     * Load and validate the file.
     *
     * Returns an empty list when the file is absent — that is the normal
     * state for someone who has not set any commands up, not an error. It
     * also returns empty when the file is readable by anyone but its owner:
     * a world-writable command list is a way to run code as this user, and
     * refusing it is the only safe reading of that permission bit.
     */
    QList<Command> catalog() const;

    /// Last reason catalog() came back empty, for showing the user.
    QString lastError() const { return m_lastError; }

    /**
     * Add a new entry to the file.
     *
     * Validated against exactly the bounds catalog() enforces on the way
     * back in — an entry this accepted and catalog() then silently dropped
     * would be a command that appeared in the editor and never ran. Fails
     * if the id is already taken, a bound is exceeded, or the existing file
     * itself does not parse (fixing a broken file is not this method's job).
     */
    bool addCommand(const Command &command);

    /// Replace the entry with this id, keeping its position in the file.
    /// Fails if no entry has that id, or the replacement does not validate.
    bool updateCommand(const QString &id, const Command &updated);

    /// Remove the entry with this id. Returns false if there was none.
    bool removeCommand(const QString &id);

    /**
     * Run the entry with this id.
     *
     * Returns the run's handle, or **0** if nothing was started — the id is
     * not in the catalogue, or too many commands are already running. The
     * same handle comes back on finished().
     *
     * Handles are allocated here rather than passed in. There are two callers
     * (the link and the local window), and letting each pick its own numbers
     * would eventually have one run's output delivered against the other's
     * request.
     */
    quint32 run(const QString &id);

    /// Longest a command may run before it is killed.
    static constexpr int kTimeoutMs = 30000;

    /// Combined stdout+stderr kept per command. Anything past this is
    /// dropped with a marker: a command that prints without end must not be
    /// able to grow a control frame without bound.
    static constexpr int kMaxOutputBytes = 16 * 1024;

    /// Commands in flight at once, across all devices.
    static constexpr int kMaxConcurrent = 4;

    /// Bounds on what the file may contain, enforced on load.
    static constexpr int kMaxCommands = 64;
    static constexpr int kMaxIdChars = 64;
    static constexpr int kMaxLabelChars = 64;
    static constexpr int kMaxArgs = 32;
    static constexpr int kMaxArgChars = 512;

signals:
    /// A command finished, was killed, or could not be started.
    void finished(quint32 requestId, const QString &id, int exitCode,
                  const QString &output, bool timedOut);

private:
    struct Running {
        QProcess *process = nullptr;
        QString id;
        QByteArray output;
        bool timedOut = false;
    };

    void report(quint32 requestId, int exitCode, bool timedOut);

    /// The same bounds catalog() enforces on the way in, checked here on the
    /// way out so nothing written by addCommand()/updateCommand() could ever
    /// fail to read back.
    bool isWithinBounds(const Command &command) const;

    /// Overwrites the file with exactly this list, in this order.
    bool writeCatalog(const QList<Command> &commands);

    QHash<quint32, Running> m_running;
    quint32 m_nextRequestId = 1;
    mutable QString m_lastError;
};

} // namespace mazeconnect::core
