#include "mazeconnect/core/CommandRunner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QTimer>

namespace mazeconnect::core {
namespace {

Q_LOGGING_CATEGORY(lcCommands, "maze.connect.commands")

bool hasControlCharacters(const QString &s) {
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)) {
            return true;
        }
    }
    return false;
}

} // namespace

CommandRunner::CommandRunner(QObject *parent) : QObject(parent) {}

CommandRunner::~CommandRunner() {
    for (auto &running : m_running) {
        if (running.process) {
            running.process->disconnect(this);
            running.process->kill();
            running.process->waitForFinished(1000);
        }
    }
}

QString CommandRunner::commandsFilePath() {
    const QString override = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("MAZECONNECT_COMMANDS_FILE"));
    if (!override.isEmpty()) {
        return override;
    }
    QString base = QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME"));
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.config");
    }
    return base + QStringLiteral("/mazeconnect/commands.json");
}

QList<Command> CommandRunner::catalog() const {
    m_lastError.clear();

    const QString path = commandsFilePath();
    QFileInfo info(path);
    if (!info.exists()) {
        // Not an error. Someone who has not written a command file simply has
        // no commands, and the phone should say that rather than "failed".
        return {};
    }

    // This file decides what runs as this user. If anyone else can write it,
    // they can run code as this user, and the contents cannot be trusted to
    // be the user's own. Refusing is the only reading of that bit that does
    // not quietly execute someone else's list.
    const QFile::Permissions perms = info.permissions();
    const QFile::Permissions othersCanTouch =
        QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
        QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    if (perms & othersCanTouch) {
        m_lastError = QStringLiteral("commands.json must be readable only by you (chmod 600)");
        qCWarning(lcCommands) << "refusing command file with permissions" << perms;
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = QStringLiteral("commands.json could not be read");
        return {};
    }
    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray()) {
        m_lastError = QStringLiteral("commands.json is not a JSON array");
        return {};
    }

    const QJsonArray array = doc.array();
    if (array.size() > kMaxCommands) {
        m_lastError = QStringLiteral("commands.json has more than %1 entries").arg(kMaxCommands);
        return {};
    }

    QList<Command> out;
    QSet<QString> seen;
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();

        Command command;
        command.id = obj.value(QLatin1StringView("id")).toString();
        command.label = obj.value(QLatin1StringView("label")).toString();
        command.confirm = obj.value(QLatin1StringView("confirm")).toBool(false);
        command.pinned = obj.value(QLatin1StringView("pinned")).toBool(false);

        if (command.id.isEmpty() || command.id.size() > kMaxIdChars
            || hasControlCharacters(command.id)) {
            continue;
        }
        // Ids are what a phone sends, and they are matched exactly. A
        // duplicate would make "the entry with this id" ambiguous, which is
        // not a question this class should be answering by guessing.
        if (seen.contains(command.id)) {
            m_lastError = QStringLiteral("duplicate command id: %1").arg(command.id);
            return {};
        }

        if (command.label.size() > kMaxLabelChars || hasControlCharacters(command.label)) {
            continue;
        }
        if (command.label.isEmpty()) {
            command.label = command.id;
        }

        const QJsonValue argvValue = obj.value(QLatin1StringView("argv"));
        if (!argvValue.isArray()) {
            continue;
        }
        const QJsonArray argvArray = argvValue.toArray();
        if (argvArray.isEmpty() || argvArray.size() > kMaxArgs) {
            continue;
        }
        bool argvOk = true;
        for (const QJsonValue &arg : argvArray) {
            if (!arg.isString()) {
                argvOk = false;
                break;
            }
            const QString text = arg.toString();
            if (text.size() > kMaxArgChars || hasControlCharacters(text)) {
                argvOk = false;
                break;
            }
            command.argv << text;
        }
        if (!argvOk || command.argv.isEmpty()) {
            continue;
        }

        seen.insert(command.id);
        out << command;
    }
    return out;
}

bool CommandRunner::isWithinBounds(const Command &command) const {
    if (command.id.isEmpty() || command.id.size() > kMaxIdChars
        || hasControlCharacters(command.id)) {
        return false;
    }
    if (command.label.size() > kMaxLabelChars || hasControlCharacters(command.label)) {
        return false;
    }
    if (command.argv.isEmpty() || command.argv.size() > kMaxArgs) {
        return false;
    }
    for (const QString &arg : command.argv) {
        if (arg.isEmpty() || arg.size() > kMaxArgChars || hasControlCharacters(arg)) {
            return false;
        }
    }
    return true;
}

bool CommandRunner::writeCatalog(const QList<Command> &commands) {
    const QString path = commandsFilePath();
    const QFileInfo info(path);
    QDir().mkpath(info.absolutePath());

    QJsonArray array;
    for (const Command &command : commands) {
        QJsonObject obj;
        obj.insert(QLatin1StringView("id"), command.id);
        obj.insert(QLatin1StringView("label"), command.label);
        obj.insert(QLatin1StringView("confirm"), command.confirm);
        obj.insert(QLatin1StringView("pinned"), command.pinned);
        QJsonArray argv;
        for (const QString &arg : command.argv) {
            argv.append(arg);
        }
        obj.insert(QLatin1StringView("argv"), argv);
        array.append(obj);
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_lastError = QStringLiteral("commands.json could not be written");
        return false;
    }
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    file.close();
    // Owner-only, same as the starter file: this decides what runs as you,
    // and a file anyone else can write is a way to run code as this user.
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

bool CommandRunner::addCommand(const Command &command) {
    if (!isWithinBounds(command)) {
        m_lastError = QStringLiteral("command does not meet the size limits");
        return false;
    }

    QList<Command> commands = catalog();
    // A non-empty error here means the existing file is broken (bad
    // permissions, malformed JSON) rather than merely absent — writing a
    // new file over it would silently discard whatever was salvageable.
    if (!m_lastError.isEmpty()) {
        return false;
    }
    if (commands.size() >= kMaxCommands) {
        m_lastError = QStringLiteral("already at %1 commands").arg(kMaxCommands);
        return false;
    }
    if (std::any_of(commands.cbegin(), commands.cend(),
                    [&command](const Command &c) { return c.id == command.id; })) {
        m_lastError = QStringLiteral("a command with this id already exists");
        return false;
    }

    commands << command;
    return writeCatalog(commands);
}

bool CommandRunner::updateCommand(const QString &id, const Command &updated) {
    if (!isWithinBounds(updated)) {
        m_lastError = QStringLiteral("command does not meet the size limits");
        return false;
    }

    QList<Command> commands = catalog();
    if (!m_lastError.isEmpty()) {
        return false;
    }
    const auto it = std::find_if(commands.begin(), commands.end(),
                                 [&id](const Command &c) { return c.id == id; });
    if (it == commands.end()) {
        m_lastError = QStringLiteral("no command with that id");
        return false;
    }
    // The id may change, but must not collide with a *different* existing
    // entry — colliding with itself (the common case: nothing else changed)
    // is fine.
    if (updated.id != id
        && std::any_of(commands.cbegin(), commands.cend(),
                       [&updated](const Command &c) { return c.id == updated.id; })) {
        m_lastError = QStringLiteral("a command with this id already exists");
        return false;
    }

    *it = updated;
    return writeCatalog(commands);
}

bool CommandRunner::removeCommand(const QString &id) {
    QList<Command> commands = catalog();
    if (!m_lastError.isEmpty()) {
        return false;
    }
    const qsizetype removed = commands.removeIf([&id](const Command &c) { return c.id == id; });
    if (removed == 0) {
        m_lastError = QStringLiteral("no command with that id");
        return false;
    }
    return writeCatalog(commands);
}

quint32 CommandRunner::run(const QString &id) {
    if (m_running.size() >= kMaxConcurrent) {
        qCWarning(lcCommands) << "refusing run: already at the concurrency limit";
        return 0;
    }

    // Exact match against the file, every time. This lookup is the entire
    // authorisation: an id that is not here does not run, and an id that is
    // here runs *this* argv rather than anything resembling it.
    const QList<Command> commands = catalog();
    const auto it = std::find_if(commands.cbegin(), commands.cend(),
                                 [&id](const Command &c) { return c.id == id; });
    if (it == commands.cend()) {
        qCInfo(lcCommands) << "refusing unknown command id";
        return 0;
    }

    const quint32 requestId = m_nextRequestId++;
    Running running;
    running.id = it->id;
    running.process = new QProcess(this);
    // Program and arguments, never one string. There is no shell here to
    // interpret metacharacters, so there is nothing to escape.
    running.process->setProgram(it->argv.first());
    running.process->setArguments(it->argv.mid(1));
    running.process->setProcessChannelMode(QProcess::MergedChannels);

    QProcess *process = running.process;
    m_running.insert(requestId, running);

    connect(process, &QProcess::readyReadStandardOutput, this, [this, requestId, process] {
        const auto it = m_running.find(requestId);
        if (it == m_running.end()) {
            return;
        }
        it->output.append(process->readAllStandardOutput());
        if (it->output.size() > kMaxOutputBytes) {
            it->output.truncate(kMaxOutputBytes);
            // Killed rather than merely truncated: a command still writing is
            // still consuming, and the extra output is going nowhere.
            process->kill();
        }
    });

    connect(process, &QProcess::errorOccurred, this,
            [this, requestId](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    report(requestId, -1, false);
                }
            });

    connect(process, &QProcess::finished, this,
            [this, requestId](int exitCode, QProcess::ExitStatus status) {
                report(requestId, status == QProcess::NormalExit ? exitCode : -1, false);
            });

    // Bound on the identity of this process, not merely on one being alive:
    // a later run must not be killed by its predecessor's timer.
    QPointer<QProcess> watched(process);
    QTimer::singleShot(kTimeoutMs, this, [this, requestId, watched] {
        const auto it = m_running.find(requestId);
        if (it == m_running.end() || it->process != watched || !watched) {
            return;
        }
        it->timedOut = true;
        watched->kill();
    });

    process->start();
    return requestId;
}

void CommandRunner::report(quint32 requestId, int exitCode, bool timedOut) {
    const auto it = m_running.find(requestId);
    if (it == m_running.end()) {
        return;
    }
    const QString id = it->id;
    const bool wasTimeout = timedOut || it->timedOut;
    QString output = QString::fromUtf8(it->output);

    if (it->process) {
        it->process->disconnect(this);
        it->process->deleteLater();
    }
    m_running.erase(it);

    if (wasTimeout) {
        output += QStringLiteral("\n[timed out after %1s]").arg(kTimeoutMs / 1000);
    }
    emit finished(requestId, id, exitCode, output, wasTimeout);
}

} // namespace mazeconnect::core
