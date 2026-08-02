#pragma once

#include <QByteArray>
#include <QString>

namespace mazeconnect::core {

/**
 * This device's long-term identity keypair.
 *
 * WHY EC P-256 AND NOT Ed25519
 * ----------------------------
 * The original design called for Ed25519. Three findings changed it, and
 * the second one is the security-relevant one:
 *
 *  1. Qt 6's QSslKey has no Ed25519 key algorithm (QSsl::KeyAlgorithm is
 *     Rsa/Dsa/Ec/Dh/MlDsa), so an Ed25519 key cannot be handed to
 *     QSslSocket without an opaque-handle escape hatch.
 *  2. Android's Keystore backs EC P-256 with the TEE/StrongBox on
 *     essentially every shipping device, while Ed25519 is not
 *     hardware-backed. Choosing P-256 is what actually lets the mobile
 *     client's private key live in hardware and never enter app memory —
 *     which matters far more here than the curve's own margin.
 *  3. SubjectPublicKeyInfo DER is byte-identical between OpenSSL's
 *     i2d_PUBKEY() and Java's PublicKey.getEncoded(), so both clients
 *     derive identical fingerprints and identical SAS codes from the same
 *     key with no custom encoding to keep in sync.
 *
 * Both curves are far above any practical attack; the deciding factor is
 * that only one of them can be kept in a phone's secure element.
 *
 * The X.509 certificate is a self-signed wrapper around this key. It exists
 * only because Qt's QSslSocket and Android's javax.net.ssl both require an
 * X.509 chain for mutual TLS; it is never validated against a CA. Trust
 * comes exclusively from the pinned public key (see DeviceStore) — never
 * from the certificate's issuer, validity dates, or subject.
 */
class Identity {
public:
    /// Public keys are SubjectPublicKeyInfo DER. For P-256 this is 91
    /// bytes, but the bounds are checked rather than the exact size so a
    /// future curve change does not silently pass a malformed key.
    static constexpr int kMinPublicKeySize = 64;
    static constexpr int kMaxPublicKeySize = 256;

    Identity() = default;

    /// Generate a fresh P-256 identity + matching self-signed certificate.
    static Identity generate(const QString &deviceName);

    /// Rebuild from stored PEM. Returns an invalid Identity if either PEM
    /// fails to parse, the key is not P-256, or the two do not match.
    static Identity fromPem(const QByteArray &privateKeyPem, const QByteArray &certificatePem);

    bool isValid() const { return !m_privateKeyPem.isEmpty() && !m_certificatePem.isEmpty(); }

    QByteArray privateKeyPem() const { return m_privateKeyPem; }
    QByteArray certificatePem() const { return m_certificatePem; }

    /// SubjectPublicKeyInfo DER — the bytes that are actually pinned.
    QByteArray publicKey() const { return m_publicKey; }

    /// Lowercase hex SHA-256 of the SPKI DER. Stable across certificate
    /// regeneration, unlike a certificate digest.
    QString fingerprint() const;

    static QByteArray fingerprintOf(const QByteArray &publicKey);

    /// True if @p publicKey is a plausibly-shaped SPKI DER blob.
    static bool isPlausiblePublicKey(const QByteArray &publicKey);

    /**
     * Extract the SPKI DER public key from a peer's DER certificate.
     * Returns empty if the certificate does not carry an EC key on P-256 —
     * callers must treat that as a hard failure, since every other key type
     * is outside this protocol.
     */
    static QByteArray publicKeyFromCertificateDer(const QByteArray &der);

private:
    QByteArray m_privateKeyPem;
    QByteArray m_certificatePem;
    QByteArray m_publicKey;
};

} // namespace mazeconnect::core
