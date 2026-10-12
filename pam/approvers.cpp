// maze-connect-approvers — enrol a phone for sudo/polkit approval, as root.
//
// pam_mazeconnect trusts exactly what is in /etc/maze-connect/approvers/<user>
// and nothing else, so this tool is where that trust is decided. It reads the
// phone's approval key from the user's own pairing record — with the user's
// privileges, not root's — shows its code, and writes it only after a person
// at this terminal has said the phone shows the same code. The user's
// Maze Connect process could have put any key in that record; the comparison
// is what stops a key that did not come from the phone.

#include "mazeconnect/core/Approval.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <iostream>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr auto kApproversDir = "/etc/maze-connect/approvers";
constexpr auto kModule = "pam_mazeconnect.so";
constexpr auto kMarker = "# maze-connect";
constexpr auto kCopiedHeader = "# maze-connect: copied from ";

using mazeconnect::core::approval::isAcceptableKey;
using mazeconnect::core::approval::keyCode;

struct Approver {
    QByteArray key;
    QString label;
};

[[noreturn]] void die(const QString &message) {
    std::cerr << "maze-connect-approvers: " << message.toStdString() << std::endl;
    std::exit(1);
}

bool validUser(const QString &user) {
    static const QRegularExpression re(QStringLiteral("^[a-z_][a-z0-9_.-]{0,31}$"));
    return re.match(user).hasMatch() && user != QLatin1StringView(".")
        && user != QLatin1StringView("..");
}

/// A label the PAM module will print as it is: printable ASCII, no spaces at
/// the ends, at most 60 characters.
QString cleanLabel(const QString &raw) {
    QString out;
    for (const QChar c : raw) {
        out.append(c.unicode() >= 0x20 && c.unicode() <= 0x7E ? c : QLatin1Char('?'));
    }
    return out.trimmed().left(60);
}

QString approversFile(const QString &user) {
    return QStringLiteral("%1/%2").arg(QLatin1StringView(kApproversDir), user);
}

QList<Approver> readApprovers(const QString &user) {
    QList<Approver> out;
    QFile file(approversFile(user));
    if (!file.open(QIODevice::ReadOnly)) {
        return out;
    }
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (const QByteArray &line : lines) {
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        const qsizetype space = line.indexOf(' ');
        const QByteArray key = QByteArray::fromBase64(
            space < 0 ? line : line.left(space),
            QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        if (!isAcceptableKey(key)) {
            continue;
        }
        out.append({key, space < 0 ? QString() : QString::fromLatin1(line.mid(space + 1))});
    }
    return out;
}

/// Write @p content to @p path atomically, root:root 0644.
void writeRootFile(const QString &path, const QByteArray &content) {
    const QByteArray target = QFile::encodeName(path);
    QByteArray tmp = target + ".XXXXXX";
    const int fd = ::mkstemp(tmp.data());
    if (fd < 0) {
        die(QStringLiteral("cannot write next to %1: %2").arg(path, QString::fromLocal8Bit(
                                                                      std::strerror(errno))));
    }
    bool ok = ::fchown(fd, 0, 0) == 0 && ::fchmod(fd, 0644) == 0;
    qsizetype written = 0;
    while (ok && written < content.size()) {
        const ssize_t n = ::write(fd, content.constData() + written,
                                  static_cast<size_t>(content.size() - written));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        ok = n > 0;
        written += n > 0 ? n : 0;
    }
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || ::rename(tmp.constData(), target.constData()) != 0) {
        ::unlink(tmp.constData());
        die(QStringLiteral("could not write %1").arg(path));
    }
}

void ensureApproversDir() {
    for (const char *dir : {"/etc/maze-connect", kApproversDir}) {
        if (::mkdir(dir, 0755) != 0 && errno != EEXIST) {
            die(QStringLiteral("cannot create %1").arg(QLatin1StringView(dir)));
        }
        struct stat st {};
        if (::lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0
            || (st.st_mode & 022) != 0) {
            die(QStringLiteral("%1 must be a directory owned by root and writable only by it")
                    .arg(QLatin1StringView(dir)));
        }
    }
}

void writeApprovers(const QString &user, const QList<Approver> &approvers) {
    ensureApproversDir();
    QByteArray content = "# Phones that may approve sudo/polkit for " + user.toLatin1()
        + ".\n# Managed by maze-connect-approvers; see pam_mazeconnect.\n";
    for (const Approver &a : approvers) {
        content += a.key.toBase64() + ' ' + cleanLabel(a.label).toLatin1() + '\n';
    }
    if (approvers.isEmpty()) {
        QFile::remove(approversFile(user));
        return;
    }
    writeRootFile(approversFile(user), content);
}

/// The user that ran sudo/pkexec, unless named.
QString targetUser(const QString &named) {
    if (!named.isEmpty()) {
        return named;
    }
    if (const char *sudo = std::getenv("SUDO_USER")) {
        return QString::fromLocal8Bit(sudo);
    }
    if (const char *uid = std::getenv("PKEXEC_UID")) {
        if (const struct passwd *pw = ::getpwuid(static_cast<uid_t>(std::atol(uid)))) {
            return QString::fromLocal8Bit(pw->pw_name);
        }
    }
    die(QStringLiteral("run with sudo, or name the user with --user"));
}

/**
 * The user's devices.json, read **as that user**: root reading a file in
 * someone's home directory would follow whatever link they left there.
 */
QByteArray readAsUser(const struct passwd &pw, const QString &path) {
    if (::setegid(pw.pw_gid) != 0 || ::seteuid(pw.pw_uid) != 0) {
        die(QStringLiteral("cannot drop privileges to read %1").arg(path));
    }
    QByteArray data;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) && file.size() <= 4 * 1024 * 1024) {
        data = file.readAll();
    }
    if (::seteuid(0) != 0 || ::setegid(0) != 0) {
        die(QStringLiteral("cannot restore privileges"));
    }
    return data;
}

bool confirm(const QString &question) {
    std::cout << question.toStdString() << " " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return false;
    }
    return answer == "yes";
}

void enroll(const QString &named, const QString &devicesPath) {
    const QString user = targetUser(named);
    if (!validUser(user)) {
        die(QStringLiteral("not a user name this can use: %1").arg(user));
    }
    const struct passwd *found = ::getpwnam(user.toLocal8Bit().constData());
    if (!found || found->pw_uid == 0) {
        die(QStringLiteral("no such (non-root) user: %1").arg(user));
    }
    const struct passwd pw = *found;
    if (!::isatty(STDIN_FILENO)) {
        die(QStringLiteral("enrolment asks you to compare a code; run it in a terminal"));
    }

    const QString path = devicesPath.isEmpty()
        ? QStringLiteral("%1/.local/share/mazeconnect/devices.json")
              .arg(QString::fromLocal8Bit(pw.pw_dir))
        : devicesPath;
    const QJsonArray devices = QJsonDocument::fromJson(readAsUser(pw, path)).array();
    struct Candidate {
        QString name;
        QByteArray key;
    };
    QList<Candidate> candidates;
    for (const QJsonValue &value : devices) {
        const QJsonObject obj = value.toObject();
        const QByteArray key = QByteArray::fromBase64(
            obj.value(QLatin1StringView("approvalKey")).toString().toLatin1(),
            QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        if (isAcceptableKey(key)) {
            candidates.append({cleanLabel(obj.value(QLatin1StringView("deviceName")).toString()),
                               key});
        }
    }
    if (candidates.isEmpty()) {
        die(QStringLiteral("no paired phone has set up approvals yet (%1).\n"
                           "On the phone: Settings > Approvals > Set up, then try again.")
                .arg(path));
    }

    std::cout << "Phones paired with " << user.toStdString() << "'s Maze Connect:\n";
    for (qsizetype i = 0; i < candidates.size(); ++i) {
        std::cout << "  " << (i + 1) << ") " << candidates[i].name.toStdString() << "   "
                  << keyCode(candidates[i].key).toStdString() << "\n";
    }
    qsizetype choice = 0;
    if (candidates.size() > 1) {
        std::cout << "Which one? " << std::flush;
        std::string line;
        std::getline(std::cin, line);
        choice = QString::fromStdString(line).toInt() - 1;
        if (choice < 0 || choice >= candidates.size()) {
            die(QStringLiteral("nothing chosen"));
        }
    }
    const Candidate &chosen = candidates[choice];
    std::cout << "\nOn the phone, open Settings > Approvals. It must show exactly:\n\n    "
              << keyCode(chosen.key).toStdString() << "\n\n";
    if (!confirm(QStringLiteral("Does it? Type yes to let this phone approve sudo and polkit "
                                "for %1:")
                     .arg(user))) {
        die(QStringLiteral("not enrolled"));
    }

    QList<Approver> approvers = readApprovers(user);
    for (const Approver &a : approvers) {
        if (a.key == chosen.key) {
            std::cout << "Already enrolled.\n";
            return;
        }
    }
    approvers.append({chosen.key, chosen.name});
    writeApprovers(user, approvers);
    std::cout << "Enrolled " << chosen.name.toStdString() << " for " << user.toStdString()
              << ".\nTurn it on with: maze-connect-approvers pam enable sudo (and/or polkit)\n";
}

void list(const QString &named) {
    QStringList users;
    if (!named.isEmpty()) {
        users << named;
    } else {
        users = QDir(QLatin1StringView(kApproversDir)).entryList(QDir::Files);
    }
    for (const QString &user : users) {
        if (!validUser(user)) {
            continue;
        }
        const auto approvers = readApprovers(user);
        for (const Approver &a : approvers) {
            std::cout << user.toStdString() << "\t" << keyCode(a.key).toStdString() << "\t"
                      << cleanLabel(a.label).toStdString() << "\n";
        }
    }
}

void remove(const QString &user, const QString &code) {
    if (!validUser(user)) {
        die(QStringLiteral("not a user name this can use: %1").arg(user));
    }
    QList<Approver> approvers = readApprovers(user);
    const qsizetype before = approvers.size();
    approvers.removeIf([&](const Approver &a) {
        return code == QLatin1StringView("--all") || keyCode(a.key) == code.toUpper();
    });
    if (approvers.size() == before) {
        die(QStringLiteral("no such key for %1").arg(user));
    }
    writeApprovers(user, approvers);
    std::cout << "Removed " << (before - approvers.size()) << " key(s).\n";
}

// ---- PAM configuration ------------------------------------------------------

struct Service {
    const char *name;    ///< sudo, polkit
    const char *etc;     ///< what we edit
    const char *vendor;  ///< what to start from if the /etc file is absent
};

constexpr Service kServices[] = {
    {"sudo", "/etc/pam.d/sudo", "/usr/lib/pam.d/sudo"},
    {"polkit", "/etc/pam.d/polkit-1", "/usr/lib/pam.d/polkit-1"},
};

const Service &service(const QString &name) {
    for (const Service &s : kServices) {
        if (name == QLatin1StringView(s.name)) {
            return s;
        }
    }
    die(QStringLiteral("unknown service %1 (sudo or polkit)").arg(name));
}

bool moduleInstalled() {
    for (const char *dir : {"/usr/lib/security", "/usr/lib64/security", "/lib/security",
                            "/usr/lib/x86_64-linux-gnu/security"}) {
        if (QFile::exists(QStringLiteral("%1/%2").arg(QLatin1StringView(dir),
                                                      QLatin1StringView(kModule)))) {
            return true;
        }
    }
    return false;
}

// polkit 126+ runs its PAM conversation in polkit-agent-helper@.service,
// whose ProtectHome=yes hides /run/user — and with it the relay socket. This
// drop-in swaps that for ProtectHome=tmpfs (home directories stay hidden)
// and makes /run/user visible read-only. Written only by `pam enable polkit`,
// removed by `pam disable polkit`.
constexpr auto kPolkitUnit = "/usr/lib/systemd/system/polkit-agent-helper@.service";
constexpr auto kPolkitDropInDir = "/etc/systemd/system/polkit-agent-helper@.service.d";
constexpr auto kPolkitDropIn =
    "/etc/systemd/system/polkit-agent-helper@.service.d/maze-connect.conf";

void reloadSystemd() {
    // Fixed argv, no shell. A failure only means the change applies at the
    // next boot instead of now.
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execl("/usr/bin/systemctl", "systemctl", "daemon-reload", static_cast<char *>(nullptr));
        ::_exit(127);
    }
    if (pid > 0) {
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
}

void polkitDropIn(bool install) {
    if (!QFile::exists(QLatin1StringView(kPolkitUnit))) {
        return; // an older polkit runs the helper setuid, outside any sandbox
    }
    if (install) {
        if (::mkdir(kPolkitDropInDir, 0755) != 0 && errno != EEXIST) {
            die(QStringLiteral("cannot create %1").arg(QLatin1StringView(kPolkitDropInDir)));
        }
        writeRootFile(QString::fromLatin1(kPolkitDropIn),
                      "# maze-connect: lets pam_mazeconnect reach the approval relay in\n"
                      "# $XDG_RUNTIME_DIR/maze-connect. Remove with:\n"
                      "#     maze-connect-approvers pam disable polkit\n"
                      "[Service]\n"
                      "ProtectHome=tmpfs\n"
                      "BindReadOnlyPaths=-/run/user\n");
    } else if (!QFile::remove(QLatin1StringView(kPolkitDropIn))) {
        return;
    }
    reloadSystemd();
}

bool isEnabled(const Service &s) {
    QFile file(QLatin1StringView(s.etc));
    return file.open(QIODevice::ReadOnly) && file.readAll().contains(kModule);
}

void pamEnable(const Service &s) {
    if (!moduleInstalled()) {
        die(QStringLiteral("%1 is not installed").arg(QLatin1StringView(kModule)));
    }
    if (isEnabled(s)) {
        // Still (re)write the polkit drop-in: an install from before it
        // existed has the PAM line but not the sandbox change.
        if (QLatin1StringView(s.name) == QLatin1StringView("polkit")) {
            polkitDropIn(true);
        }
        std::cout << s.name << ": already enabled\n";
        return;
    }
    QByteArray content;
    bool copied = false;
    QFile etc(QLatin1StringView(s.etc));
    if (etc.open(QIODevice::ReadOnly)) {
        content = etc.readAll();
        etc.close();
    } else {
        QFile vendor(QLatin1StringView(s.vendor));
        if (!vendor.open(QIODevice::ReadOnly)) {
            die(QStringLiteral("neither %1 nor %2 exists")
                    .arg(QLatin1StringView(s.etc), QLatin1StringView(s.vendor)));
        }
        content = vendor.readAll();
        copied = true;
    }
    QList<QByteArray> lines = content.split('\n');
    qsizetype at = -1;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QByteArray t = lines[i].trimmed();
        if (t.startsWith("auth") || t.startsWith("-auth")) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        die(QStringLiteral("%1 has no auth lines; not touching it").arg(QLatin1StringView(s.etc)));
    }
    // sufficient: a verified approval ends authentication; anything else —
    // including this module missing or failing — falls through to what was
    // there before.
    lines.insert(at, QByteArray("auth       sufficient   ") + kModule + "   " + kMarker);
    if (copied) {
        lines.insert(lines.first().startsWith("#%PAM") ? 1 : 0,
                     QByteArray(kCopiedHeader) + s.vendor);
    }
    writeRootFile(QString::fromLatin1(s.etc), lines.join('\n'));
    if (QLatin1StringView(s.name) == QLatin1StringView("polkit")) {
        polkitDropIn(true);
    }
    std::cout << s.name << ": enabled (" << s.etc << ")\n";
}

void pamDisable(const Service &s) {
    if (QLatin1StringView(s.name) == QLatin1StringView("polkit")) {
        polkitDropIn(false);
    }
    QFile etc(QLatin1StringView(s.etc));
    if (!etc.open(QIODevice::ReadOnly)) {
        return;
    }
    const QList<QByteArray> lines = etc.readAll().split('\n');
    etc.close();
    QList<QByteArray> kept;
    bool copied = false;
    for (const QByteArray &line : lines) {
        if (line.contains(kModule) && line.contains(kMarker)) {
            continue;
        }
        if (line.startsWith(kCopiedHeader)) {
            copied = true;
            continue;
        }
        kept.append(line);
    }
    if (kept.size() == lines.size()) {
        std::cout << s.name << ": not enabled\n";
        return;
    }
    // A file this tool made from the vendor default goes away again if it
    // is back to being exactly that default.
    if (copied) {
        QFile vendor(QLatin1StringView(s.vendor));
        if (vendor.open(QIODevice::ReadOnly) && vendor.readAll() == kept.join('\n')) {
            QFile::remove(QLatin1StringView(s.etc));
            std::cout << s.name << ": disabled (removed " << s.etc << ")\n";
            return;
        }
    }
    writeRootFile(QString::fromLatin1(s.etc), kept.join('\n'));
    std::cout << s.name << ": disabled\n";
}

void usage() {
    std::cout <<
        "usage: maze-connect-approvers <command>\n"
        "\n"
        "  enroll [--user NAME] [--devices FILE]   let a paired phone approve sudo/polkit\n"
        "  list [NAME]                             show enrolled phones\n"
        "  remove NAME (CODE|--all)                stop a phone approving for NAME\n"
        "  pam status                              which services ask the phone\n"
        "  pam enable (sudo|polkit)                ask the phone before the password\n"
        "  pam disable (sudo|polkit|all)           stop asking\n";
}

void requireRoot() {
    if (::geteuid() != 0) {
        die(QStringLiteral("this needs root: run it with sudo"));
    }
}

} // namespace

int main(int argc, char *argv[]) {
    ::umask(022);
    QStringList args;
    for (int i = 1; i < argc; ++i) {
        args << QString::fromLocal8Bit(argv[i]);
    }
    if (args.isEmpty() || args.first() == QLatin1StringView("--help")) {
        usage();
        return args.isEmpty() ? 1 : 0;
    }
    const QString command = args.takeFirst();

    if (command == QLatin1StringView("enroll")) {
        requireRoot();
        QString user;
        QString devices;
        for (qsizetype i = 0; i < args.size(); ++i) {
            if (args[i] == QLatin1StringView("--user") && i + 1 < args.size()) {
                user = args[++i];
            } else if (args[i] == QLatin1StringView("--devices") && i + 1 < args.size()) {
                devices = args[++i];
            } else {
                usage();
                return 1;
            }
        }
        enroll(user, devices);
    } else if (command == QLatin1StringView("list")) {
        list(args.value(0));
    } else if (command == QLatin1StringView("remove") && args.size() == 2) {
        requireRoot();
        remove(args[0], args[1]);
    } else if (command == QLatin1StringView("pam") && !args.isEmpty()) {
        const QString action = args.takeFirst();
        if (action == QLatin1StringView("status")) {
            std::cout << "module: " << (moduleInstalled() ? "installed" : "not installed")
                      << "\n";
            for (const Service &s : kServices) {
                std::cout << s.name << ": " << (isEnabled(s) ? "enabled" : "disabled") << "\n";
            }
        } else if (action == QLatin1StringView("enable") && args.size() == 1) {
            requireRoot();
            pamEnable(service(args[0]));
        } else if (action == QLatin1StringView("disable") && args.size() == 1) {
            requireRoot();
            if (args[0] == QLatin1StringView("all")) {
                for (const Service &s : kServices) {
                    pamDisable(s);
                }
            } else {
                pamDisable(service(args[0]));
            }
        } else {
            usage();
            return 1;
        }
    } else {
        usage();
        return 1;
    }
    return 0;
}
