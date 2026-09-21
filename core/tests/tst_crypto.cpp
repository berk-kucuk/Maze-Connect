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
    void commitmentBindsTheNonce();
    void commitmentStopsAGrindingManInTheMiddle();
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

void TestCrypto::commitmentBindsTheNonce() {
    const QByteArray nonce = Sas::generateNonce();
    const QByteArray other = Sas::generateNonce();
    const QByteArray commitment = Sas::commit(nonce);

    QCOMPARE(commitment.size(), qsizetype(Sas::kCommitSize));
    QVERIFY(Sas::verifyCommitment(commitment, nonce));
    // Binding: no other nonce opens this commitment.
    QVERIFY(!Sas::verifyCommitment(commitment, other));
    // Deterministic, so both clients compute the same value.
    QCOMPARE(Sas::commit(nonce), commitment);
    // Domain-separated from the SAS hash over the same material.
    QVERIFY(Sas::commit(nonce) != Sas::commit(other));

    // Malformed input is a hard failure, never a commitment to nothing.
    QVERIFY(Sas::commit(QByteArray()).isEmpty());
    QVERIFY(Sas::commit(QByteArray(Sas::kNonceSize - 1, 'x')).isEmpty());
    QVERIFY(!Sas::verifyCommitment(QByteArray(), nonce));
    QVERIFY(!Sas::verifyCommitment(commitment, QByteArray()));
}

void TestCrypto::commitmentStopsAGrindingManInTheMiddle() {
    // sasDetectsManInTheMiddle above only covers a PASSIVE relay — one that
    // picks its nonces at random and hopes. That is not the attack.
    //
    // The real one: Mallory runs both halves, finishes the Bob side first so
    // code_B is fixed, then searches its OWN nonce until the Alice side comes
    // out equal. The space is 10^6, so it lands in well under a second, both
    // humans see the same six digits, and both confirm. Nothing about the
    // key-binding argument prevents it — what prevents it is having to be
    // committed to a nonce before seeing the other side's.
    //
    // This test performs that search and asserts the commitment is what
    // refuses the result. Capped so a run cannot hang; the cap is far above
    // the ~10^6 expected trials only because failing to find a collision
    // would make the test pass vacuously, and the QVERIFY below catches that.
    const Identity alice = Identity::generate(QStringLiteral("alice"));
    const Identity bob = Identity::generate(QStringLiteral("bob"));
    const Identity mToAlice = Identity::generate(QStringLiteral("mallory-a"));
    const Identity mToBob = Identity::generate(QStringLiteral("mallory-b"));

    // ---- Bob's half, completed first: Mallory is the initiator there ------
    const QByteArray nMalloryToBob = Sas::generateNonce();
    const QByteArray nBob = Sas::generateNonce();
    const QString codeBob =
        Sas::derive(mToBob.publicKey(), bob.publicKey(), nMalloryToBob, nBob);
    QCOMPARE(codeBob.size(), Sas::kDigits);

    // ---- Alice's half: Mallory is the responder, and grinds --------------
    const QByteArray nAlice = Sas::generateNonce();
    QByteArray forged;
    bool found = false;
    for (int attempt = 0; attempt < 40'000'000 && !found; ++attempt) {
        const QByteArray candidate = Sas::generateNonce();
        if (Sas::derive(alice.publicKey(), mToAlice.publicKey(), nAlice, candidate) == codeBob) {
            forged = candidate;
            found = true;
        }
    }
    QVERIFY2(found, "could not grind a colliding nonce - test would be vacuous");

    // The grind works: this is exactly what both users would have seen.
    QCOMPARE(Sas::derive(alice.publicKey(), mToAlice.publicKey(), nAlice, forged), codeBob);

    // And this is why it no longer helps. Mallory had to send its commitment
    // to Bob before Bob's nonce existed, so the nonce it wants to use now is
    // not the one it is bound to, and Bob's side aborts on the mismatch.
    const QByteArray committed = Sas::commit(nMalloryToBob);
    QVERIFY(!Sas::verifyCommitment(committed, forged));
    QVERIFY(Sas::verifyCommitment(committed, nMalloryToBob));
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
