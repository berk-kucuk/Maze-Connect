#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "mazeconnect/core/RemoteInput.h"
#include "mazeconnect/core/SharedFolder.h"
#include "mazeconnect/core/Thumbnailer.h"

#include <QImage>

using namespace mazeconnect::core;

/**
 * The two new ways a phone reaches into this computer, tested for what they
 * refuse: the shared folder against every way out of it, and remote input
 * against everything presenter mode must not do, text that must not be typed,
 * floods, and a second device.
 */
class TestRemoteControl : public QObject {
    Q_OBJECT

private slots:
    void sharedFolderListsOnlyWhatIsInside();
    void sharedFolderRefusesEveryWayOut();
    void sharedFolderDoesNotFollowLinks();
    void fetchGivesOnlyRegularFiles();

    void presenterCanOnlyPressSlideKeys();
    void fullModeBoundsEverything();
    void textIsAllOrNothing();
    void oneDeviceAtATime();
    void floodIsCut();

    void previewsAreSmallJpegs();
    void previewsRefuseWhatIsNotAPicture();
};

namespace {

/// Records what would have been injected.
class FakeBackend : public InputBackend {
public:
    struct Call {
        QString what;
        double a = 0;
        double b = 0;
    };
    QList<Call> calls;
    bool active = false;

    void start() override {
        active = true;
        emit started(true, QString());
    }
    void stop() override { active = false; }
    bool isActive() const override { return active; }
    void pointerMotion(double dx, double dy) override { calls.append({"move", dx, dy}); }
    void pointerButton(int button, bool pressed) override {
        calls.append({"button", double(button), double(pressed)});
    }
    void pointerScroll(int axis, int steps) override {
        calls.append({"scroll", double(axis), double(steps)});
    }
    void keysym(int keysym, bool pressed) override {
        calls.append({"key", double(keysym), double(pressed)});
    }
};

QVariantMap key(const QString &name, const QString &action = QStringLiteral("tap"),
                const QStringList &mods = {}) {
    return {{"kind", "key"}, {"key", name}, {"action", action}, {"mods", mods}};
}

void writeFile(const QString &path, const QByteArray &data = "x") {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

} // namespace

// ---- Shared folder ------------------------------------------------------------

void TestRemoteControl::sharedFolderListsOnlyWhatIsInside() {
    QTemporaryDir dir;
    const QString root = dir.filePath("shared");
    SharedFolder folder(root);
    QVERIFY(folder.ensureExists());
    QDir().mkpath(root + "/Photos");
    writeFile(root + "/notes.txt");
    writeFile(root + "/.hidden");
    writeFile(root + "/Photos/a.jpg");

    QString error;
    const QJsonArray top = folder.list(QString(), error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(top.size(), 2); // Photos, notes.txt — never .hidden
    QCOMPARE(top.at(0).toObject().value("name").toString(), QStringLiteral("Photos"));
    QVERIFY(top.at(0).toObject().value("dir").toBool());
    QCOMPARE(top.at(1).toObject().value("name").toString(), QStringLiteral("notes.txt"));

    const QJsonArray photos = folder.list(QStringLiteral("Photos"), error);
    QCOMPARE(photos.size(), 1);
}

void TestRemoteControl::sharedFolderRefusesEveryWayOut() {
    QTemporaryDir dir;
    const QString root = dir.filePath("shared");
    SharedFolder folder(root);
    QVERIFY(folder.ensureExists());
    QDir().mkpath(root + "/a");
    writeFile(dir.filePath("secret.txt"));

    const QStringList attempts = {
        "..", "../secret.txt", "a/../../secret.txt", "a/..", "./a", "a/./b",
        "/etc/passwd", "a//b", "a/", "\\etc", "a\\..\\..", ".ssh", "a/.git",
        QString("a") + QChar(0) + "b", QString("a\nb"), QString(40, 'x').replace(0, 40, "a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a"),
        QString(2000, 'a'),
    };
    for (const QString &attempt : attempts) {
        QString error;
        QVERIFY2(folder.resolve(attempt, error).isEmpty(), qPrintable("accepted: " + attempt));
        QVERIFY2(!error.isEmpty(), qPrintable("no reason for: " + attempt));
    }
    // 33 levels is past kMaxDepth even if every one existed.
    QString deep;
    for (int i = 0; i < SharedFolder::kMaxDepth + 1; ++i) {
        deep += (i ? "/" : "") + QStringLiteral("d");
    }
    QString error;
    QVERIFY(folder.resolve(deep, error).isEmpty());
}

void TestRemoteControl::sharedFolderDoesNotFollowLinks() {
    QTemporaryDir dir;
    const QString root = dir.filePath("shared");
    SharedFolder folder(root);
    QVERIFY(folder.ensureExists());
    QDir().mkpath(dir.filePath("outside"));
    writeFile(dir.filePath("outside/secret.txt"), "secret");

    // A link to a folder outside, and a link to a file outside.
    QVERIFY(QFile::link(dir.filePath("outside"), root + "/escape"));
    QVERIFY(QFile::link(dir.filePath("outside/secret.txt"), root + "/secret.txt"));

    QString error;
    QVERIFY(folder.resolve(QStringLiteral("escape"), error).isEmpty());
    QVERIFY(folder.resolve(QStringLiteral("escape/secret.txt"), error).isEmpty());
    QVERIFY(folder.fileForFetch(QStringLiteral("secret.txt"), error).isEmpty());
    // And neither is even listed.
    const QJsonArray entries = folder.list(QString(), error);
    QCOMPARE(entries.size(), 0);

    // A shared folder that is itself a link is refused as a whole.
    QVERIFY(QFile::link(dir.filePath("outside"), dir.filePath("linked-root")));
    SharedFolder linked(dir.filePath("linked-root"));
    QVERIFY(linked.list(QString(), error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestRemoteControl::fetchGivesOnlyRegularFiles() {
    QTemporaryDir dir;
    const QString root = dir.filePath("shared");
    SharedFolder folder(root);
    QVERIFY(folder.ensureExists());
    QDir().mkpath(root + "/Docs");
    writeFile(root + "/Docs/cv.pdf");

    QString error;
    QCOMPARE(folder.fileForFetch(QStringLiteral("Docs/cv.pdf"), error),
             QFileInfo(root + "/Docs/cv.pdf").canonicalFilePath());
    QVERIFY(folder.fileForFetch(QStringLiteral("Docs"), error).isEmpty()); // a folder
    QVERIFY(folder.fileForFetch(QString(), error).isEmpty());              // the root
    QVERIFY(folder.fileForFetch(QStringLiteral("Docs/missing.pdf"), error).isEmpty());
}

// ---- Remote input -------------------------------------------------------------

void TestRemoteControl::presenterCanOnlyPressSlideKeys() {
    auto *backend = new FakeBackend;
    RemoteInput input(backend);
    QVERIFY(input.begin("phone", RemoteInput::Mode::Presenter));
    QVERIFY(input.isActive());

    QVERIFY(input.handleEvent("phone", key("right")));
    QVERIFY(input.handleEvent("phone", key("f5")));
    QVERIFY(input.handleEvent("phone", key("blank")));
    const int allowed = backend->calls.size();

    // Anything that types, points, holds or combines is refused.
    QVERIFY(!input.handleEvent("phone", key("a")));
    QVERIFY(!input.handleEvent("phone", key("enter")));
    QVERIFY(!input.handleEvent("phone", key("super")));
    QVERIFY(!input.handleEvent("phone", key("right", "press")));
    QVERIFY(!input.handleEvent("phone", key("left", "tap", {"ctrl"})));
    QVERIFY(!input.handleEvent("phone", {{"kind", "text"}, {"text", "rm -rf ~"}}));
    QVERIFY(!input.handleEvent("phone", {{"kind", "move"}, {"dx", 5}, {"dy", 5}}));
    QVERIFY(!input.handleEvent("phone", {{"kind", "button"}, {"button", "left"}}));
    QCOMPARE(backend->calls.size(), allowed);
}

void TestRemoteControl::fullModeBoundsEverything() {
    auto *backend = new FakeBackend;
    RemoteInput input(backend);
    QVERIFY(input.begin("phone", RemoteInput::Mode::Full));

    QVERIFY(input.handleEvent("phone", {{"kind", "move"}, {"dx", 1e9}, {"dy", -1e9}}));
    QCOMPARE(backend->calls.last().a, RemoteInput::kMaxMotion);
    QCOMPARE(backend->calls.last().b, -RemoteInput::kMaxMotion);

    // Not numbers, not finite, not known: refused, nothing injected.
    const int before = backend->calls.size();
    QVERIFY(!input.handleEvent("phone", {{"kind", "move"}, {"dx", "5"}, {"dy", 1}}));
    QVERIFY(!input.handleEvent("phone", {{"kind", "move"}, {"dx", qQNaN()}, {"dy", 1}}));
    QVERIFY(!input.handleEvent("phone", {{"kind", "button"}, {"button", "fourth"}}));
    QVERIFY(!input.handleEvent("phone", key("0xff0d")));      // no raw keysyms
    QVERIFY(!input.handleEvent("phone", key("a", "tap", {"hyper"})));
    QVERIFY(!input.handleEvent("phone", {{"kind", "teleport"}}));
    QCOMPARE(backend->calls.size(), before);

    // Ctrl+C: modifier down, key down/up, modifier up.
    QVERIFY(input.handleEvent("phone", key("c", "tap", {"ctrl"})));
    QCOMPARE(backend->calls.size(), before + 4);
    QCOMPARE(int(backend->calls.at(before).a), 0xffe3);
    QCOMPARE(int(backend->calls.at(before + 1).a), int('c'));

    QVERIFY(input.handleEvent("phone", {{"kind", "scroll"}, {"dy", 1000}}));
    QCOMPARE(int(backend->calls.last().b), RemoteInput::kMaxScrollSteps);
}

void TestRemoteControl::textIsAllOrNothing() {
    auto *backend = new FakeBackend;
    RemoteInput input(backend);
    QVERIFY(input.begin("phone", RemoteInput::Mode::Full));

    QVERIFY(input.handleEvent("phone", {{"kind", "text"}, {"text", QStringLiteral("Çay ☕\n")}}));
    QCOMPARE(backend->calls.size(), 6 * 2);
    QCOMPARE(int(backend->calls.at(0).a), 0xc7);                // Ç, Latin-1
    QCOMPARE(int(backend->calls.at(8).a), 0x01000000 | 0x2615); // ☕, Unicode keysym
    QCOMPARE(int(backend->calls.at(10).a), 0xff0d);             // newline -> Return

    const int before = backend->calls.size();
    // An escape sequence, a bidi override, an oversize string: nothing typed.
    QVERIFY(!input.handleEvent("phone", {{"kind", "text"}, {"text", QString("ls") + QChar(0x1b) + "[2K"}}));
    QVERIFY(!input.handleEvent("phone", {{"kind", "text"}, {"text", QString("a") + QChar(0x202e) + "b"}}));
    QVERIFY(!input.handleEvent("phone", {{"kind", "text"}, {"text", QString(RemoteInput::kMaxTextChars + 1, 'a')}}));
    QVERIFY(!input.handleEvent("phone", {{"kind", "text"}, {"text", 42}}));
    QCOMPARE(backend->calls.size(), before);
}

void TestRemoteControl::oneDeviceAtATime() {
    auto *backend = new FakeBackend;
    RemoteInput input(backend);
    QVERIFY(input.begin("phone", RemoteInput::Mode::Full));
    QVERIFY(!input.begin("intruder", RemoteInput::Mode::Full));
    QVERIFY(!input.handleEvent("intruder", key("enter")));
    // Only the owner can end it, and after that nobody's events go through.
    input.end("intruder");
    QVERIFY(input.isActive());
    input.end("phone");
    QVERIFY(!input.isActive());
    QVERIFY(!input.handleEvent("phone", key("enter")));
}

void TestRemoteControl::floodIsCut() {
    auto *backend = new FakeBackend;
    RemoteInput input(backend);
    QVERIFY(input.begin("phone", RemoteInput::Mode::Full));
    int accepted = 0;
    for (int i = 0; i < RemoteInput::kMaxEventsPerSecond * 3; ++i) {
        accepted += input.handleEvent("phone", {{"kind", "move"}, {"dx", 1}, {"dy", 0}}) ? 1 : 0;
    }
    QVERIFY(accepted <= RemoteInput::kMaxEventsPerSecond * 2); // at most two windows
    QVERIFY(accepted < RemoteInput::kMaxEventsPerSecond * 3);
}

void TestRemoteControl::previewsAreSmallJpegs() {
    QTemporaryDir dir;
    QImage big(3000, 2000, QImage::Format_ARGB32);
    big.fill(QColor(200, 30, 30, 128)); // translucent: the flattening path
    const QString path = dir.filePath("photo.png");
    QVERIFY(big.save(path));

    for (const int edge : {thumbnailer::kThumbEdge, thumbnailer::kLargeEdge}) {
        QSize size;
        QString error;
        const QByteArray jpeg = thumbnailer::jpegPreview(path, edge, size, error);
        QVERIFY2(!jpeg.isEmpty(), qPrintable(error));
        QVERIFY(jpeg.startsWith("\xFF\xD8")); // a JPEG, whatever went in
        QVERIFY(jpeg.size() <= thumbnailer::kMaxPreviewBytes);
        QCOMPARE(qMax(size.width(), size.height()), edge);
        QCOMPARE(QImage::fromData(jpeg).size(), size);
    }
}

void TestRemoteControl::previewsRefuseWhatIsNotAPicture() {
    QTemporaryDir dir;
    // Named like a picture, is not one: decided by content, refused.
    const QString fake = dir.filePath("evil.jpg");
    writeFile(fake, "#!/bin/sh\nrm -rf ~\n");
    QSize size;
    QString error;
    QVERIFY(thumbnailer::jpegPreview(fake, 160, size, error).isEmpty());
    QVERIFY(!error.isEmpty());

    // A symlink is not followed even here.
    QImage img(10, 10, QImage::Format_RGB32);
    img.fill(Qt::white);
    QVERIFY(img.save(dir.filePath("real.png")));
    QVERIFY(QFile::link(dir.filePath("real.png"), dir.filePath("link.png")));
    QVERIFY(thumbnailer::jpegPreview(dir.filePath("link.png"), 160, size, error).isEmpty());

    QVERIFY(thumbnailer::looksLikeImage("a.JPG"));
    QVERIFY(!thumbnailer::looksLikeImage("a.pdf"));
}

QTEST_MAIN(TestRemoteControl)
#include "tst_remotecontrol.moc"
