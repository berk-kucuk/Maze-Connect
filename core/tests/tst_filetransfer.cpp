#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "mazeconnect/core/FileTransfer.h"
#include "mazeconnect/core/Limits.h"

using namespace mazeconnect::core;

class TestFileTransfer : public QObject {
    Q_OBJECT

private slots:
    void receivesCompleteFile();
    void rejectsTraversalFilename();
    void rejectsOversizedDeclaration();
    void cutsOffPeerThatOverruns();
    void rejectsSizeMismatch();
    void removesPartialFileOnCancel();
    void doesNotOverwriteExistingFile();
    void refusesDuplicateTransferId();
};

void TestFileTransfer::receivesCompleteFile() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    FileTransferReceiver receiver(tmp.filePath(QStringLiteral("inbox")));

    const QByteArray payload = QByteArrayLiteral("hello maze connect");
    QString reason;
    QVERIFY2(receiver.begin(1, QStringLiteral("note.txt"), payload.size(), reason),
             qPrintable(reason));
    QVERIFY2(receiver.appendChunk(1, payload, reason), qPrintable(reason));

    QString finalPath;
    QVERIFY2(receiver.finish(1, finalPath, reason), qPrintable(reason));

    QFile written(finalPath);
    QVERIFY(written.open(QIODevice::ReadOnly));
    QCOMPARE(written.readAll(), payload);
    QCOMPARE(QFileInfo(finalPath).fileName(), QStringLiteral("note.txt"));
    QCOMPARE(receiver.activeCount(), 0);
}

void TestFileTransfer::rejectsTraversalFilename() {
    QTemporaryDir tmp;
    FileTransferReceiver receiver(tmp.filePath(QStringLiteral("inbox")));

    QString reason;
    QVERIFY(!receiver.begin(1, QStringLiteral("../../escaped.txt"), 10, reason));
    QVERIFY(!receiver.begin(2, QStringLiteral("/etc/passwd"), 10, reason));
    QVERIFY(!receiver.begin(3, QStringLiteral(".."), 10, reason));
    QCOMPARE(receiver.activeCount(), 0);

    // And nothing was created outside the inbox.
    QVERIFY(!QFile::exists(tmp.filePath(QStringLiteral("escaped.txt"))));
}

void TestFileTransfer::rejectsOversizedDeclaration() {
    QTemporaryDir tmp;
    FileTransferReceiver receiver(tmp.filePath(QStringLiteral("inbox")));

    QString reason;
    QVERIFY(!receiver.begin(1, QStringLiteral("huge.bin"), limits::kMaxFileBytes + 1, reason));
    QVERIFY(!receiver.begin(2, QStringLiteral("negative.bin"), -1, reason));
}

void TestFileTransfer::cutsOffPeerThatOverruns() {
    QTemporaryDir tmp;
    FileTransferReceiver receiver(tmp.filePath(QStringLiteral("inbox")));

    QString reason;
    QVERIFY(receiver.begin(1, QStringLiteral("small.bin"), 10, reason));

    // Peer declared 10 bytes but sends 100 — the receiver must not simply
    // keep writing whatever arrives.
    QVERIFY2(!receiver.appendChunk(1, QByteArray(100, 'x'), reason),
             "receiver accepted more data than the peer declared");
    QCOMPARE(receiver.activeCount(), 0); // transfer torn down
}

void TestFileTransfer::rejectsSizeMismatch() {
    QTemporaryDir tmp;
    FileTransferReceiver receiver(tmp.filePath(QStringLiteral("inbox")));

    QString reason;
    QVERIFY(receiver.begin(1, QStringLiteral("truncated.bin"), 100, reason));
    QVERIFY(receiver.appendChunk(1, QByteArray(50, 'x'), reason));

    // Finishing early must fail rather than deliver a short file.
    QString finalPath;
    QVERIFY(!receiver.finish(1, finalPath, reason));
    QCOMPARE(receiver.activeCount(), 0);

    // No partial file left behind under any name.
    const QDir inbox(receiver.inboxRoot());
    QCOMPARE(inbox.entryList(QDir::Files | QDir::Hidden).size(), 0);
}

void TestFileTransfer::removesPartialFileOnCancel() {
    QTemporaryDir tmp;
    FileTransferReceiver receiver(tmp.filePath(QStringLiteral("inbox")));

    QString reason;
    QVERIFY(receiver.begin(1, QStringLiteral("partial.bin"), 1000, reason));
    QVERIFY(receiver.appendChunk(1, QByteArray(100, 'x'), reason));

    receiver.cancel(1);

    const QDir inbox(receiver.inboxRoot());
    QCOMPARE(inbox.entryList(QDir::Files | QDir::Hidden).size(), 0);
    QCOMPARE(receiver.activeCount(), 0);
}

void TestFileTransfer::doesNotOverwriteExistingFile() {
    QTemporaryDir tmp;
    FileTransferReceiver receiver(tmp.filePath(QStringLiteral("inbox")));

    QString reason;
    QString firstPath;
    QVERIFY(receiver.begin(1, QStringLiteral("dup.txt"), 5, reason));
    QVERIFY(receiver.appendChunk(1, QByteArrayLiteral("first"), reason));
    QVERIFY(receiver.finish(1, firstPath, reason));

    QString secondPath;
    QVERIFY(receiver.begin(2, QStringLiteral("dup.txt"), 6, reason));
    QVERIFY(receiver.appendChunk(2, QByteArrayLiteral("second"), reason));
    QVERIFY(receiver.finish(2, secondPath, reason));

    QVERIFY(firstPath != secondPath);

    // The original content must survive: a peer cannot replace a file by
    // resending the same name.
    QFile first(firstPath);
    QVERIFY(first.open(QIODevice::ReadOnly));
    QCOMPARE(first.readAll(), QByteArrayLiteral("first"));
}

void TestFileTransfer::refusesDuplicateTransferId() {
    QTemporaryDir tmp;
    FileTransferReceiver receiver(tmp.filePath(QStringLiteral("inbox")));

    QString reason;
    QVERIFY(receiver.begin(1, QStringLiteral("a.txt"), 10, reason));
    QVERIFY(!receiver.begin(1, QStringLiteral("b.txt"), 10, reason));
}

QTEST_MAIN(TestFileTransfer)
#include "tst_filetransfer.moc"
