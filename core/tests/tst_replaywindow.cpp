#include <QtTest>

#include "mazeconnect/core/ReplayWindow.h"

using namespace mazeconnect::core;

class TestReplayWindow : public QObject {
    Q_OBJECT

private slots:
    void acceptsMonotonicSequence();
    void rejectsZero();
    void rejectsExactReplay();
    void acceptsOutOfOrderWithinWindow();
    void rejectsReplayOfOutOfOrderMessage();
    void rejectsTooOld();
    void handlesLargeJump();
    void resetClearsState();
};

void TestReplayWindow::acceptsMonotonicSequence() {
    ReplayWindow w;
    for (quint64 i = 1; i <= 10000; ++i) {
        QVERIFY2(w.accept(i), qPrintable(QStringLiteral("rejected counter %1").arg(i)));
    }
    QCOMPARE(w.highest(), quint64(10000));
}

void TestReplayWindow::rejectsZero() {
    ReplayWindow w;
    // 0 must never be valid, so a zeroed or truncated counter field is not
    // mistaken for a legitimate first message.
    QVERIFY(!w.accept(0));
    QCOMPARE(w.highest(), quint64(0));
}

void TestReplayWindow::rejectsExactReplay() {
    ReplayWindow w;
    QVERIFY(w.accept(1));
    QVERIFY(!w.accept(1));

    QVERIFY(w.accept(2));
    QVERIFY(!w.accept(2));
    QVERIFY(!w.accept(1));
}

void TestReplayWindow::acceptsOutOfOrderWithinWindow() {
    ReplayWindow w;
    QVERIFY(w.accept(100));
    // Network reordering inside the window is legitimate.
    QVERIFY(w.accept(98));
    QVERIFY(w.accept(99));
    QVERIFY(w.accept(50));
    QCOMPARE(w.highest(), quint64(100));
}

void TestReplayWindow::rejectsReplayOfOutOfOrderMessage() {
    ReplayWindow w;
    QVERIFY(w.accept(100));
    QVERIFY(w.accept(98));
    // Re-sending the same late message is still a replay.
    QVERIFY(!w.accept(98));
}

void TestReplayWindow::rejectsTooOld() {
    ReplayWindow w;
    QVERIFY(w.accept(ReplayWindow::kWindowSize + 100));
    // Far enough behind that we can no longer prove it isn't a replay.
    QVERIFY(!w.accept(1));
    QVERIFY(!w.accept(50));
}

void TestReplayWindow::handlesLargeJump() {
    ReplayWindow w;
    QVERIFY(w.accept(1));
    // A jump past the window resets the history; the new counter is fine,
    // but the old one must not become acceptable again.
    QVERIFY(w.accept(1000000));
    QCOMPARE(w.highest(), quint64(1000000));
    QVERIFY(!w.accept(1));
    QVERIFY(!w.accept(1000000));
    QVERIFY(w.accept(999999));
}

void TestReplayWindow::resetClearsState() {
    ReplayWindow w;
    QVERIFY(w.accept(5));
    w.reset();
    QCOMPARE(w.highest(), quint64(0));
    QVERIFY(w.accept(5)); // fresh session, same counter is fine again
}

QTEST_MAIN(TestReplayWindow)
#include "tst_replaywindow.moc"
