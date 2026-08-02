#include <QCoreApplication>
#include <QLoggingCategory>

#include "mazeconnect/core/Version.h"

// Placeholder entry point. The daemon owns the long-term identity key and
// the mTLS listen socket once pairing/transport lands (see core/src/pairing,
// core/src/transport) — until then this just proves the process boots under
// its intended systemd --user unit.
int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("mazeconnectd");
    QCoreApplication::setApplicationVersion(QString::number(mazeconnect::core::protocolVersion()));

    qInfo("mazeconnectd starting (protocol v%d)", mazeconnect::core::protocolVersion());

    return 0;
}
