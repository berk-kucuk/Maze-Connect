#include "mazeconnect/core/Sas.h"

#include "mazeconnect/core/Identity.h"

#include <QCryptographicHash>

#include <openssl/rand.h>

namespace mazeconnect::core {
namespace {

// Domain separation: this hash input must never collide with any other
// hash the protocol computes over the same key material.
constexpr char kContext[] = "maze-connect/sas/v1";

// Public keys are SubjectPublicKeyInfo DER (see Identity). Bounds-checked
// rather than fixed-size so a future curve change cannot silently pass a
// malformed key, while still refusing anything that plainly did not come
// out of Identity.

void appendLengthPrefixed(QCryptographicHash &hash, const QByteArray &field) {
    // Length-prefix every field so that concatenation is unambiguous —
    // otherwise (A="ab", B="c") and (A="a", B="bc") would hash identically.
    const quint32 len = static_cast<quint32>(field.size());
    const char lenBytes[4] = {
        static_cast<char>((len >> 24) & 0xFF),
        static_cast<char>((len >> 16) & 0xFF),
        static_cast<char>((len >> 8) & 0xFF),
        static_cast<char>(len & 0xFF),
    };
    hash.addData(QByteArrayView(lenBytes, 4));
    hash.addData(field);
}

} // namespace

QByteArray Sas::generateNonce() {
    QByteArray nonce(kNonceSize, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), kNonceSize) != 1) {
        // Never silently downgrade to a weaker source: a nonce we cannot
        // generate securely must fail pairing, not weaken it.
        return {};
    }
    return nonce;
}

QString Sas::derive(const QByteArray &initiatorPublicKey,
                    const QByteArray &responderPublicKey,
                    const QByteArray &initiatorNonce,
                    const QByteArray &responderNonce) {
    if (!Identity::isPlausiblePublicKey(initiatorPublicKey)
        || !Identity::isPlausiblePublicKey(responderPublicKey)
        || initiatorNonce.size() != kNonceSize
        || responderNonce.size() != kNonceSize) {
        return {};
    }

    // A peer that echoes our own key back would otherwise produce a code
    // that trivially matches on both screens. Two distinct devices always
    // have distinct keys, so equality here means something is wrong.
    if (initiatorPublicKey == responderPublicKey) {
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(kContext, static_cast<qsizetype>(sizeof(kContext) - 1)));
    appendLengthPrefixed(hash, initiatorPublicKey);
    appendLengthPrefixed(hash, responderPublicKey);
    appendLengthPrefixed(hash, initiatorNonce);
    appendLengthPrefixed(hash, responderNonce);

    const QByteArray digest = hash.result();
    if (digest.size() < 4) {
        return {};
    }

    // Take the leading 31 bits (masking the sign bit) and reduce mod 10^6.
    // The modulo bias across a 2^31 range is well under one part in 2000,
    // which is irrelevant for a code whose security comes from the human
    // comparison rather than from its own entropy.
    const quint32 value = ((static_cast<quint32>(static_cast<quint8>(digest[0])) << 24)
                           | (static_cast<quint32>(static_cast<quint8>(digest[1])) << 16)
                           | (static_cast<quint32>(static_cast<quint8>(digest[2])) << 8)
                           | static_cast<quint32>(static_cast<quint8>(digest[3])))
                          & 0x7FFFFFFFu;

    return QString::number(value % 1000000u).rightJustified(kDigits, u'0');
}

} // namespace mazeconnect::core
