#include <QtTest>

#include "mazeconnect/core/Version.h"

class TestVersion : public QObject {
    Q_OBJECT

private slots:
    void protocolVersionMatchesConstant();
};

void TestVersion::protocolVersionMatchesConstant() {
    QCOMPARE(mazeconnect::core::protocolVersion(), mazeconnect::core::kProtocolVersion);
}

QTEST_MAIN(TestVersion)
#include "tst_version.moc"
