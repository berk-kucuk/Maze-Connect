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
 */
class Sas {
public:
    /// Number of decimal digits shown to the user.
    static constexpr int kDigits = 6;

    /// Size of each side's pairing nonce.
    static constexpr int kNonceSize = 32;

    /// Fresh cryptographically-random nonce for one pairing attempt.
    static QByteArray generateNonce();

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
