#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "mazeconnect/core/PathSanitizer.h"

using namespace mazeconnect::core;

class TestPathSanitizer : public QObject {
    Q_OBJECT

private slots:
    void acceptsOrdinaryNames();
    void rejectsTraversal_data();
    void rejectsTraversal();
    void rejectsControlCharacters();
    void rejectsOverlongNames();
    void rejectsReservedDeviceNames();
    void confinementRejectsSymlinkEscape();
    void confinementRejectsSiblingPrefix();
    void uniqueNameAvoidsOverwrite();
};

void TestPathSanitizer::acceptsOrdinaryNames() {
    QCOMPARE(PathSanitizer::sanitizeFilename(QStringLiteral("report.pdf")),
             QStringLiteral("report.pdf"));
    QCOMPARE(PathSanitizer::sanitizeFilename(QStringLiteral("holiday photo.jpg")),
             QStringLiteral("holiday photo.jpg"));
    // Non-ASCII must survive — this is a Turkish-language project.
    QCOMPARE(PathSanitizer::sanitizeFilename(QStringLiteral("çalışma raporu.odt")),
             QStringLiteral("çalışma raporu.odt"));
}

void TestPathSanitizer::rejectsTraversal_data() {
    QTest::addColumn<QString>("name");

    QTest::newRow("dotdot") << QStringLiteral("..");
    QTest::newRow("dot") << QStringLiteral(".");
    QTest::newRow("parent-escape") << QStringLiteral("../../etc/passwd");
    QTest::newRow("absolute") << QStringLiteral("/etc/passwd");
    QTest::newRow("nested") << QStringLiteral("sub/dir/file.txt");
    QTest::newRow("backslash") << QStringLiteral("..\\..\\windows\\system32");
    QTest::newRow("home") << QStringLiteral("~/.ssh/authorized_keys");
    QTest::newRow("only-dots") << QStringLiteral("...");
    QTest::newRow("leading-dash") << QStringLiteral("-rf");
    QTest::newRow("empty") << QString();
    QTest::newRow("whitespace") << QStringLiteral("   ");
    QTest::newRow("trailing-dot") << QStringLiteral("file.");
}

void TestPathSanitizer::rejectsTraversal() {
    QFETCH(QString, name);
    QVERIFY2(PathSanitizer::sanitizeFilename(name).isEmpty(),
             qPrintable(QStringLiteral("accepted hostile name: %1").arg(name)));
}

void TestPathSanitizer::rejectsControlCharacters() {
    QVERIFY(PathSanitizer::sanitizeFilename(QStringLiteral("a\0b.txt")).isEmpty());
    QVERIFY(PathSanitizer::sanitizeFilename(QStringLiteral("a\nb.txt")).isEmpty());
    QVERIFY(PathSanitizer::sanitizeFilename(QStringLiteral("a\tb.txt")).isEmpty());
    QVERIFY(PathSanitizer::sanitizeFilename(QStringLiteral("bell\x07.txt")).isEmpty());
}

void TestPathSanitizer::rejectsOverlongNames() {
    QVERIFY(PathSanitizer::sanitizeFilename(QString(4096, u'a')).isEmpty());
}

void TestPathSanitizer::rejectsReservedDeviceNames() {
    QVERIFY(PathSanitizer::sanitizeFilename(QStringLiteral("CON")).isEmpty());
    QVERIFY(PathSanitizer::sanitizeFilename(QStringLiteral("con.txt")).isEmpty());
    QVERIFY(PathSanitizer::sanitizeFilename(QStringLiteral("LPT1.dat")).isEmpty());
    // ...but a name that merely starts with those letters is fine.
    QCOMPARE(PathSanitizer::sanitizeFilename(QStringLiteral("connection.log")),
             QStringLiteral("connection.log"));
}

void TestPathSanitizer::confinementRejectsSymlinkEscape() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString root = tmp.filePath(QStringLiteral("inbox"));
    const QString outside = tmp.filePath(QStringLiteral("outside"));
    QVERIFY(QDir().mkpath(root));
    QVERIFY(QDir().mkpath(outside));

    // A real file outside the inbox, and a symlink inside the inbox that
    // points at it — the exact shape of an escape a name filter cannot see.
    const QString secret = outside + QStringLiteral("/secret.txt");
    QFile f(secret);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("sensitive");
    f.close();

    const QString link = root + QStringLiteral("/innocent.txt");
    QVERIFY(QFile::link(secret, link));

    QVERIFY2(!PathSanitizer::isWithinRoot(link, root),
             "symlink pointing outside the inbox was treated as confined");

    // A genuine file inside the inbox passes.
    const QString inside = root + QStringLiteral("/real.txt");
    QFile g(inside);
    QVERIFY(g.open(QIODevice::WriteOnly));
    g.write("ok");
    g.close();
    QVERIFY(PathSanitizer::isWithinRoot(inside, root));
}

void TestPathSanitizer::confinementRejectsSiblingPrefix() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString root = tmp.filePath(QStringLiteral("inbox"));
    const QString sibling = tmp.filePath(QStringLiteral("inbox-evil"));
    QVERIFY(QDir().mkpath(root));
    QVERIFY(QDir().mkpath(sibling));

    const QString file = sibling + QStringLiteral("/f.txt");
    QFile f(file);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    // "/tmp/.../inbox-evil/f.txt" starts with "/tmp/.../inbox" as a string
    // but is not inside it.
    QVERIFY(!PathSanitizer::isWithinRoot(file, root));
}

void TestPathSanitizer::uniqueNameAvoidsOverwrite() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    QCOMPARE(PathSanitizer::uniqueNameIn(tmp.path(), QStringLiteral("a.txt")),
             QStringLiteral("a.txt"));

    QFile f(tmp.filePath(QStringLiteral("a.txt")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.close();

    QCOMPARE(PathSanitizer::uniqueNameIn(tmp.path(), QStringLiteral("a.txt")),
             QStringLiteral("a (2).txt"));
}

QTEST_MAIN(TestPathSanitizer)
#include "tst_pathsanitizer.moc"
