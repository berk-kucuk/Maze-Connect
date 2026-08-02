#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "mazeconnect/core/CommandRunner.h"

using namespace mazeconnect::core;

/**
 * The allow-list, and the ways a peer might try to get around it.
 *
 * The property under test is one sentence: **a phone sends an id, and only an
 * id it was given can run anything.** Everything below is a way of asking
 * whether that sentence is still true — a command-shaped id, an id that is
 * nearly right, an argv assembled at the far end, a file someone else can
 * write.
 *
 * Note what is *not* tested here, because it cannot be: escaping. No shell is
 * ever invoked, so there is no metacharacter to escape and no quoting bug to
 * have. `runsOnlyTheArgvFromTheFile` is what pins that down.
 */
class TestCommandRunner : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void absentFileIsNoCommandsNotAnError();
    void loadsAWellFormedFile();
    void refusesAFileOthersCanRead();
    void refusesAnUnknownId();
    void refusesACommandShapedId();
    void runsOnlyTheArgvFromTheFile();
    void neverInvokesAShell();
    void argumentsAreNeverReSplit();
    void malformedEntriesAreSkippedNotGuessed();
    void duplicateIdsRefuseTheWholeFile();
    void capsOutput();
    void reportsAFailingCommandsExitCode();

    void addCommandWritesAFileThatReadsBack();
    void addCommandRefusesADuplicateId();
    void addCommandRefusesOutOfBoundsInput();
    void addCommandRefusesWhenTheExistingFileIsBroken();
    void updateCommandReplacesTheEntry();
    void updateCommandCanChangeTheId();
    void updateCommandRefusesAnUnknownId();
    void updateCommandRefusesCollidingWithAnotherId();
    void removeCommandDropsOnlyThatEntry();
    void removeCommandRefusesAnUnknownId();
    void writtenFileIsOwnerOnly();

private:
    void writeCommands(const QString &json, QFile::Permissions perms = QFile::ReadOwner | QFile::WriteOwner);

    QTemporaryDir m_dir;
    QString m_path;
};

void TestCommandRunner::init() {
    QVERIFY(m_dir.isValid());
    m_path = QDir(m_dir.path()).filePath(QStringLiteral("commands.json"));
    qputenv("MAZECONNECT_COMMANDS_FILE", m_path.toUtf8());
}

void TestCommandRunner::cleanup() {
    qunsetenv("MAZECONNECT_COMMANDS_FILE");
    QFile::remove(m_path);
}

void TestCommandRunner::writeCommands(const QString &json, QFile::Permissions perms) {
    QFile file(m_path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(json.toUtf8());
    file.close();
    QVERIFY(file.setPermissions(perms));
}

void TestCommandRunner::absentFileIsNoCommandsNotAnError() {
    // Someone who has not set commands up has no commands. That is a normal
    // state to be in, and the phone should say so rather than "failed".
    CommandRunner runner;
    QVERIFY(runner.catalog().isEmpty());
    QVERIFY(runner.lastError().isEmpty());
}

void TestCommandRunner::loadsAWellFormedFile() {
    writeCommands(R"([
        {"id": "lock", "label": "Lock screen", "argv": ["/usr/bin/true"], "confirm": false, "pinned": true},
        {"id": "sleep", "label": "Suspend", "argv": ["/usr/bin/true", "--now"], "confirm": true}
    ])");

    CommandRunner runner;
    const QList<Command> commands = runner.catalog();
    QCOMPARE(commands.size(), 2);
    QCOMPARE(commands.at(0).id, QStringLiteral("lock"));
    QCOMPARE(commands.at(0).label, QStringLiteral("Lock screen"));
    QCOMPARE(commands.at(0).argv, QStringList{QStringLiteral("/usr/bin/true")});
    QVERIFY(!commands.at(0).confirm);
    QVERIFY(commands.at(0).pinned);
    QVERIFY(commands.at(1).confirm);
    QVERIFY2(!commands.at(1).pinned, "an entry with no pinned key must default to false");
    QCOMPARE(commands.at(1).argv.size(), 2);
}

void TestCommandRunner::refusesAFileOthersCanRead() {
    // This file decides what runs as this user. If anyone else can write it,
    // the list is not necessarily the user's own any more.
    writeCommands(R"([{"id": "x", "argv": ["/usr/bin/true"]}])",
                  QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);

    CommandRunner runner;
    QVERIFY2(runner.catalog().isEmpty(), "loaded a command file other users can read");
    QVERIFY(!runner.lastError().isEmpty());
}

void TestCommandRunner::refusesAnUnknownId() {
    writeCommands(R"([{"id": "lock", "argv": ["/usr/bin/true"]}])");

    CommandRunner runner;
    QSignalSpy finished(&runner, &CommandRunner::finished);

    QVERIFY2(runner.run(QStringLiteral("unlock")) == 0, "ran an id that is not in the file");
    QVERIFY2(runner.run(QString()) == 0, "ran an empty id");
    QVERIFY2(runner.run(QStringLiteral("LOCK")) == 0, "ids are matched exactly, not case-folded");
    QVERIFY2(runner.run(QStringLiteral("lock ")) == 0, "a near-miss id must not match");

    QTest::qWait(200);
    QCOMPARE(finished.count(), 0);
}

void TestCommandRunner::refusesACommandShapedId() {
    // The plan's own test: send something that looks like a command line as
    // the id. It is not parsed, split, or interpreted — it is looked up, and
    // it is not there.
    writeCommands(R"([{"id": "lock", "argv": ["/usr/bin/true"]}])");

    CommandRunner runner;
    QSignalSpy finished(&runner, &CommandRunner::finished);

    const QStringList attempts = {
        QStringLiteral("/usr/bin/touch /tmp/maze-connect-should-not-exist"),
        QStringLiteral("lock; touch /tmp/maze-connect-should-not-exist"),
        QStringLiteral("lock && touch /tmp/maze-connect-should-not-exist"),
        QStringLiteral("lock | sh"),
        QStringLiteral("$(touch /tmp/maze-connect-should-not-exist)"),
        QStringLiteral("`touch /tmp/maze-connect-should-not-exist`"),
        QStringLiteral("../../bin/sh"),
    };
    for (const QString &attempt : attempts) {
        QVERIFY2(runner.run(attempt) == 0, qPrintable(QStringLiteral("ran: %1").arg(attempt)));
    }

    QTest::qWait(300);
    QCOMPARE(finished.count(), 0);
    QVERIFY2(!QFile::exists(QStringLiteral("/tmp/maze-connect-should-not-exist")),
             "a command-shaped id reached something that executed it");
}

void TestCommandRunner::runsOnlyTheArgvFromTheFile() {
    const QString marker = QDir(m_dir.path()).filePath(QStringLiteral("ran"));
    writeCommands(QStringLiteral(R"([{"id": "touch", "argv": ["/usr/bin/touch", "%1"]}])")
                      .arg(marker));

    CommandRunner runner;
    QSignalSpy finished(&runner, &CommandRunner::finished);

    const quint32 handle = runner.run(QStringLiteral("touch"));
    QVERIFY(handle != 0);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 5000);

    QCOMPARE(finished.at(0).at(0).toUInt(), handle);
    QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("touch"));
    QCOMPARE(finished.at(0).at(2).toInt(), 0);
    QVERIFY2(QFile::exists(marker), "the argv from the file did not run");
}

void TestCommandRunner::neverInvokesAShell() {
    // The argument is passed through as one string, exactly as written. If a
    // shell were involved anywhere, `;` and `>` would split it and the marker
    // file would appear; QProcess with a program and an argument list hands
    // it to execve() untouched.
    const QString marker = QDir(m_dir.path()).filePath(QStringLiteral("shell-ran"));
    writeCommands(QStringLiteral(R"([{"id": "echo",
        "argv": ["/usr/bin/echo", "hello; touch %1"]}])").arg(marker));

    CommandRunner runner;
    QSignalSpy finished(&runner, &CommandRunner::finished);

    QVERIFY(runner.run(QStringLiteral("echo")) != 0);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 5000);

    QVERIFY2(finished.at(0).at(3).toString().contains(QStringLiteral("hello; touch")),
             "the argument was not passed through verbatim");
    QVERIFY2(!QFile::exists(marker), "a shell interpreted the argument");
}

void TestCommandRunner::malformedEntriesAreSkippedNotGuessed() {
    // One bad row should cost that row, not the file — but a row missing the
    // thing that makes it a command must never be completed with a default.
    writeCommands(R"([
        {"id": "noargv", "label": "no argv"},
        {"id": "emptyargv", "argv": []},
        {"id": "notstrings", "argv": [1, 2]},
        {"argv": ["/usr/bin/true"]},
        {"id": "good", "argv": ["/usr/bin/true"]}
    ])");

    CommandRunner runner;
    const QList<Command> commands = runner.catalog();
    QCOMPARE(commands.size(), 1);
    QCOMPARE(commands.at(0).id, QStringLiteral("good"));
    // A label that was never given falls back to the id rather than to blank.
    QCOMPARE(commands.at(0).label, QStringLiteral("good"));
}

void TestCommandRunner::duplicateIdsRefuseTheWholeFile() {
    // "The entry with this id" has to mean one thing. Picking the first or
    // the last would be a guess about which command the user meant to run.
    writeCommands(R"([
        {"id": "same", "argv": ["/usr/bin/true"]},
        {"id": "same", "argv": ["/usr/bin/false"]}
    ])");

    CommandRunner runner;
    QVERIFY(runner.catalog().isEmpty());
    QVERIFY(runner.lastError().contains(QStringLiteral("duplicate")));
}

void TestCommandRunner::capsOutput() {
    // A command that prints without end must not be able to grow a control
    // frame without bound.
    writeCommands(R"([{"id": "flood", "argv": ["/usr/bin/yes", "flooding-the-output-buffer"]}])");

    CommandRunner runner;
    QSignalSpy finished(&runner, &CommandRunner::finished);

    QVERIFY(runner.run(QStringLiteral("flood")) != 0);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 10000);

    const QString output = finished.at(0).at(3).toString();
    QVERIFY2(output.size() <= CommandRunner::kMaxOutputBytes + 64,
             "output was not capped");
}

void TestCommandRunner::reportsAFailingCommandsExitCode() {
    // A command that ran and failed is not the same as one that never ran,
    // and the phone has to be able to tell them apart.
    writeCommands(R"([{"id": "fail", "argv": ["/usr/bin/false"]}])");

    CommandRunner runner;
    QSignalSpy finished(&runner, &CommandRunner::finished);

    QVERIFY2(runner.run(QStringLiteral("fail")) != 0, "a command that exits non-zero still runs");
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 5000);
    QVERIFY(finished.at(0).at(2).toInt() != 0);
}

void TestCommandRunner::argumentsAreNeverReSplit() {
    // One argv element is one argument, whatever is inside it.
    //
    // The usual way command execution goes wrong is a string that gets parsed
    // twice — once by the author, once by a shell. There is no second parse
    // here: QProcess hands the list to execve(), so spaces, quotes, `$`, `;`
    // and newlines are ordinary characters in one argument.
    const QString marker = QDir(m_dir.path()).filePath(QStringLiteral("split"));
    writeCommands(QStringLiteral(R"([{"id": "one",
        "argv": ["/usr/bin/echo", "a b; touch %1 && echo $HOME `id` \"q\""]}])").arg(marker));

    CommandRunner runner;
    QSignalSpy finished(&runner, &CommandRunner::finished);
    QVERIFY(runner.run(QStringLiteral("one")) != 0);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 5000);

    const QString output = finished.at(0).at(3).toString().trimmed();
    // echo printed exactly the one argument it was given — unexpanded,
    // unsplit, with the metacharacters intact.
    QCOMPARE(output, QStringLiteral("a b; touch %1 && echo $HOME `id` \"q\"").arg(marker));
    QVERIFY2(!QFile::exists(marker), "part of an argument was executed");
    QVERIFY2(!output.contains(QStringLiteral("/home/")),
             "a variable inside an argument was expanded");
}

void TestCommandRunner::addCommandWritesAFileThatReadsBack() {
    CommandRunner runner;
    QVERIFY(runner.catalog().isEmpty());

    Command command;
    command.id = QStringLiteral("lock");
    command.label = QStringLiteral("Lock screen");
    command.argv = {QStringLiteral("loginctl"), QStringLiteral("lock-session")};
    command.confirm = false;
    command.pinned = true;
    QVERIFY(runner.addCommand(command));

    // A second instance, so this proves the file on disk rather than
    // whatever the first runner happens to have cached in memory.
    CommandRunner reader;
    const QList<Command> commands = reader.catalog();
    QCOMPARE(commands.size(), 1);
    QCOMPARE(commands.at(0).id, QStringLiteral("lock"));
    QCOMPARE(commands.at(0).label, QStringLiteral("Lock screen"));
    QCOMPARE(commands.at(0).argv, command.argv);
    QVERIFY(!commands.at(0).confirm);
    QVERIFY2(commands.at(0).pinned, "pinned did not round-trip through addCommand()");
}

void TestCommandRunner::addCommandRefusesADuplicateId() {
    writeCommands(R"([{"id": "lock", "argv": ["/usr/bin/true"]}])");

    CommandRunner runner;
    Command command;
    command.id = QStringLiteral("lock");
    command.argv = {QStringLiteral("/usr/bin/false")};
    QVERIFY2(!runner.addCommand(command), "added a second command under the same id");
    QVERIFY(runner.lastError().contains(QStringLiteral("already exists")));

    // The original entry must survive an add that was refused.
    QCOMPARE(runner.catalog().size(), 1);
    QCOMPARE(runner.catalog().at(0).argv, QStringList{QStringLiteral("/usr/bin/true")});
}

void TestCommandRunner::addCommandRefusesOutOfBoundsInput() {
    CommandRunner runner;

    Command noId;
    noId.argv = {QStringLiteral("/usr/bin/true")};
    QVERIFY2(!runner.addCommand(noId), "added a command with no id");

    Command noArgv;
    noArgv.id = QStringLiteral("x");
    QVERIFY2(!runner.addCommand(noArgv), "added a command with no argv");

    Command controlChar;
    controlChar.id = QStringLiteral("bad\nid");
    controlChar.argv = {QStringLiteral("/usr/bin/true")};
    QVERIFY2(!runner.addCommand(controlChar), "added an id with a control character");

    Command tooManyArgs;
    tooManyArgs.id = QStringLiteral("many");
    for (int i = 0; i < CommandRunner::kMaxArgs + 1; ++i) {
        tooManyArgs.argv << QStringLiteral("arg");
    }
    QVERIFY2(!runner.addCommand(tooManyArgs), "added a command over the argument-count bound");

    QVERIFY2(runner.catalog().isEmpty(), "an out-of-bounds add still wrote something");
}

void TestCommandRunner::addCommandRefusesWhenTheExistingFileIsBroken() {
    // Others-readable is exactly what catalog() itself refuses to load — see
    // refusesAFileOthersCanRead(). Adding on top of a file already in this
    // state would silently replace something the user may still want to
    // recover by hand.
    writeCommands(R"([{"id": "x", "argv": ["/usr/bin/true"]}])",
                  QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther);

    CommandRunner runner;
    Command command;
    command.id = QStringLiteral("y");
    command.argv = {QStringLiteral("/usr/bin/true")};
    QVERIFY2(!runner.addCommand(command), "wrote over a file that failed to load");
}

void TestCommandRunner::updateCommandReplacesTheEntry() {
    writeCommands(R"([
        {"id": "lock", "label": "Lock", "argv": ["/usr/bin/true"], "confirm": false},
        {"id": "sleep", "argv": ["/usr/bin/true"]}
    ])");

    CommandRunner runner;
    Command updated;
    updated.id = QStringLiteral("lock");
    updated.label = QStringLiteral("Lock screen");
    updated.argv = {QStringLiteral("loginctl"), QStringLiteral("lock-session")};
    updated.confirm = true;
    QVERIFY(runner.updateCommand(QStringLiteral("lock"), updated));

    const QList<Command> commands = runner.catalog();
    QCOMPARE(commands.size(), 2);
    // Position preserved, not moved to the end.
    QCOMPARE(commands.at(0).id, QStringLiteral("lock"));
    QCOMPARE(commands.at(0).label, QStringLiteral("Lock screen"));
    QCOMPARE(commands.at(0).argv, updated.argv);
    QVERIFY(commands.at(0).confirm);
    QCOMPARE(commands.at(1).id, QStringLiteral("sleep"));
}

void TestCommandRunner::updateCommandCanChangeTheId() {
    writeCommands(R"([{"id": "old", "argv": ["/usr/bin/true"]}])");

    CommandRunner runner;
    Command updated;
    updated.id = QStringLiteral("new");
    updated.argv = {QStringLiteral("/usr/bin/true")};
    QVERIFY(runner.updateCommand(QStringLiteral("old"), updated));

    const QList<Command> commands = runner.catalog();
    QCOMPARE(commands.size(), 1);
    QCOMPARE(commands.at(0).id, QStringLiteral("new"));
}

void TestCommandRunner::updateCommandRefusesAnUnknownId() {
    CommandRunner runner;
    Command updated;
    updated.id = QStringLiteral("x");
    updated.argv = {QStringLiteral("/usr/bin/true")};
    QVERIFY2(!runner.updateCommand(QStringLiteral("missing"), updated),
             "updated an id that was never there");
}

void TestCommandRunner::updateCommandRefusesCollidingWithAnotherId() {
    writeCommands(R"([
        {"id": "a", "argv": ["/usr/bin/true"]},
        {"id": "b", "argv": ["/usr/bin/true"]}
    ])");

    CommandRunner runner;
    Command updated;
    updated.id = QStringLiteral("b"); // already taken by the other entry
    updated.argv = {QStringLiteral("/usr/bin/false")};
    QVERIFY2(!runner.updateCommand(QStringLiteral("a"), updated),
             "renamed one entry onto another entry's id");

    // Untouched: 'a' still has its original argv.
    const QList<Command> commands = runner.catalog();
    QCOMPARE(commands.at(0).id, QStringLiteral("a"));
    QCOMPARE(commands.at(0).argv, QStringList{QStringLiteral("/usr/bin/true")});
}

void TestCommandRunner::removeCommandDropsOnlyThatEntry() {
    writeCommands(R"([
        {"id": "a", "argv": ["/usr/bin/true"]},
        {"id": "b", "argv": ["/usr/bin/true"]}
    ])");

    CommandRunner runner;
    QVERIFY(runner.removeCommand(QStringLiteral("a")));

    const QList<Command> commands = runner.catalog();
    QCOMPARE(commands.size(), 1);
    QCOMPARE(commands.at(0).id, QStringLiteral("b"));
}

void TestCommandRunner::removeCommandRefusesAnUnknownId() {
    writeCommands(R"([{"id": "a", "argv": ["/usr/bin/true"]}])");

    CommandRunner runner;
    QVERIFY2(!runner.removeCommand(QStringLiteral("missing")), "removed an id that was never there");
    QCOMPARE(runner.catalog().size(), 1);
}

void TestCommandRunner::writtenFileIsOwnerOnly() {
    CommandRunner runner;
    Command command;
    command.id = QStringLiteral("x");
    command.argv = {QStringLiteral("/usr/bin/true")};
    QVERIFY(runner.addCommand(command));

    const QFile::Permissions perms = QFileInfo(m_path).permissions();
    const QFile::Permissions othersCanTouch =
        QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
        QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    QVERIFY2(!(perms & othersCanTouch), "a written command file was not owner-only");
}

QTEST_MAIN(TestCommandRunner)
#include "tst_commandrunner.moc"
