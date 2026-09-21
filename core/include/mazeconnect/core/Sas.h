#pragma once

#include <QByteArray>
#include <QString>

namespace mazeconnect::core {

/**
 * Short Authentication String — the 6-digit code both users compare on
 * screen during pairing.
 *
 * WHY IT IS BOUND TO PUBLIC KEYS, NOT THE TLS EXPORTER SECRET
 * -----------------------------------------------------------
 * The original design bound the SAS to the RFC 5705 `tls-exporter` secret.
 * Neither Qt's QSslSocket nor Android's javax.net.ssl exposes
 * SSL_export_keying_material, so that binding is not reachable from either
 * client without replacing the whole TLS stack with raw OpenSSL/BoringSSL.
 *
 * Binding to both peers' long-term Ed25519 public keys preserves the
 * property that actually matters. A man-in-the-middle cannot forge either
 * side's key, so it must terminate TLS twice and present its *own* key to
 * each side:
 *
 *     Alice sees (A, M)  ->  code_A = H(A, M, nonces)
 *     Bob   sees (M, B)  ->  code_B = H(M, B, nonces)
 *
 * code_A != code_B, so the on-screen comparison fails and pairing aborts.
 * This is the same construction KDE Connect and Signal safety numbers use.
 *
 * The inputs are ordered deterministically (initiator first, then
 * responder) so both sides derive an identical string without needing to
 * agree on who sorted what.
 *
 * WHY THE NONCE IS COMMITTED TO BEFORE IT IS REVEALED
 * ---------------------------------------------------
 * The paragraph above is only true while neither side can CHOOSE its nonce
 * after seeing the other's. Without that, the argument fails completely, and
 * it failed here: the exchange was one round (initiator sends its nonce,
 * responder answers with its own), so a man-in-the-middle running both halves
 * could finish the Bob side first, fixing code_B, and then search its own
 * nonce until code_A came out equal. Six digits is a search space of 10^6 —
 * under a second, in plain Python, at which point both humans see the same
 * number and confirm.
 *
 * So pairing is three messages, not two:
 *
 *     initiator -> responder : PairRequest  { commitment = commit(N_i) }
 *     responder -> initiator : PairResponse { nonce = N_r }
 *     initiator -> responder : PairReveal   { nonce = N_i }
 *
 * The responder must pick N_r knowing only a hash of N_i, and the initiator
 * is bound to the N_i it committed to before it ever saw N_r — the responder
 * checks commit(N_i) against what it was sent and aborts on a mismatch.
 * Neither side, and so neither half of a man-in-the-middle, can move its own
 * contribution after learning the other's. This is the standard SAS
 * commitment round (ZRTP's hash commitment, the same idea as Signal's).
 */
class Sas {
public:
    /// Number of decimal digits shown to the user.
    static constexpr int kDigits = 6;

    /// Size of each side's pairing nonce.
    static constexpr int kNonceSize = 32;

    /// Size of the commitment (SHA-256 output).
    static constexpr int kCommitSize = 32;

    /// Fresh cryptographically-random nonce for one pairing attempt.
    static QByteArray generateNonce();

    /**
     * Binding commitment to a nonce, sent before the nonce itself.
     *
     * SHA-256 over its own context string, so this hash can never collide
     * with the SAS hash computed over the same nonce. Returns empty for a
     * wrong-sized nonce; callers must treat empty as a hard failure.
     *
     * Hiding is what stops the responder steering its answer, and binding is
     * what stops the initiator swapping its nonce after seeing the response.
     * A 32-byte random pre-image gives both against SHA-256.
     */
    static QByteArray commit(const QByteArray &nonce);

    /**
     * Constant-time check that @p nonce is the one @p commitment named.
     *
     * Constant time because a mismatch here means an active attacker is
     * present, and that is the last moment to avoid telling them how much of
     * their guess was right.
     */
    static bool verifyCommitment(const QByteArray &commitment, const QByteArray &nonce);

    /**
     * Derive the shared verification code.
     *
     * @param initiatorPublicKey  raw Ed25519 key of the side that opened the
     *                            connection
     * @param responderPublicKey  raw Ed25519 key of the side that accepted it
     * @param initiatorNonce      the initiator's nonce
     * @param responderNonce      the responder's nonce
     *
     * Returns a zero-padded 6-digit string, or an empty string if any input
     * is missing or the wrong size — callers must treat empty as a hard
     * failure and abort pairing rather than displaying anything.
     */
    static QString derive(const QByteArray &initiatorPublicKey,
                          const QByteArray &responderPublicKey,
                          const QByteArray &initiatorNonce,
                          const QByteArray &responderNonce);
};

} // namespace mazeconnect::core
