/*
 * pam_mazeconnect — the parts that have no PAM in them, so they can be
 * tested on their own.
 *
 * Everything here handles input from a process this module does not trust
 * (the user's Maze Connect relay) or from files it checks before reading.
 * Every function is bounded by the caller's buffer and fails closed.
 */
#ifndef MAZECONNECT_APPROVAL_CORE_H
#define MAZECONNECT_APPROVAL_CORE_H

#include <stddef.h>

#include <openssl/evp.h>

#define MC_NONCE_SIZE 32
#define MC_FP_SIZE 32
#define MC_MIN_SIG 8
#define MC_MAX_SIG 72
#define MC_MAX_KEYS 16
#define MC_MAX_LABEL 64
#define MC_MAX_HOST 64
#define MC_MAX_DETAIL 120
#define MC_MAX_USER 32
/* The signing input is at most the fixed fields plus these bounds. */
#define MC_MAX_INPUT 1024

typedef struct {
    const char *purpose;          /* "sudo" or "polkit" */
    const char *host;
    const char *user;
    const char *detail;
    const unsigned char *nonce;   /* MC_NONCE_SIZE bytes */
    const char *code;             /* the digits shown on the terminal */
    long long issued_at;          /* seconds since the epoch */
} mc_challenge;

typedef struct {
    EVP_PKEY *key;
    char label[MC_MAX_LABEL];
} mc_key;

/*
 * The bytes the phone signs — byte for byte the construction in
 * core/src/crypto/Approval.cpp and the phone's ApprovalSigning.kt:
 *
 *   "maze-connect/approval/v1" || lp(purpose) || lp(host) || lp(user)
 *     || lp(detail) || lp(nonce) || lp(code) || lp(issuedAt decimal)
 *     || lp(verifier fingerprint)
 *
 * Returns the length written, or -1 if it does not fit in @cap.
 */
int mc_signing_input(const mc_challenge *c, const unsigned char fp[MC_FP_SIZE],
                     unsigned char *out, size_t cap);

/*
 * Strict standard base64 (with padding). Returns the decoded length, or -1
 * for anything malformed or longer than @cap.
 */
int mc_b64decode(const char *in, size_t in_len, unsigned char *out, size_t cap);
/* Returns the encoded length (NUL-terminated), or -1 if @cap is too small. */
int mc_b64encode(const unsigned char *in, size_t len, char *out, size_t cap);

/*
 * The relay's answer: "OK <signature> <verifier>" or "ERR <reason>".
 * Returns 1 for OK (filling @sig/@sig_len/@fp), 0 for ERR (filling @reason
 * with printable ASCII), -1 for anything else.
 */
int mc_parse_answer(const char *line, unsigned char sig[MC_MAX_SIG], size_t *sig_len,
                    unsigned char fp[MC_FP_SIZE], char *reason, size_t reason_cap);

/*
 * An approvers file: one "<base64 SPKI> <label>" per line, '#' comments.
 * Only P-256 keys are kept. Returns how many were loaded (0..@max).
 */
int mc_parse_approvers(const char *buf, size_t len, mc_key *keys, int max);
void mc_free_keys(mc_key *keys, int count);

/* 1 if @sig is a valid ECDSA-SHA256 signature by @key over @input. */
int mc_verify(EVP_PKEY *key, const unsigned char *input, size_t input_len,
              const unsigned char *sig, size_t sig_len);

/* A P-256 key from SPKI DER with nothing trailing it, or NULL. */
EVP_PKEY *mc_parse_p256(const unsigned char *der, size_t len);

/*
 * A name that is safe to put in a path: [a-z_][a-z0-9_.-]*, at most
 * MC_MAX_USER characters, not "." or "..".
 */
int mc_valid_username(const char *user);

/* Copy printable ASCII, replacing anything else with '?', NUL-terminated. */
void mc_printable(const char *in, char *out, size_t cap);

/* JSON string body for @in (no quotes added). Returns 0, or -1 if too long. */
int mc_json_escape(const char *in, char *out, size_t cap);

#endif
