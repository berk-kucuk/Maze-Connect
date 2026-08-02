#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "mazeconnect/core/StatusProvider.h"

using namespace mazeconnect::core;

/**
 * StatusProvider against stand-in helpers.
 *
 * The real helper needs maze-tools installed, which a build machine may not
 * have — and the interesting cases are the ones the real helper does not
 * produce anyway. What matters is that every way of *not* getting a snapshot
 * ends as a reported failure rather than as an empty snapshot: a dashboard
 * showing a machine with no security services running, because a probe
 * crashed, would be worse than one saying it could not read the machine.
 */
class TestStatusProvider : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void reportsSnapshotFromHelper();
    void missingHelperIsAFailureNotAnEmptySnapshot();
    void nonZeroExitSurfacesTheReason();
    void malformedOutputIsRejected();
    void nonObjectJsonIsRejected();
    void repeatedRequestsAreServedFromCache();
    void concurrentRequestsRunTheHelperOnce();
    void oversizedOutputIsRefused();

private:
    /// Writes an executable stand-in helper and points the provider at it.
    void installHelper(const QString &script);
    QString countFilePath() const;
    int helperRunCount() const;

    QTemporaryDir m_dir;
    QString m_helper;
};

void TestStatusProvider::init() {
    QVERIFY(m_dir.isValid());
    m_helper = QDir(m_dir.path()).filePath(QStringLiteral("helper.sh"));
}

void TestStatusProvider::cleanup() {
    qunsetenv("MAZECONNECT_STATUS_HELPER");
    QFile::remove(m_helper);
    QFile::remove(countFilePath());
}

QString TestStatusProvider::countFilePath() const {
    return QDir(m_dir.path()).filePath(QStringLiteral("runs"));
}

int TestStatusProvider::helperRunCount() const {
    QFile file(countFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    return static_cast<int>(file.readAll().size());
}

void TestStatusProvider::installHelper(const QString &script) {
    QFile file(m_helper);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(script.toUtf8());
    file.close();
    QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    qputenv("MAZECONNECT_STATUS_HELPER", m_helper.toUtf8());
}

void TestStatusProvider::reportsSnapshotFromHelper() {
    installHelper(QStringLiteral("#!/bin/sh\nprintf '%s' '{\"hostname\":\"testbox\",\"unavailable\":[]}'\n"));

    StatusProvider provider;
    QVERIFY(provider.isAvailable());

    QSignalSpy ready(&provider, &StatusProvider::snapshotReady);
    QSignalSpy failed(&provider, &StatusProvider::snapshotFailed);
    provider.request();

    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1, 5000);
    QCOMPARE(failed.count(), 0);

    const auto snapshot = qvariant_cast<QJsonObject>(ready.at(0).at(0));
    QCOMPARE(snapshot.value(QLatin1StringView("hostname")).toString(), QStringLiteral("testbox"));
    QCOMPARE(provider.lastSnapshot(), snapshot);
}

void TestStatusProvider::missingHelperIsAFailureNotAnEmptySnapshot() {
    // maze-tools not installed: the helper file is simply absent.
    qputenv("MAZECONNECT_STATUS_HELPER",
            QDir(m_dir.path()).filePath(QStringLiteral("nope")).toUtf8());

    StatusProvider provider;
    QVERIFY(!provider.isAvailable());

    QSignalSpy ready(&provider, &StatusProvider::snapshotReady);
    QSignalSpy failed(&provider, &StatusProvider::snapshotFailed);
    provider.request();

    // Synchronous: there is nothing to wait for.
    QCOMPARE(failed.count(), 1);
    QCOMPARE(ready.count(), 0);
    QVERIFY(provider.lastSnapshot().isEmpty());
}

void TestStatusProvider::nonZeroExitSurfacesTheReason() {
    installHelper(QStringLiteral("#!/bin/sh\necho 'maze_status is not available' >&2\nexit 3\n"));

    StatusProvider provider;
    QSignalSpy ready(&provider, &StatusProvider::snapshotReady);
    QSignalSpy failed(&provider, &StatusProvider::snapshotFailed);
    provider.request();

    QTRY_VERIFY_WITH_TIMEOUT(failed.count() == 1, 5000);
    QCOMPARE(ready.count(), 0);
    QVERIFY2(failed.at(0).at(0).toString().contains(QStringLiteral("maze_status")),
             "the helper's own reason should reach the user");
}

void TestStatusProvider::malformedOutputIsRejected() {
    installHelper(QStringLiteral("#!/bin/sh\nprintf '%s' '{not json'\n"));

    StatusProvider provider;
    QSignalSpy ready(&provider, &StatusProvider::snapshotReady);
    QSignalSpy failed(&provider, &StatusProvider::snapshotFailed);
    provider.request();

    QTRY_VERIFY_WITH_TIMEOUT(failed.count() == 1, 5000);
    QCOMPARE(ready.count(), 0);
}

void TestStatusProvider::nonObjectJsonIsRejected() {
    // Valid JSON, wrong shape. The report puts this straight on the wire, so
    // an array here would become a message body that is not an object.
    installHelper(QStringLiteral("#!/bin/sh\nprintf '%s' '[1,2,3]'\n"));

    StatusProvider provider;
    QSignalSpy ready(&provider, &StatusProvider::snapshotReady);
    QSignalSpy failed(&provider, &StatusProvider::snapshotFailed);
    provider.request();

    QTRY_VERIFY_WITH_TIMEOUT(failed.count() == 1, 5000);
    QCOMPARE(ready.count(), 0);
}

void TestStatusProvider::repeatedRequestsAreServedFromCache() {
    installHelper(QStringLiteral("#!/bin/sh\nprintf 'x' >> %1\nprintf '%s' '{\"n\":1}'\n")
                      .arg(countFilePath()));

    StatusProvider provider;
    QSignalSpy ready(&provider, &StatusProvider::snapshotReady);

    provider.request();
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1, 5000);
    QCOMPARE(helperRunCount(), 1);

    // Well inside kCacheMs. A phone polling the dashboard must not fork a
    // Python process per poll.
    provider.request();
    provider.request();
    QCOMPARE(ready.count(), 3);
    QCOMPARE(helperRunCount(), 1);
}

void TestStatusProvider::concurrentRequestsRunTheHelperOnce() {
    // Slow enough that the second and third requests land while it runs.
    installHelper(QStringLiteral("#!/bin/sh\nprintf 'x' >> %1\nsleep 0.4\nprintf '%s' '{\"n\":1}'\n")
                      .arg(countFilePath()));

    StatusProvider provider;
    QSignalSpy ready(&provider, &StatusProvider::snapshotReady);

    provider.request();
    provider.request();
    provider.request();

    QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 5000);
    QTest::qWait(200);

    // One run, and exactly one answer: the callers that arrived mid-run are
    // coalesced onto it rather than queued behind their own runs.
    QCOMPARE(helperRunCount(), 1);
    QCOMPARE(ready.count(), 1);
}

void TestStatusProvider::oversizedOutputIsRefused() {
    // A snapshot is ~2 KB. Something producing megabytes at the helper's path
    // is not our helper, and must not be buffered without limit — nor become
    // a control frame too large to send.
    installHelper(QStringLiteral("#!/bin/sh\nexec yes '{\"flood\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}'\n"));

    StatusProvider provider;
    QSignalSpy ready(&provider, &StatusProvider::snapshotReady);
    QSignalSpy failed(&provider, &StatusProvider::snapshotFailed);
    provider.request();

    QTRY_VERIFY_WITH_TIMEOUT(failed.count() == 1, 10000);
    QCOMPARE(ready.count(), 0);
}

QTEST_MAIN(TestStatusProvider)
#include "tst_statusprovider.moc"
