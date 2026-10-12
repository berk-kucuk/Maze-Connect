#include "mazeconnect/core/Approval.h"

#include "mazeconnect/core/Limits.h"

#include <QCryptographicHash>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <memory>

namespace mazeconnect::core::approval {
namespace {

constexpr char kContext[] = "maze-connect/approval/v1";

struct PkeyFree {
    void operator()(EVP_PKEY *p) const { EVP_PKEY_free(p); }
};
struct MdCtxFree {
    void operator()(EVP_MD_CTX *p) const { EVP_MD_CTX_free(p); }
};

void appendLengthPrefixed(QByteArray &out, const QByteArray &field) {
    const quint32 len = static_cast<quint32>(field.size());
    out.append(static_cast<char>((len >> 24) & 0xFF));
    out.append(static_cast<char>((len >> 16) & 0xFF));
    out.append(static_cast<char>((len >> 8) & 0xFF));
    out.append(static_cast<char>(len & 0xFF));
    out.append(field);
}

std::unique_ptr<EVP_PKEY, PkeyFree> parseP256(const QByteArray &spkiDer) {
    if (spkiDer.isEmpty() || spkiDer.size() > 256) {
        return nullptr;
    }
    const auto *cursor = reinterpret_cast<const unsigned char *>(spkiDer.constData());
    std::unique_ptr<EVP_PKEY, PkeyFree> key(d2i_PUBKEY(nullptr, &cursor, spkiDer.size()));
    // Trailing bytes after the structure mean this is not the encoding a
    // phone produced; refuse rather than ignore them.
    if (!key || cursor != reinterpret_cast<const unsigned char *>(spkiDer.constData())
                              + spkiDer.size()) {
        return nullptr;
    }
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_EC) {
        return nullptr;
    }
    char group[64] = {};
    size_t groupLen = 0;
    if (EVP_PKEY_get_utf8_string_param(key.get(), OSSL_PKEY_PARAM_GROUP_NAME, group,
                                       sizeof(group), &groupLen)
        != 1) {
        return nullptr;
    }
    const QByteArray name(group, static_cast<qsizetype>(groupLen));
    if (name != "prime256v1" && name != "P-256" && name != "secp256r1") {
        return nullptr;
    }
    return key;
}

} // namespace

QString purposeName(Purpose purpose) {
    switch (purpose) {
    case Purpose::Unlock:
        return QStringLiteral("unlock");
    case Purpose::Sudo:
        return QStringLiteral("sudo");
    case Purpose::Polkit:
        return QStringLiteral("polkit");
    }
    return {};
}

std::optional<Purpose> purposeFromName(const QString &name) {
    if (name == QLatin1StringView("unlock")) {
        return Purpose::Unlock;
    }
    if (name == QLatin1StringView("sudo")) {
        return Purpose::Sudo;
    }
    if (name == QLatin1StringView("polkit")) {
        return Purpose::Polkit;
    }
    return std::nullopt;
}

QByteArray signingInput(const Challenge &challenge, const QByteArray &verifierFingerprint) {
    QByteArray out(kContext, static_cast<qsizetype>(sizeof(kContext) - 1));
    appendLengthPrefixed(out, purposeName(challenge.purpose).toUtf8());
    appendLengthPrefixed(out, challenge.host.toUtf8());
    appendLengthPrefixed(out, challenge.user.toUtf8());
    appendLengthPrefixed(out, challenge.detail.toUtf8());
    appendLengthPrefixed(out, challenge.nonce);
    appendLengthPrefixed(out, challenge.code.toUtf8());
    appendLengthPrefixed(out, QByteArray::number(challenge.issuedAt));
    appendLengthPrefixed(out, verifierFingerprint);
    return out;
}

bool isAcceptableKey(const QByteArray &spkiDer) {
    return parseP256(spkiDer) != nullptr;
}

bool verify(const QByteArray &spkiDer, const QByteArray &input, const QByteArray &derSignature) {
    if (derSignature.size() < limits::kMinApprovalSignature
        || derSignature.size() > limits::kMaxApprovalSignature) {
        return false;
    }
    const auto key = parseP256(spkiDer);
    if (!key) {
        return false;
    }
    std::unique_ptr<EVP_MD_CTX, MdCtxFree> ctx(EVP_MD_CTX_new());
    if (!ctx
        || EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1) {
        return false;
    }
    // 1 is the only success; 0 is a bad signature and anything negative an
    // error, and both are a refusal.
    return EVP_DigestVerify(ctx.get(),
                            reinterpret_cast<const unsigned char *>(derSignature.constData()),
                            static_cast<size_t>(derSignature.size()),
                            reinterpret_cast<const unsigned char *>(input.constData()),
                            static_cast<size_t>(input.size()))
        == 1;
}

QString keyCode(const QByteArray &spkiDer) {
    const QByteArray hex =
        QCryptographicHash::hash(spkiDer, QCryptographicHash::Sha256).left(8).toHex().toUpper();
    return QString::fromLatin1(hex.mid(0, 4) + '-' + hex.mid(4, 4) + '-' + hex.mid(8, 4) + '-'
                               + hex.mid(12, 4));
}

QByteArray newNonce() {
    QByteArray nonce(limits::kApprovalNonceSize, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), nonce.size()) != 1) {
        return {};
    }
    return nonce;
}

QString newCode() {
    // 10..99 by rejection sampling: no modulo bias, and never a leading zero
    // that someone might drop when typing.
    for (;;) {
        unsigned char byte = 0;
        if (RAND_bytes(&byte, 1) != 1) {
            return {};
        }
        if (byte < 180) { // 180 = 2 * 90, the largest multiple of 90 under 256
            return QString::number(10 + byte % 90);
        }
    }
}

} // namespace mazeconnect::core::approval
