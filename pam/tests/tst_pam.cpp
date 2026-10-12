#include <QtTest>

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <security/pam_appl.h>

#include <mutex>
#include <pwd.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "mazeconnect/core/Approval.h"
#include "mazeconnect/core/ApprovalRelay.h"

using namespace mazeconnect::core;

/**
 * pam_mazeconnect through real libpam, with a real relay socket.
 *
 * pam_start_confdir() reads a service file from a directory of the test's
 * own, so the module runs exactly as sudo would load it — only pointed at
 * key and socket directories inside a temporary one. The test plays the
 * phone: it reads the code the module showed (through the conversation, as
 * a terminal would) and signs what the owner would.
 */
class TestPam : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void theRightCodeApproves();
    void theWrongCodeDoesNot();
    void anUnenrolledKeyDoesNot();
    void noPhoneMeansThePasswordAtOnce();
    void aWritableKeyFileIsIgnored();
    void noRelayMeansThePasswordAtOnce();
    void remoteIsLeftAloneSilentIsNot();

private:
    struct Run {
        int result = -1;
        QStringList infos;
        qint64 elapsedMs = 0;
    };

    /// pam_authenticate on its own thread; @p phone runs on this one each
    /// time the relay is asked, with the code the module showed (or empty).
    Run authenticate(const std::function<void(const ApprovalRelay::Request &)> &phone,
                     int flags = 0, const char *rhost = nullptr);
    QString waitForCode(int timeoutMs = 3000);
    QByteArray sign(EVP_PKEY *key, const approval::Challenge &c, const QByteArray &fp);

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<ApprovalRelay> m_relay;
    EVP_PKEY *m_key = nullptr;
    EVP_PKEY *m_other = nullptr;
    QString m_user;

    std::mutex m_mutex;
    QStringList m_infos;
};

namespace {

/**
 * What libpam answers when the only module in the stack stood aside
 * (PAM_IGNORE): no module decided, so the stack fails closed. Pinned, so
 * that a module that did not even load (PAM_MODULE_UNKNOWN) or one that
 * refused (PAM_AUTH_ERR) cannot pass for one that stepped aside — this file
 * once passed four tests with a module whose entry points were not exported.
 */
constexpr int kStoodAside = PAM_PERM_DENIED;

QByteArray spkiOf(EVP_PKEY *key) {
    unsigned char *der = nullptr;
    const int len = i2d_PUBKEY(key, &der);
    QByteArray out(reinterpret_cast<const char *>(der), len);
    OPENSSL_free(der);
    return out;
}

struct ConvState {
    TestPam *test;
    std::mutex *mutex;
    QStringList *infos;
};

int conversation(int count, const struct pam_message **messages, struct pam_response **responses,
                 void *data) {
    auto *state = static_cast<ConvState *>(data);
    auto *replies = static_cast<pam_response *>(calloc(static_cast<size_t>(count),
                                                        sizeof(pam_response)));
    for (int i = 0; i < count; ++i) {
        if (messages[i]->msg_style == PAM_TEXT_INFO || messages[i]->msg_style == PAM_ERROR_MSG) {
            std::lock_guard<std::mutex> lock(*state->mutex);
            state->infos->append(QString::fromUtf8(messages[i]->msg));
        }
    }
    *responses = replies;
    return PAM_SUCCESS;
}

} // namespace

void TestPam::initTestCase() {
    if (::geteuid() == 0) {
        QSKIP("run as an ordinary user: as root the module rightly refuses user-owned files");
    }
    m_user = QString::fromLocal8Bit(::getpwuid(::getuid())->pw_name);
    m_key = EVP_EC_gen("P-256");
    m_other = EVP_EC_gen("P-256");
    qunsetenv("SSH_CONNECTION");
    qunsetenv("SSH_CLIENT");
    qunsetenv("SSH_TTY");
}

void TestPam::init() {
    m_dir = std::make_unique<QTemporaryDir>();
    QDir root(m_dir->path());
    QVERIFY(root.mkpath(QStringLiteral("approvers")));
    QVERIFY(root.mkpath(QStringLiteral("run")));
    QVERIFY(root.mkpath(QStringLiteral("pam.d")));
    ::chmod(QFile::encodeName(root.filePath("approvers")).constData(), 0755);

    QFile keys(root.filePath(QStringLiteral("approvers/") + m_user));
    QVERIFY(keys.open(QIODevice::WriteOnly));
    keys.write("# test\n" + spkiOf(m_key).toBase64() + " Test phone\n");
    keys.close();
    ::chmod(QFile::encodeName(keys.fileName()).constData(), 0644);

    QFile service(root.filePath(QStringLiteral("pam.d/sudo")));
    QVERIFY(service.open(QIODevice::WriteOnly));
    service.write(QStringLiteral("auth required %1 approvers_dir=%2 runtime_dir=%3 timeout=5\n")
                      .arg(QStringLiteral(PAM_MODULE_PATH), root.filePath("approvers"),
                           root.filePath("run"))
                      .toUtf8());
    service.close();

    m_relay = std::make_unique<ApprovalRelay>();
    QVERIFY(m_relay->start(root.filePath(QStringLiteral("run")), nullptr));
    m_relay->setPeerCheckForTesting([](uid_t uid) { return uid == ::getuid(); });
    std::lock_guard<std::mutex> lock(m_mutex);
    m_infos.clear();
}

void TestPam::cleanup() {
    m_relay.reset();
    m_dir.reset();
}

QString TestPam::waitForCode(int timeoutMs) {
    static const QRegularExpression re(QStringLiteral("enter (\\d\\d)"));
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const QString &info : std::as_const(m_infos)) {
                const auto match = re.match(info);
                if (match.hasMatch()) {
                    return match.captured(1);
                }
            }
        }
        QTest::qWait(20);
    }
    return {};
}

QByteArray TestPam::sign(EVP_PKEY *key, const approval::Challenge &c, const QByteArray &fp) {
    const QByteArray input = approval::signingInput(c, fp);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    size_t len = 0;
    EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key);
    EVP_DigestSign(ctx, nullptr, &len, reinterpret_cast<const unsigned char *>(input.constData()),
                   static_cast<size_t>(input.size()));
    QByteArray out(static_cast<qsizetype>(len), Qt::Uninitialized);
    EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(out.data()), &len,
                   reinterpret_cast<const unsigned char *>(input.constData()),
                   static_cast<size_t>(input.size()));
    EVP_MD_CTX_free(ctx);
    out.resize(static_cast<qsizetype>(len));
    return out;
}

TestPam::Run TestPam::authenticate(
    const std::function<void(const ApprovalRelay::Request &)> &phone, int flags,
    const char *rhost) {
    Run run;
    QMetaObject::Connection hook = connect(m_relay.get(), &ApprovalRelay::requested, this,
                                           [&](const ApprovalRelay::Request &r) { phone(r); },
                                           Qt::QueuedConnection);
    ConvState state{this, &m_mutex, &m_infos};
    const QByteArray confdir = QFile::encodeName(QDir(m_dir->path()).filePath("pam.d"));
    const QByteArray user = m_user.toLocal8Bit();
    std::atomic<bool> done{false};
    QElapsedTimer timer;
    timer.start();
    std::thread worker([&] {
        const struct pam_conv conv = {conversation, &state};
        pam_handle_t *pamh = nullptr;
        int rc = pam_start_confdir("sudo", user.constData(), &conv, confdir.constData(), &pamh);
        if (rc == PAM_SUCCESS && rhost) {
            pam_set_item(pamh, PAM_RHOST, rhost);
        }
        if (rc == PAM_SUCCESS) {
            rc = pam_authenticate(pamh, flags);
        }
        run.result = rc;
        pam_end(pamh, rc);
        done = true;
    });
    // The module's own deadline is five seconds; this only stops a hang
    // from hanging the suite.
    while (!done.load() && timer.elapsed() < 15000) {
        QTest::qWait(10);
    }
    worker.join();
    run.elapsedMs = timer.elapsed();
    disconnect(hook);
    std::lock_guard<std::mutex> lock(m_mutex);
    run.infos = m_infos;
    return run;
}

void TestPam::theRightCodeApproves() {
    QSignalSpy finished(m_relay.get(), &ApprovalRelay::finished);
    const Run run = authenticate([this](const ApprovalRelay::Request &r) {
        // What the phone shows is what PAM made — this host, this user.
        QCOMPARE(r.challenge.user, m_user);
        QVERIFY(r.challenge.purpose == approval::Purpose::Sudo);
        QVERIFY(r.challenge.code.isEmpty()); // never sent
        approval::Challenge c = r.challenge;
        c.code = waitForCode();
        QVERIFY(!c.code.isEmpty());
        const QByteArray fp(32, 'F');
        m_relay->answer(r.id, sign(m_key, c, fp), fp, QStringLiteral("Test phone"));
    });
    QCOMPARE(run.result, PAM_SUCCESS);
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(finished.at(0).at(1).toBool(), true);
}

void TestPam::theWrongCodeDoesNot() {
    QSignalSpy finished(m_relay.get(), &ApprovalRelay::finished);
    const Run run = authenticate([this](const ApprovalRelay::Request &r) {
        approval::Challenge c = r.challenge;
        const int shown = waitForCode().toInt();
        // The owner types something else — or something that never saw the
        // terminal guesses.
        c.code = QString::number(shown == 99 ? 10 : shown + 1);
        const QByteArray fp(32, 'F');
        m_relay->answer(r.id, sign(m_key, c, fp), fp, QStringLiteral("Test phone"));
    });
    QCOMPARE(run.result, PAM_AUTH_ERR);
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(finished.at(0).at(1).toBool(), false);
}

void TestPam::anUnenrolledKeyDoesNot() {
    const Run run = authenticate([this](const ApprovalRelay::Request &r) {
        approval::Challenge c = r.challenge;
        c.code = waitForCode();
        const QByteArray fp(32, 'F');
        m_relay->answer(r.id, sign(m_other, c, fp), fp, QStringLiteral("Stranger"));
    });
    QCOMPARE(run.result, PAM_AUTH_ERR);
}

void TestPam::noPhoneMeansThePasswordAtOnce() {
    const Run run = authenticate([this](const ApprovalRelay::Request &r) {
        m_relay->fail(r.id, QStringLiteral("no phone that can approve is connected"));
    });
    QCOMPARE(run.result, kStoodAside);
    QVERIFY2(run.elapsedMs < 2000, "sudo waited for a phone that was never coming");
    // Nothing was said: there was nothing to approve.
    QVERIFY(run.infos.isEmpty());
}

void TestPam::aWritableKeyFileIsIgnored() {
    const QString keys = QDir(m_dir->path()).filePath(QStringLiteral("approvers/") + m_user);
    QVERIFY(::chmod(QFile::encodeName(keys).constData(), 0666) == 0);
    QSignalSpy requested(m_relay.get(), &ApprovalRelay::requested);
    const Run run = authenticate([](const ApprovalRelay::Request &) {});
    QCOMPARE(run.result, kStoodAside);
    QCOMPARE(requested.count(), 0);

    // Nor a key file reached through a symlink.
    QVERIFY(::chmod(QFile::encodeName(keys).constData(), 0644) == 0);
    const QString real = QDir(m_dir->path()).filePath(QStringLiteral("elsewhere"));
    QVERIFY(QFile::rename(keys, real));
    QVERIFY(QFile::link(real, keys));
    const Run linked = authenticate([](const ApprovalRelay::Request &) {});
    QCOMPARE(linked.result, kStoodAside);
    QCOMPARE(requested.count(), 0);

    // Nor one in a directory others can write to.
    QVERIFY(QFile::remove(keys));
    QVERIFY(QFile::rename(real, keys));
    QVERIFY(::chmod(QFile::encodeName(QDir(m_dir->path()).filePath("approvers")).constData(),
                    0777)
            == 0);
    const Run open = authenticate([](const ApprovalRelay::Request &) {});
    QCOMPARE(open.result, kStoodAside);
    QCOMPARE(requested.count(), 0);
}

void TestPam::noRelayMeansThePasswordAtOnce() {
    m_relay->stop();
    const Run run = authenticate([](const ApprovalRelay::Request &) {});
    QCOMPARE(run.result, kStoodAside);
    QVERIFY(run.elapsedMs < 2000);
    QVERIFY(run.infos.isEmpty());
}

void TestPam::remoteIsLeftAloneSilentIsNot() {
    QSignalSpy requested(m_relay.get(), &ApprovalRelay::requested);
    QCOMPARE(authenticate([](const ApprovalRelay::Request &) {}, 0, "attacker.example").result,
             kStoodAside);
    QCOMPARE(requested.count(), 0);
    // sudo calls pam_authenticate with PAM_SILENT by default (pam_silent).
    // Standing aside on it once meant the phone was never asked for sudo;
    // the code is still shown, and approving still works.
    const Run silent = authenticate([this](const ApprovalRelay::Request &r) {
        approval::Challenge c = r.challenge;
        c.code = waitForCode();
        const QByteArray fp(32, 'F');
        m_relay->answer(r.id, sign(m_key, c, fp), fp, QStringLiteral("Test phone"));
    }, PAM_SILENT);
    QCOMPARE(silent.result, PAM_SUCCESS);
    QCOMPARE(requested.count(), 1);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_infos.clear();
    }
    // sudo puts this machine's own name in PAM_RHOST (its pam_rhost option);
    // that is not a remote session, and once was taken for one, which made
    // the module stand aside for every sudo.
    char host[256] = {};
    ::gethostname(host, sizeof(host) - 1);
    authenticate([this](const ApprovalRelay::Request &r) {
        m_relay->fail(r.id, QStringLiteral("no phone"));
    }, 0, host);
    QCOMPARE(requested.count(), 2);
    authenticate([this](const ApprovalRelay::Request &r) {
        m_relay->fail(r.id, QStringLiteral("no phone"));
    }, 0, "localhost");
    QCOMPARE(requested.count(), 3);
    // The same stack, asked plainly, does reach the phone: the two refusals
    // above were the module deciding, not the module missing.
    authenticate([this](const ApprovalRelay::Request &r) {
        m_relay->fail(r.id, QStringLiteral("no phone"));
    });
    QCOMPARE(requested.count(), 4);
}

QTEST_GUILESS_MAIN(TestPam)
#include "tst_pam.moc"
