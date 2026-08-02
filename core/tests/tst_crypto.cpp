#include <QtTest>

#include "mazeconnect/core/Identity.h"
#include "mazeconnect/core/Sas.h"

using namespace mazeconnect::core;

class TestCrypto : public QObject {
    Q_OBJECT

private slots:
    void generatesDistinctIdentities();
    void identitySurvivesPemRoundTrip();
    void rejectsMismatchedPem();
    void fingerprintIsStableAndKeyDerived();
    void sasIsDeterministicAndSymmetric();
    void sasDiffersForDifferentPeers();
    void sasDetectsManInTheMiddle();
    void sasRejectsMalformedInput();
    void nonceIsRandom();
    void matchesCrossPlatformKnownAnswer();
};

void TestCrypto::generatesDistinctIdentities() {
    const Identity a = Identity::generate(QStringLiteral("device-a"));
    const Identity b = Identity::generate(QStringLiteral("device-b"));

    QVERIFY(a.isValid());
    QVERIFY(b.isValid());
    QVERIFY(Identity::isPlausiblePublicKey(a.publicKey()));
    QVERIFY(a.publicKey() != b.publicKey());
    QVERIFY(a.fingerprint() != b.fingerprint());
    QVERIFY(a.certificatePem().startsWith("-----BEGIN CERTIFICATE-----"));
}

void TestCrypto::identitySurvivesPemRoundTrip() {
    const Identity original = Identity::generate(QStringLiteral("round-trip"));
    QVERIFY(original.isValid());

    const Identity restored = Identity::fromPem(original.privateKeyPem(), original.certificatePem());
    QVERIFY(restored.isValid());
    QCOMPARE(restored.publicKey(), original.publicKey());
    QCOMPARE(restored.fingerprint(), original.fingerprint());
}

void TestCrypto::rejectsMismatchedPem() {
    const Identity a = Identity::generate(QStringLiteral("a"));
    const Identity b = Identity::generate(QStringLiteral("b"));

    // a's key with b's certificate must not load: we would be presenting a
    // certificate we cannot sign for.
    const Identity frankenstein = Identity::fromPem(a.privateKeyPem(), b.certificatePem());
    QVERIFY(!frankenstein.isValid());

    QVERIFY(!Identity::fromPem(QByteArray(), a.certificatePem()).isValid());
    QVERIFY(!Identity::fromPem(a.privateKeyPem(), QByteArray()).isValid());
    QVERIFY(!Identity::fromPem(QByteArrayLiteral("not a pem"), a.certificatePem()).isValid());
}

void TestCrypto::fingerprintIsStableAndKeyDerived() {
    const Identity a = Identity::generate(QStringLiteral("a"));
    // Fingerprint is over the raw public key, so it is reproducible from the
    // key alone — this is what makes it survive certificate regeneration.
    QCOMPARE(a.fingerprint(),
             QString::fromLatin1(Identity::fingerprintOf(a.publicKey()).toHex()));
    QCOMPARE(a.fingerprint().size(), qsizetype(64)); // SHA-256 hex
}

void TestCrypto::sasIsDeterministicAndSymmetric() {
    const Identity alice = Identity::generate(QStringLiteral("alice"));
    const Identity bob = Identity::generate(QStringLiteral("bob"));
    const QByteArray na = Sas::generateNonce();
    const QByteArray nb = Sas::generateNonce();

    // Both sides feed the same ordered inputs (initiator first) and must
    // arrive at the same code.
    const QString onAlice = Sas::derive(alice.publicKey(), bob.publicKey(), na, nb);
    const QString onBob = Sas::derive(alice.publicKey(), bob.publicKey(), na, nb);

    QCOMPARE(onAlice.size(), Sas::kDigits);
    QCOMPARE(onAlice, onBob);
}

void TestCrypto::sasDiffersForDifferentPeers() {
    const Identity alice = Identity::generate(QStringLiteral("alice"));
    const Identity bob = Identity::generate(QStringLiteral("bob"));
    const QByteArray na = Sas::generateNonce();
    const QByteArray nb = Sas::generateNonce();

    // Swapping the roles changes the code — ordering is load-bearing.
    QVERIFY(Sas::derive(alice.publicKey(), bob.publicKey(), na, nb)
            != Sas::derive(bob.publicKey(), alice.publicKey(), na, nb));

    // A different nonce changes the code, so a captured code cannot be
    // reused for a later pairing attempt.
    QVERIFY(Sas::derive(alice.publicKey(), bob.publicKey(), na, nb)
            != Sas::derive(alice.publicKey(), bob.publicKey(), na, Sas::generateNonce()));
}

void TestCrypto::sasDetectsManInTheMiddle() {
    // The property the whole pairing flow rests on: an attacker relaying
    // two TLS sessions must present its own key to each side, so the two
    // screens show different codes.
    const Identity alice = Identity::generate(QStringLiteral("alice"));
    const Identity bob = Identity::generate(QStringLiteral("bob"));
    const Identity mallory = Identity::generate(QStringLiteral("mallory"));

    const QByteArray na = Sas::generateNonce();
    const QByteArray nb = Sas::generateNonce();

    // Alice believes she is talking to Bob, but sees Mallory's key.
    const QString shownToAlice = Sas::derive(alice.publicKey(), mallory.publicKey(), na, nb);
    // Bob sees Mallory as the initiator.
    const QString shownToBob = Sas::derive(mallory.publicKey(), bob.publicKey(), na, nb);

    QVERIFY(!shownToAlice.isEmpty());
    QVERIFY(!shownToBob.isEmpty());
    QVERIFY2(shownToAlice != shownToBob,
             "MITM produced matching codes on both devices — pairing would succeed");
}

void TestCrypto::sasRejectsMalformedInput() {
    const Identity a = Identity::generate(QStringLiteral("a"));
    const Identity b = Identity::generate(QStringLiteral("b"));
    const QByteArray n = Sas::generateNonce();

    QVERIFY(Sas::derive(QByteArray(), b.publicKey(), n, n).isEmpty());
    QVERIFY(Sas::derive(a.publicKey(), QByteArray(), n, n).isEmpty());
    QVERIFY(Sas::derive(a.publicKey(), b.publicKey(), QByteArray(), n).isEmpty());
    QVERIFY(Sas::derive(a.publicKey(), b.publicKey(), n, QByteArray(16, 'x')).isEmpty());
    QVERIFY(Sas::derive(QByteArray(8, 'k'), b.publicKey(), n, n).isEmpty());

    // A peer echoing our own key back must not yield a trivially matching code.
    QVERIFY(Sas::derive(a.publicKey(), a.publicKey(), n, n).isEmpty());
}

void TestCrypto::nonceIsRandom() {
    const QByteArray a = Sas::generateNonce();
    const QByteArray b = Sas::generateNonce();
    QCOMPARE(a.size(), qsizetype(Sas::kNonceSize));
    QVERIFY(a != b);
    QVERIFY(!a.isEmpty());
}

/**
 * Pins the exact digits produced for a fixed input.
 *
 * This is the cross-platform contract with Maze-Connect-Mobile: the Kotlin
 * implementation must produce the same six digits for these same bytes
 * (see SasTest.matchesKnownAnswerFromSharedConstruction). If either side's
 * derivation drifts, this fails here rather than the two clients silently
 * failing to pair in the field.
 */
void TestCrypto::matchesCrossPlatformKnownAnswer() {
    QByteArray a(91, Qt::Uninitialized);
    QByteArray b(91, Qt::Uninitialized);
    for (int i = 0; i < 91; ++i) {
        a[i] = static_cast<char>(i);
        b[i] = static_cast<char>(i + 100);
    }
    const QByteArray na(Sas::kNonceSize, char(1));
    const QByteArray nb(Sas::kNonceSize, char(2));

    QCOMPARE(Sas::derive(a, b, na, nb), QStringLiteral("876154"));
}

QTEST_MAIN(TestCrypto)
#include "tst_crypto.moc"
