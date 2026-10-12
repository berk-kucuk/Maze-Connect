#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace mazeconnect::core::approval {

/**
 * What a phone's fingerprint is being asked to approve.
 *
 * A closed set, so the phone can say the true thing in words of its own
 * rather than display a label the computer chose: "unlock the screen" and
 * "administrator access" are the phone's sentences, not ours.
 */
enum class Purpose {
    Unlock, ///< unlock this computer's screen — asked for by the phone itself
    Sudo,   ///< `sudo` on this computer, through pam_mazeconnect
    Polkit, ///< a polkit (or run0) prompt, through pam_mazeconnect
};

QString purposeName(Purpose purpose);
std::optional<Purpose> purposeFromName(const QString &name);

/**
 * One challenge, as signed.
 *
 * Every field the phone shows its owner is part of what gets signed, so a
 * signature cannot be lifted from one request and presented for another.
 * `code` is the number shown on this computer that the owner types on the
 * phone; it never crosses the network in the request, only inside the
 * signature, which is what makes the typing mean anything.
 */
struct Challenge {
    Purpose purpose = Purpose::Unlock;
    QString host;
    QString user;
    QString detail;
    QByteArray nonce; ///< limits::kApprovalNonceSize random bytes
    QString code;     ///< empty when the purpose takes none
    qint64 issuedAt = 0; ///< seconds since the epoch, as the computer saw it
};

/**
 * The exact bytes the phone's approval key signs:
 *
 *     "maze-connect/approval/v1"
 *       || lp(purpose) || lp(host) || lp(user) || lp(detail)
 *       || lp(nonce)   || lp(code) || lp(issuedAt as decimal ASCII)
 *       || lp(SHA-256 of the verifying computer's identity key)
 *
 * `lp(x)` is a 4-byte big-endian length followed by x; strings are UTF-8.
 * The last field binds the signature to the computer the phone believes it
 * is answering — the phone takes it from its own pin, never from the request
 * — so an answer given to one paired computer is worthless to another.
 *
 * Mirrored byte for byte by the phone's ApprovalSigning.kt and by
 * pam/pam_mazeconnect.c; the known-answer vector in tst_approval holds all
 * three to it.
 */
QByteArray signingInput(const Challenge &challenge, const QByteArray &verifierFingerprint);

/// True for an SPKI DER public key on P-256 — the only kind of approval key.
bool isAcceptableKey(const QByteArray &spkiDer);

/**
 * Verify a DER ECDSA-SHA256 signature by @p spkiDer over @p input.
 *
 * Refuses anything but P-256, and a signature outside the DER size bounds,
 * before handing either to OpenSSL.
 */
bool verify(const QByteArray &spkiDer, const QByteArray &input, const QByteArray &derSignature);

/**
 * A short form of an approval key for people to compare: the first eight
 * bytes of its SHA-256, as four groups of four hex digits. The phone shows
 * the same thing for its own key, and `maze-connect-approvers enroll` asks
 * for it to be checked before a key is trusted for sudo.
 */
QString keyCode(const QByteArray &spkiDer);

QByteArray newNonce();

/// A uniformly random limits::kApprovalCodeDigits-digit code with no
/// leading zero, so what is typed is exactly what was shown.
QString newCode();

} // namespace mazeconnect::core::approval
