#include "mazeconnect/core/Identity.h"

#include <QCryptographicHash>

#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <memory>

namespace mazeconnect::core {
namespace {

template <typename T, void (*Free)(T *)>
struct Deleter {
    void operator()(T *p) const { Free(p); }
};

using EvpPkey = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
using EvpPkeyCtx = std::unique_ptr<EVP_PKEY_CTX, Deleter<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;
using X509Ptr = std::unique_ptr<X509, Deleter<X509, X509_free>>;
using BioPtr = std::unique_ptr<BIO, Deleter<BIO, BIO_free_all>>;
using BigNumPtr = std::unique_ptr<BIGNUM, Deleter<BIGNUM, BN_free>>;

QByteArray bioToByteArray(BIO *bio) {
    char *data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    if (len <= 0 || data == nullptr) {
        return {};
    }
    return QByteArray(data, static_cast<int>(len));
}

/// True only for an EC key on the P-256 curve. Any other curve or key type
/// is outside this protocol and is refused rather than accommodated.
bool isP256(EVP_PKEY *pkey) {
    if (pkey == nullptr || EVP_PKEY_base_id(pkey) != EVP_PKEY_EC) {
        return false;
    }
    char groupName[64] = {};
    size_t groupLen = 0;
    if (EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_GROUP_NAME, groupName,
                                       sizeof(groupName), &groupLen)
        != 1) {
        return false;
    }
    const QByteArray name(groupName, static_cast<int>(groupLen));
    return name == "prime256v1" || name == "P-256" || name == "secp256r1";
}

/// SubjectPublicKeyInfo DER — the same bytes Java's PublicKey.getEncoded()
/// produces, which is what makes fingerprints match across the two clients.
QByteArray spkiDer(EVP_PKEY *pkey) {
    if (!isP256(pkey)) {
        return {};
    }
    unsigned char *buffer = nullptr;
    const int len = i2d_PUBKEY(pkey, &buffer);
    if (len <= 0 || buffer == nullptr) {
        return {};
    }
    QByteArray out(reinterpret_cast<const char *>(buffer), len);
    OPENSSL_free(buffer);
    return out;
}

} // namespace

bool Identity::isPlausiblePublicKey(const QByteArray &publicKey) {
    return publicKey.size() >= kMinPublicKeySize && publicKey.size() <= kMaxPublicKeySize;
}

Identity Identity::generate(const QString &deviceName) {
    Identity identity;

    EvpPkeyCtx ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
    if (!ctx || EVP_PKEY_keygen_init(ctx.get()) != 1
        || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_X9_62_prime256v1) != 1) {
        return {};
    }
    EVP_PKEY *rawKey = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &rawKey) != 1) {
        return {};
    }
    EvpPkey pkey(rawKey);

    X509Ptr cert(X509_new());
    if (!cert || X509_set_version(cert.get(), 2) != 1) { // v3
        return {};
    }

    // Random 64-bit serial. Nothing depends on it — the pinned key is the
    // identity — but a constant serial would be needlessly odd on the wire.
    unsigned char serialBytes[8] = {};
    if (RAND_bytes(serialBytes, sizeof(serialBytes)) != 1) {
        return {};
    }
    serialBytes[0] &= 0x7F; // keep it positive
    BigNumPtr serial(BN_bin2bn(serialBytes, sizeof(serialBytes), nullptr));
    if (!serial || BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(cert.get())) == nullptr) {
        return {};
    }

    // Validity is deliberately long and deliberately not load-bearing.
    // Verification never consults these dates: an expiry-driven re-pair
    // would train users to re-accept fingerprints, which is exactly the
    // habit this protocol must not build.
    if (X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) == nullptr
        || X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60 * 24 * 365 * 20) == nullptr) {
        return {};
    }

    if (X509_set_pubkey(cert.get(), pkey.get()) != 1) {
        return {};
    }

    // Subject == issuer (self-signed). The CN is cosmetic and trusted
    // nowhere; it is bounded only so a hostile local name cannot bloat the
    // certificate.
    X509_NAME *name = X509_get_subject_name(cert.get());
    const QByteArray cn = deviceName.left(64).toUtf8();
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                                   reinterpret_cast<const unsigned char *>(cn.constData()),
                                   cn.size(), -1, 0)
            != 1
        || X509_set_issuer_name(cert.get(), name) != 1) {
        return {};
    }

    if (X509_sign(cert.get(), pkey.get(), EVP_sha256()) == 0) {
        return {};
    }

    BioPtr keyBio(BIO_new(BIO_s_mem()));
    BioPtr certBio(BIO_new(BIO_s_mem()));
    if (!keyBio || !certBio) {
        return {};
    }
    if (PEM_write_bio_PKCS8PrivateKey(keyBio.get(), pkey.get(), nullptr, nullptr, 0, nullptr,
                                      nullptr)
            != 1
        || PEM_write_bio_X509(certBio.get(), cert.get()) != 1) {
        return {};
    }

    identity.m_privateKeyPem = bioToByteArray(keyBio.get());
    identity.m_certificatePem = bioToByteArray(certBio.get());
    identity.m_publicKey = spkiDer(pkey.get());

    if (identity.m_publicKey.isEmpty() || identity.m_privateKeyPem.isEmpty()
        || identity.m_certificatePem.isEmpty()) {
        return {};
    }
    return identity;
}

Identity Identity::fromPem(const QByteArray &privateKeyPem, const QByteArray &certificatePem) {
    if (privateKeyPem.isEmpty() || certificatePem.isEmpty()) {
        return {};
    }

    BioPtr keyBio(BIO_new_mem_buf(privateKeyPem.constData(), privateKeyPem.size()));
    BioPtr certBio(BIO_new_mem_buf(certificatePem.constData(), certificatePem.size()));
    if (!keyBio || !certBio) {
        return {};
    }

    EvpPkey pkey(PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr));
    X509Ptr cert(PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr));
    if (!pkey || !cert || !isP256(pkey.get())) {
        return {};
    }

    // The stored key and certificate must belong together; a mismatched
    // pair would mean presenting a certificate we cannot sign for.
    if (X509_check_private_key(cert.get(), pkey.get()) != 1) {
        return {};
    }

    Identity identity;
    identity.m_privateKeyPem = privateKeyPem;
    identity.m_certificatePem = certificatePem;
    identity.m_publicKey = spkiDer(pkey.get());
    if (identity.m_publicKey.isEmpty()) {
        return {};
    }
    return identity;
}

QByteArray Identity::fingerprintOf(const QByteArray &publicKey) {
    if (publicKey.isEmpty()) {
        return {};
    }
    return QCryptographicHash::hash(publicKey, QCryptographicHash::Sha256);
}

QString Identity::fingerprint() const {
    const QByteArray digest = fingerprintOf(m_publicKey);
    if (digest.isEmpty()) {
        return {};
    }
    return QString::fromLatin1(digest.toHex());
}

QByteArray Identity::publicKeyFromCertificateDer(const QByteArray &der) {
    if (der.isEmpty()) {
        return {};
    }
    const unsigned char *p = reinterpret_cast<const unsigned char *>(der.constData());
    X509Ptr cert(d2i_X509(nullptr, &p, der.size()));
    if (!cert) {
        return {};
    }
    EvpPkey pkey(X509_get_pubkey(cert.get()));
    return spkiDer(pkey.get());
}

} // namespace mazeconnect::core
