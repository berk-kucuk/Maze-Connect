/*
 * The parts of pam_mazeconnect that read untrusted input, on their own.
 * No framework: each check prints what failed and the exit status is the
 * number of failures.
 */
#include "approval_core.h"

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static void hex(const unsigned char *in, size_t len, char *out) {
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; ++i) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * len] = '\0';
}

static void test_vector(void) {
    /* The shared known answer: tst_approval and the phone's
     * ApprovalSigningTest assert the same digest for the same challenge. */
    unsigned char nonce[MC_NONCE_SIZE];
    unsigned char fp[MC_FP_SIZE];
    unsigned char input[MC_MAX_INPUT];
    unsigned char digest[32];
    unsigned int digest_len = 0;
    char digest_hex[65];
    mc_challenge c;
    int len;
    int i;

    for (i = 0; i < MC_NONCE_SIZE; ++i) nonce[i] = (unsigned char)i;
    memset(fp, 0xAA, sizeof(fp));
    c.purpose = "sudo";
    c.host = "maze";
    c.user = "berk";
    c.detail = "sudo \xC2\xB7 pts/1";
    c.nonce = nonce;
    c.code = "47";
    c.issued_at = 1790000000LL;
    len = mc_signing_input(&c, fp, input, sizeof(input));
    CHECK(len > 0);
    EVP_Digest(input, (size_t)len, digest, &digest_len, EVP_sha256(), NULL);
    hex(digest, 32, digest_hex);
    CHECK(strcmp(digest_hex,
                 "ecb446382e92746576b13cc8937fdee33d74a74dfb35f6d3d2f8257b3cf7b6c7") == 0);

    /* Never past the buffer. */
    CHECK(mc_signing_input(&c, fp, input, 40) == -1);
    CHECK(mc_signing_input(&c, fp, input, (size_t)len - 1) == -1);
    CHECK(mc_signing_input(&c, fp, input, (size_t)len) == len);
}

static void test_base64(void) {
    unsigned char out[16];
    char enc[32];
    CHECK(mc_b64decode("aGk=", 4, out, sizeof(out)) == 2 && memcmp(out, "hi", 2) == 0);
    CHECK(mc_b64decode("aGl0", 4, out, sizeof(out)) == 3);
    CHECK(mc_b64decode("aA==", 4, out, sizeof(out)) == 1 && out[0] == 'h');
    /* Malformed: wrong length, bad characters, padding in the middle,
     * whitespace, non-canonical trailing bits, too long for the buffer. */
    CHECK(mc_b64decode("aGk", 3, out, sizeof(out)) == -1);
    CHECK(mc_b64decode("aG!=", 4, out, sizeof(out)) == -1);
    CHECK(mc_b64decode("a=k=", 4, out, sizeof(out)) == -1);
    CHECK(mc_b64decode("aA==aGk=", 8, out, sizeof(out)) == -1);
    CHECK(mc_b64decode("aG k", 4, out, sizeof(out)) == -1);
    CHECK(mc_b64decode("aGl=", 4, out, sizeof(out)) == -1);
    CHECK(mc_b64decode("aB==", 4, out, sizeof(out)) == -1);
    CHECK(mc_b64decode("===", 3, out, sizeof(out)) == -1);
    CHECK(mc_b64decode("====", 4, out, sizeof(out)) == -1);
    CHECK(mc_b64decode("aGl0aGl0", 8, out, 5) == -1);
    CHECK(mc_b64encode((const unsigned char *)"hi", 2, enc, sizeof(enc)) == 4
          && strcmp(enc, "aGk=") == 0);
    CHECK(mc_b64encode((const unsigned char *)"hit", 3, enc, 4) == -1);
}

static void test_answer(void) {
    unsigned char sig[MC_MAX_SIG];
    size_t sig_len = 0;
    unsigned char fp[MC_FP_SIZE];
    char reason[32];
    char line[256];
    char sig_b64[128];
    char fp_b64[64];
    unsigned char raw_sig[40];
    unsigned char raw_fp[MC_FP_SIZE];

    memset(raw_sig, 0x30, sizeof(raw_sig));
    memset(raw_fp, 0x11, sizeof(raw_fp));
    mc_b64encode(raw_sig, sizeof(raw_sig), sig_b64, sizeof(sig_b64));
    mc_b64encode(raw_fp, sizeof(raw_fp), fp_b64, sizeof(fp_b64));

    snprintf(line, sizeof(line), "OK %s %s", sig_b64, fp_b64);
    CHECK(mc_parse_answer(line, sig, &sig_len, fp, reason, sizeof(reason)) == 1);
    CHECK(sig_len == sizeof(raw_sig) && memcmp(sig, raw_sig, sig_len) == 0);
    CHECK(memcmp(fp, raw_fp, MC_FP_SIZE) == 0);

    /* Extra fields, a short verifier, a missing one, a lowercase verb. */
    snprintf(line, sizeof(line), "OK %s %s extra", sig_b64, fp_b64);
    CHECK(mc_parse_answer(line, sig, &sig_len, fp, reason, sizeof(reason)) == -1);
    snprintf(line, sizeof(line), "OK %s aGk=", sig_b64);
    CHECK(mc_parse_answer(line, sig, &sig_len, fp, reason, sizeof(reason)) == -1);
    snprintf(line, sizeof(line), "OK %s", sig_b64);
    CHECK(mc_parse_answer(line, sig, &sig_len, fp, reason, sizeof(reason)) == -1);
    snprintf(line, sizeof(line), "ok %s %s", sig_b64, fp_b64);
    CHECK(mc_parse_answer(line, sig, &sig_len, fp, reason, sizeof(reason)) == -1);
    CHECK(mc_parse_answer("", sig, &sig_len, fp, reason, sizeof(reason)) == -1);

    /* A signature too long to be one is refused before it is copied. */
    {
        unsigned char big[100];
        char big_b64[160];
        memset(big, 1, sizeof(big));
        mc_b64encode(big, sizeof(big), big_b64, sizeof(big_b64));
        snprintf(line, sizeof(line), "OK %s %s", big_b64, fp_b64);
        CHECK(mc_parse_answer(line, sig, &sig_len, fp, reason, sizeof(reason)) == -1);
    }

    /* An error line comes back as printable ASCII, cut to the buffer. */
    CHECK(mc_parse_answer("ERR no \x1b[2Jphone that can approve is connected", sig, &sig_len,
                          fp, reason, sizeof(reason))
          == 0);
    CHECK(strchr(reason, 0x1b) == NULL);
    CHECK(strlen(reason) == sizeof(reason) - 1);
}

static EVP_PKEY *gen(const char *curve, char *b64, size_t cap) {
    EVP_PKEY *key = EVP_EC_gen(curve);
    unsigned char *der = NULL;
    const int len = i2d_PUBKEY(key, &der);
    mc_b64encode(der, (size_t)len, b64, cap);
    OPENSSL_free(der);
    return key;
}

static void test_approvers_and_verify(void) {
    char a_b64[200];
    char b_b64[200];
    char big_b64[300];
    char file[2048];
    mc_key keys[MC_MAX_KEYS];
    EVP_PKEY *a = gen("P-256", a_b64, sizeof(a_b64));
    EVP_PKEY *b = gen("P-256", b_b64, sizeof(b_b64));
    EVP_PKEY *big = gen("P-384", big_b64, sizeof(big_b64));
    int n;

    snprintf(file, sizeof(file),
             "# comment\n"
             "%s Pixel \x1b[31mred\n"
             "not-base64 nope\n"
             "%s\n"
             "%s P-384 is not accepted\n",
             a_b64, b_b64, big_b64);
    memset(keys, 0, sizeof(keys));
    n = mc_parse_approvers(file, strlen(file), keys, MC_MAX_KEYS);
    CHECK(n == 2);
    CHECK(strchr(keys[0].label, 0x1b) == NULL);
    CHECK(strncmp(keys[0].label, "Pixel ", 6) == 0);
    CHECK(keys[1].label[0] == '\0');

    /* A signature by `a` verifies with key 0 and not with key 1. */
    {
        const unsigned char msg[] = "challenge";
        unsigned char sig[MC_MAX_SIG];
        size_t sig_len = sizeof(sig);
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, a);
        EVP_DigestSign(ctx, sig, &sig_len, msg, sizeof(msg));
        EVP_MD_CTX_free(ctx);
        CHECK(mc_verify(keys[0].key, msg, sizeof(msg), sig, sig_len) == 1);
        CHECK(mc_verify(keys[1].key, msg, sizeof(msg), sig, sig_len) == 0);
        CHECK(mc_verify(keys[0].key, msg, sizeof(msg) - 1, sig, sig_len) == 0);
        CHECK(mc_verify(keys[0].key, msg, sizeof(msg), sig, 4) == 0);
    }

    /* Never more than asked for. */
    mc_free_keys(keys, n);
    CHECK(mc_parse_approvers(file, strlen(file), keys, 1) == 1);
    mc_free_keys(keys, 1);

    EVP_PKEY_free(a);
    EVP_PKEY_free(b);
    EVP_PKEY_free(big);
}

static void test_names_and_text(void) {
    char out[16];
    CHECK(mc_valid_username("berk"));
    CHECK(mc_valid_username("_svc-1.x"));
    CHECK(!mc_valid_username(""));
    CHECK(!mc_valid_username(".."));
    CHECK(!mc_valid_username("../etc"));
    CHECK(!mc_valid_username("a/b"));
    CHECK(!mc_valid_username("Root"));
    CHECK(!mc_valid_username("-x"));
    CHECK(!mc_valid_username("abcdefghijklmnopqrstuvwxyz0123456"));

    CHECK(mc_json_escape("a\"b\\c", out, sizeof(out)) == 0 && strcmp(out, "a\\\"b\\\\c") == 0);
    CHECK(mc_json_escape("a\nb", out, sizeof(out)) == -1);
    CHECK(mc_json_escape("0123456789abcdef", out, sizeof(out)) == -1);

    mc_printable("ok\x01\xff", out, sizeof(out));
    CHECK(strcmp(out, "ok??") == 0);
}

int main(void) {
    test_vector();
    test_base64();
    test_answer();
    test_approvers_and_verify();
    test_names_and_text();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
    } else {
        printf("all checks passed\n");
    }
    return failures;
}
