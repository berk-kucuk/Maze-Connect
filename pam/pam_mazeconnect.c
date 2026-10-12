/*
 * pam_mazeconnect — approve sudo and polkit prompts with a paired phone.
 *
 *     auth  sufficient  pam_mazeconnect.so
 *
 * How it decides, and why it is safe to put in front of a password:
 *
 *  - It only ever *adds* a way in. Anything short of a verified signature —
 *    no enrolled key, no relay, no phone, a timeout, a refusal, a crash in
 *    the user's session — is "not this module", and the stack goes on to the
 *    password exactly as before.
 *  - The trust anchor is a key **root** enrolled in
 *    /etc/maze-connect/approvers/<user>, after its owner compared it with the
 *    phone's screen (`maze-connect-approvers enroll`). The file and every
 *    directory above it must be owned by root and writable by nobody else.
 *  - The challenge is made here: a fresh 32-byte nonce, the time, this host,
 *    the user, what is being approved, and a two-digit code that is shown on
 *    the terminal and **never sent**. The owner types the code on the phone
 *    and the phone signs it with a biometric-bound key. A signature over any
 *    other code does not verify — so something that sees only the relay
 *    cannot get an approval out of the owner for a sudo it started in the
 *    background, because the owner never sees that code.
 *  - The user's Maze Connect process only carries the challenge to the phone
 *    and the signature back. It is not trusted: what it returns is one line
 *    of fixed-form text, bounded, and checked here before anything is
 *    believed. Verification is done here, by root, with OpenSSL.
 *  - Remote sessions (SSH) are left alone unless `allow_remote` is given: a
 *    phone in the owner's pocket is not a reason to let someone at a remote
 *    shell in.
 *
 * Options: timeout=<seconds> (5..60, default 30), allow_remote, debug,
 * approvers_dir=<absolute path>, runtime_dir=<absolute path, %u = uid>.
 */
#define _GNU_SOURCE

#include "approval_core.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-login.h>

#define PAM_SM_AUTH
#include <security/pam_ext.h>
#include <security/pam_modules.h>

/* Built with -fvisibility=hidden: only the two entry points libpam looks
 * up are exported. PAM_EXTERN is plain `extern`, which that hides too. */
#define MC_EXPORT __attribute__((visibility("default")))

#define MC_DEFAULT_APPROVERS "/etc/maze-connect/approvers"
#define MC_DEFAULT_RUNTIME "/run/user/%u"
#define MC_SOCKET_TAIL "/maze-connect/approval.sock"
#define MC_MAX_FILE (64 * 1024)
#define MC_MAX_LINE 512
/* How long to wait for an immediate "no phone" before saying anything. */
#define MC_QUICK_MS 400

struct options {
    int timeout_s;
    int allow_remote;
    int debug;
    const char *approvers_dir;
    const char *runtime_dir;
};

static void parse_options(pam_handle_t *pamh, int argc, const char **argv,
                          struct options *opt) {
    int i;
    opt->timeout_s = 30;
    opt->allow_remote = 0;
    opt->debug = 0;
    opt->approvers_dir = MC_DEFAULT_APPROVERS;
    opt->runtime_dir = MC_DEFAULT_RUNTIME;
    for (i = 0; i < argc; ++i) {
        if (strncmp(argv[i], "timeout=", 8) == 0) {
            char *end = NULL;
            const long v = strtol(argv[i] + 8, &end, 10);
            if (end && *end == '\0' && v >= 5 && v <= 60) {
                opt->timeout_s = (int)v;
            }
        } else if (strcmp(argv[i], "allow_remote") == 0) {
            opt->allow_remote = 1;
        } else if (strcmp(argv[i], "debug") == 0) {
            opt->debug = 1;
        } else if (strncmp(argv[i], "approvers_dir=", 14) == 0 && argv[i][14] == '/') {
            opt->approvers_dir = argv[i] + 14;
        } else if (strncmp(argv[i], "runtime_dir=", 12) == 0 && argv[i][12] == '/') {
            opt->runtime_dir = argv[i] + 12;
        } else {
            pam_syslog(pamh, LOG_WARNING, "unknown option: %s", argv[i]);
        }
    }
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Root, or — only when this module is not running as root, as in its own
 * tests — the user it runs as. A process that is not root cannot gain
 * anything from a file it could have written itself.
 */
static int trusted_owner(uid_t owner) {
    return owner == 0 || (geteuid() != 0 && owner == geteuid());
}

/* Every directory from / down to @dir: trusted owner, not writable by
 * anyone else (a root-owned sticky directory such as /tmp excepted). */
static int trusted_directory_chain(const char *dir) {
    char path[PATH_MAX];
    size_t len;
    size_t i;

    len = strlen(dir);
    if (len == 0 || len >= sizeof(path) || dir[0] != '/') {
        return 0;
    }
    memcpy(path, dir, len + 1);
    for (i = 1; i <= len; ++i) {
        if (path[i] == '/' || path[i] == '\0') {
            struct stat st;
            const char saved = path[i];
            path[i] = '\0';
            if (lstat(i == 0 ? "/" : path, &st) != 0 || !S_ISDIR(st.st_mode)
                || !trusted_owner(st.st_uid)) {
                return 0;
            }
            if ((st.st_mode & 022) != 0 && !(st.st_uid == 0 && (st.st_mode & S_ISVTX))) {
                return 0;
            }
            path[i] = saved;
        }
    }
    {
        struct stat st;
        if (lstat("/", &st) != 0 || st.st_uid != 0 || (st.st_mode & 022) != 0) {
            return 0;
        }
        /* The last component may not be a sticky shared directory itself. */
        if (lstat(dir, &st) != 0 || (st.st_mode & 022) != 0) {
            return 0;
        }
    }
    return 1;
}

static int load_keys(pam_handle_t *pamh, const struct options *opt, const char *user,
                     mc_key *keys) {
    char path[PATH_MAX];
    char *buf;
    struct stat st;
    ssize_t got;
    size_t total = 0;
    int fd;
    int count;

    if (!trusted_directory_chain(opt->approvers_dir)) {
        if (opt->debug) {
            pam_syslog(pamh, LOG_DEBUG, "%s is not a trusted directory", opt->approvers_dir);
        }
        return 0;
    }
    if (snprintf(path, sizeof(path), "%s/%s", opt->approvers_dir, user) >= (int)sizeof(path)) {
        return 0;
    }
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return 0;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || !trusted_owner(st.st_uid)
        || (st.st_mode & 022) != 0 || st.st_size <= 0 || st.st_size > MC_MAX_FILE) {
        pam_syslog(pamh, LOG_WARNING, "ignoring %s: not a root-owned, root-only-writable file",
                   path);
        close(fd);
        return 0;
    }
    buf = malloc((size_t)st.st_size);
    if (!buf) {
        close(fd);
        return 0;
    }
    while (total < (size_t)st.st_size) {
        got = read(fd, buf + total, (size_t)st.st_size - total);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        total += (size_t)got;
    }
    close(fd);
    count = mc_parse_approvers(buf, total, keys, MC_MAX_KEYS);
    free(buf);
    return count;
}

static int connect_relay(pam_handle_t *pamh, const struct options *opt, uid_t uid) {
    char base[PATH_MAX];
    char dir[PATH_MAX];
    char sock_path[PATH_MAX];
    const char *t;
    size_t o = 0;
    struct stat st;
    struct sockaddr_un addr;
    struct ucred cred;
    socklen_t cred_len = sizeof(cred);
    struct timeval tv = {2, 0};
    int fd;

    /* runtime_dir with %u replaced by the uid. */
    for (t = opt->runtime_dir; *t && o + 12 < sizeof(base); ++t) {
        if (t[0] == '%' && t[1] == 'u') {
            o += (size_t)snprintf(base + o, sizeof(base) - o, "%u", (unsigned)uid);
            ++t;
        } else {
            base[o++] = *t;
        }
    }
    if (*t) {
        return -1;
    }
    base[o] = '\0';
    if (snprintf(dir, sizeof(dir), "%s/maze-connect", base) >= (int)sizeof(dir)
        || snprintf(sock_path, sizeof(sock_path), "%s%s", base, MC_SOCKET_TAIL)
            >= (int)sizeof(sock_path)
        || strlen(sock_path) >= sizeof(addr.sun_path)) {
        return -1;
    }
    /* The user's own private directory and socket — nobody else's. */
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != uid
        || (st.st_mode & 077) != 0) {
        return -1;
    }
    if (lstat(sock_path, &st) != 0 || !S_ISSOCK(st.st_mode) || st.st_uid != uid) {
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0
        || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0
        || cred_len != sizeof(cred) || cred.uid != uid) {
        if (opt->debug) {
            pam_syslog(pamh, LOG_DEBUG, "relay at %s is not reachable or not the user's",
                       sock_path);
        }
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        const ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * One line into @line, waiting until @deadline_ms. Returns 1 for a line, 0
 * for the deadline, -1 for a closed or misbehaving peer, -2 if interrupted.
 */
static int read_line(int fd, char *line, size_t cap, size_t *have, long long deadline_ms) {
    for (;;) {
        char *nl = memchr(line, '\n', *have);
        struct pollfd pfd;
        long long left;
        int r;
        ssize_t n;

        if (nl) {
            *nl = '\0';
            return 1;
        }
        if (*have + 1 >= cap) {
            return -1;
        }
        left = deadline_ms - now_ms();
        if (left <= 0) {
            return 0;
        }
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        r = poll(&pfd, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (r < 0) {
            return errno == EINTR ? -2 : -1;
        }
        if (r == 0) {
            return 0;
        }
        n = recv(fd, line + *have, cap - 1 - *have, 0);
        if (n < 0 && errno == EINTR) {
            return -2;
        }
        if (n <= 0) {
            return -1;
        }
        *have += (size_t)n;
        if (memchr(line, '\0', *have)) {
            return -1;
        }
    }
}

static const char *purpose_for(const char *service) {
    if (!service) {
        return NULL;
    }
    if (strcmp(service, "sudo") == 0 || strcmp(service, "sudo-i") == 0) {
        return "sudo";
    }
    if (strcmp(service, "polkit-1") == 0) {
        return "polkit";
    }
    return NULL;
}

/* Whether @name is this machine: "localhost", a loopback address, or this
 * host's own name (with or without its domain). */
static int is_local_host(const char *name) {
    char host[256];
    size_t short_len;
    if (strcmp(name, "localhost") == 0 || strcmp(name, "127.0.0.1") == 0
        || strcmp(name, "::1") == 0) {
        return 1;
    }
    if (gethostname(host, sizeof(host)) != 0) {
        return 0;
    }
    host[sizeof(host) - 1] = '\0';
    if (strcmp(name, host) == 0) {
        return 1;
    }
    short_len = strcspn(host, ".");
    return strlen(name) == short_len && strncmp(name, host, short_len) == 0;
}

static int is_remote(pam_handle_t *pamh) {
    const void *item = NULL;
    char *session = NULL;
    int remote = 0;

    /* sudo sets PAM_RHOST to this machine's own name (its pam_rhost
     * option); sshd sets it to the client. Only the latter is remote. */
    if (pam_get_item(pamh, PAM_RHOST, &item) == PAM_SUCCESS && item
        && ((const char *)item)[0] && !is_local_host((const char *)item)) {
        return 1;
    }
    /* The logind session this process belongs to: an SSH login is marked
     * remote there, by sshd's own PAM stack, and the caller cannot change
     * that — unlike the environment. Not being in a session at all (polkit's
     * socket-activated helper) is not remote. */
    if (sd_pid_get_session(0, &session) >= 0 && session) {
        remote = sd_session_is_remote(session) > 0;
        free(session);
        if (remote) {
            return 1;
        }
    }
    /* Defence in depth only: the caller can unset these, which would just
     * bring the phone prompt back — it cannot make anything verify. */
    return getenv("SSH_CONNECTION") != NULL || getenv("SSH_CLIENT") != NULL
        || getenv("SSH_TTY") != NULL;
}

static int random_code(char out[3]) {
    for (;;) {
        unsigned char byte;
        if (getrandom(&byte, 1, 0) != 1) {
            return -1;
        }
        if (byte < 180) {
            const int v = 10 + byte % 90;
            out[0] = (char)('0' + v / 10);
            out[1] = (char)('0' + v % 10);
            out[2] = '\0';
            return 0;
        }
    }
}

static void build_detail(pam_handle_t *pamh, const char *purpose, char *out, size_t cap) {
    const void *tty = NULL;
    char clean[64];
    if (strcmp(purpose, "sudo") == 0 && pam_get_item(pamh, PAM_TTY, &tty) == PAM_SUCCESS && tty
        && ((const char *)tty)[0]) {
        const char *name = (const char *)tty;
        if (strncmp(name, "/dev/", 5) == 0) {
            name += 5;
        }
        mc_printable(name, clean, sizeof(clean));
        snprintf(out, cap, "sudo \xC2\xB7 %s", clean);
    } else {
        snprintf(out, cap, "%s", purpose);
    }
}

MC_EXPORT PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
                                   const char **argv) {
    struct options opt;
    const void *service_item = NULL;
    const char *purpose;
    const char *user = NULL;
    struct passwd pw;
    struct passwd *pwres = NULL;
    char pwbuf[4096];
    mc_key keys[MC_MAX_KEYS];
    int key_count;
    int fd = -1;
    unsigned char nonce[MC_NONCE_SIZE];
    char nonce_b64[64];
    char code[3];
    char host_raw[256];
    char host[MC_MAX_HOST + 1];
    char detail[MC_MAX_DETAIL + 1];
    char esc_host[2 * MC_MAX_HOST + 1];
    char esc_detail[2 * MC_MAX_DETAIL + 1];
    char request[1024];
    char line[MC_MAX_LINE];
    size_t have = 0;
    long long issued_at;
    long long deadline;
    int r;
    int prompted = 0;
    int result = PAM_IGNORE;
    static const char kVerified[] = "{\"v\":1,\"verified\":true}\n";
    static const char kRefused[] = "{\"v\":1,\"verified\":false}\n";

    memset(keys, 0, sizeof(keys));
    /* PAM_SILENT is not a reason to stand aside. sudo passes it on every
     * call (its pam_silent default) and still prints what the conversation
     * is given, as it does for pam_fprintd's "place your finger"; standing
     * aside on it made this module do nothing for sudo at all. The code has
     * to reach the terminal for the approval to mean anything, so it is
     * shown regardless. */
    (void)flags;
    parse_options(pamh, argc, argv, &opt);

    if (pam_get_item(pamh, PAM_SERVICE, &service_item) != PAM_SUCCESS) {
        return PAM_IGNORE;
    }
    purpose = purpose_for((const char *)service_item);
    if (!purpose) {
        return PAM_IGNORE;
    }
    if (pam_get_user(pamh, &user, NULL) != PAM_SUCCESS || !mc_valid_username(user)) {
        return PAM_IGNORE;
    }
    if (getpwnam_r(user, &pw, pwbuf, sizeof(pwbuf), &pwres) != 0 || !pwres || pw.pw_uid == 0) {
        return PAM_IGNORE;
    }
    if (!opt.allow_remote && is_remote(pamh)) {
        if (opt.debug) {
            pam_syslog(pamh, LOG_DEBUG, "remote session, not asking the phone");
        }
        return PAM_IGNORE;
    }

    key_count = load_keys(pamh, &opt, user, keys);
    if (key_count <= 0) {
        return PAM_IGNORE;
    }
    fd = connect_relay(pamh, &opt, pw.pw_uid);
    if (fd < 0) {
        goto out;
    }

    if (getrandom(nonce, sizeof(nonce), 0) != (ssize_t)sizeof(nonce) || random_code(code) != 0
        || mc_b64encode(nonce, sizeof(nonce), nonce_b64, sizeof(nonce_b64)) < 0) {
        goto out;
    }
    if (gethostname(host_raw, sizeof(host_raw)) != 0) {
        snprintf(host_raw, sizeof(host_raw), "localhost");
    }
    host_raw[sizeof(host_raw) - 1] = '\0';
    mc_printable(host_raw, host, sizeof(host));
    build_detail(pamh, purpose, detail, sizeof(detail));
    issued_at = (long long)time(NULL);
    if (mc_json_escape(host, esc_host, sizeof(esc_host)) != 0
        || mc_json_escape(detail, esc_detail, sizeof(esc_detail)) != 0) {
        goto out;
    }
    r = snprintf(request, sizeof(request),
                 "{\"v\":1,\"purpose\":\"%s\",\"host\":\"%s\",\"user\":\"%s\","
                 "\"detail\":\"%s\",\"nonce\":\"%s\",\"issuedAt\":%lld,\"timeoutMs\":%d}\n",
                 purpose, esc_host, user, esc_detail, nonce_b64, issued_at,
                 opt.timeout_s * 1000);
    if (r <= 0 || r >= (int)sizeof(request) || send_all(fd, request, (size_t)r) != 0) {
        goto out;
    }

    /* No phone that can answer is said at once; then nothing is shown and
     * the password prompt comes straight away. */
    deadline = now_ms() + opt.timeout_s * 1000LL;
    r = read_line(fd, line, sizeof(line), &have, now_ms() + MC_QUICK_MS);
    if (r == 0) {
        pam_info(pamh, "Maze Connect: approve on your phone and enter %s "
                       "(or wait %d s for the password).",
                 code, opt.timeout_s);
        prompted = 1;
        r = read_line(fd, line, sizeof(line), &have, deadline);
    }
    if (r == 1) {
        unsigned char sig[MC_MAX_SIG];
        size_t sig_len = 0;
        unsigned char fp[MC_FP_SIZE];
        char reason[161];
        const int kind = mc_parse_answer(line, sig, &sig_len, fp, reason, sizeof(reason));
        if (kind == 0) {
            if (opt.debug) {
                pam_syslog(pamh, LOG_DEBUG, "relay: %s", reason);
            }
            /* Only worth saying if the prompt above was shown. */
            if (prompted) {
                pam_info(pamh, "Maze Connect: %s", reason);
            }
            result = PAM_IGNORE;
        } else if (kind == 1) {
            unsigned char input[MC_MAX_INPUT];
            mc_challenge c;
            int input_len;
            int i;
            int matched = -1;
            c.purpose = purpose;
            c.host = host;
            c.user = user;
            c.detail = detail;
            c.nonce = nonce;
            c.code = code;
            c.issued_at = issued_at;
            input_len = mc_signing_input(&c, fp, input, sizeof(input));
            for (i = 0; input_len > 0 && i < key_count && matched < 0; ++i) {
                if (mc_verify(keys[i].key, input, (size_t)input_len, sig, sig_len)) {
                    matched = i;
                }
            }
            if (matched >= 0) {
                pam_syslog(pamh, LOG_NOTICE, "%s for %s approved on the phone \"%s\"",
                           purpose, user, keys[matched].label);
                send_all(fd, kVerified, sizeof(kVerified) - 1);
                result = PAM_SUCCESS;
            } else {
                pam_syslog(pamh, LOG_WARNING,
                           "%s for %s: a phone answered, but its signature did not verify "
                           "(wrong code, or a key not enrolled)",
                           purpose, user);
                send_all(fd, kRefused, sizeof(kRefused) - 1);
                pam_info(pamh, "Maze Connect: not approved — was the code %s?", code);
                result = PAM_AUTH_ERR;
            }
        } else {
            pam_syslog(pamh, LOG_WARNING, "malformed answer from the relay");
            result = PAM_AUTH_ERR;
        }
    } else if (r == 0) {
        pam_info(pamh, "Maze Connect: no answer from the phone.");
        result = PAM_AUTH_ERR;
    } else {
        result = prompted || r == -2 ? PAM_AUTH_ERR : PAM_IGNORE;
    }

out:
    if (fd >= 0) {
        close(fd);
    }
    mc_free_keys(keys, key_count > 0 ? key_count : 0);
    explicit_bzero(nonce, sizeof(nonce));
    return result;
}

MC_EXPORT PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_SUCCESS;
}
