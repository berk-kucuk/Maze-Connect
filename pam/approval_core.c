#include "approval_core.h"

#include <stdio.h>
#include <string.h>

#include <openssl/core_names.h>
#include <openssl/x509.h>

static const char kContext[] = "maze-connect/approval/v1";

static int append_lp(unsigned char *out, size_t cap, size_t *pos, const void *field,
                     size_t len) {
    if (len > 0xFFFFFFFFu || *pos > cap || cap - *pos < 4 || cap - *pos - 4 < len) {
        return -1;
    }
    out[(*pos)++] = (unsigned char)((len >> 24) & 0xFF);
    out[(*pos)++] = (unsigned char)((len >> 16) & 0xFF);
    out[(*pos)++] = (unsigned char)((len >> 8) & 0xFF);
    out[(*pos)++] = (unsigned char)(len & 0xFF);
    if (len) {
        memcpy(out + *pos, field, len);
    }
    *pos += len;
    return 0;
}

int mc_signing_input(const mc_challenge *c, const unsigned char fp[MC_FP_SIZE],
                     unsigned char *out, size_t cap) {
    size_t pos = 0;
    char issued[32];
    const size_t ctx = sizeof(kContext) - 1;

    if (!c || !c->purpose || !c->host || !c->user || !c->detail || !c->nonce || !c->code
        || !fp || !out || cap < ctx) {
        return -1;
    }
    if (snprintf(issued, sizeof(issued), "%lld", c->issued_at) <= 0) {
        return -1;
    }
    memcpy(out, kContext, ctx);
    pos = ctx;
    if (append_lp(out, cap, &pos, c->purpose, strlen(c->purpose))
        || append_lp(out, cap, &pos, c->host, strlen(c->host))
        || append_lp(out, cap, &pos, c->user, strlen(c->user))
        || append_lp(out, cap, &pos, c->detail, strlen(c->detail))
        || append_lp(out, cap, &pos, c->nonce, MC_NONCE_SIZE)
        || append_lp(out, cap, &pos, c->code, strlen(c->code))
        || append_lp(out, cap, &pos, issued, strlen(issued))
        || append_lp(out, cap, &pos, fp, MC_FP_SIZE)) {
        return -1;
    }
    return (int)pos;
}

static int b64value(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

int mc_b64decode(const char *in, size_t in_len, unsigned char *out, size_t cap) {
    size_t i;
    size_t o = 0;

    if (!in || !out || in_len == 0 || in_len % 4 != 0) {
        return -1;
    }
    for (i = 0; i < in_len; i += 4) {
        int v[4];
        int pad = 0;
        int k;
        const int last = (i + 4 == in_len);
        for (k = 0; k < 4; ++k) {
            const unsigned char ch = (unsigned char)in[i + k];
            if (ch == '=') {
                /* Padding only in the last quantum, only in its last two
                 * places, and nothing but padding after it. */
                if (!last || k < 2) {
                    return -1;
                }
                v[k] = 0;
                ++pad;
            } else {
                if (pad) {
                    return -1;
                }
                v[k] = b64value(ch);
                if (v[k] < 0) {
                    return -1;
                }
            }
        }
        {
            const unsigned long triple = ((unsigned long)v[0] << 18)
                | ((unsigned long)v[1] << 12) | ((unsigned long)v[2] << 6)
                | (unsigned long)v[3];
            const int bytes = 3 - pad;
            /* Non-canonical encodings (stray low bits under the padding) are
             * refused, so one value has exactly one spelling. */
            if ((pad == 1 && (v[2] & 0x3)) || (pad == 2 && (v[1] & 0xF))) {
                return -1;
            }
            if (o + (size_t)bytes > cap) {
                return -1;
            }
            out[o++] = (unsigned char)((triple >> 16) & 0xFF);
            if (bytes > 1) out[o++] = (unsigned char)((triple >> 8) & 0xFF);
            if (bytes > 2) out[o++] = (unsigned char)(triple & 0xFF);
        }
    }
    return (int)o;
}

int mc_b64encode(const unsigned char *in, size_t len, char *out, size_t cap) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((len + 2) / 3) * 4 + 1;
    size_t i;
    size_t o = 0;

    if (!out || need > cap) {
        return -1;
    }
    for (i = 0; i + 2 < len; i += 3) {
        const unsigned long t = ((unsigned long)in[i] << 16) | ((unsigned long)in[i + 1] << 8)
            | in[i + 2];
        out[o++] = alphabet[(t >> 18) & 63];
        out[o++] = alphabet[(t >> 12) & 63];
        out[o++] = alphabet[(t >> 6) & 63];
        out[o++] = alphabet[t & 63];
    }
    if (len - i == 1) {
        const unsigned long t = (unsigned long)in[i] << 16;
        out[o++] = alphabet[(t >> 18) & 63];
        out[o++] = alphabet[(t >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len - i == 2) {
        const unsigned long t = ((unsigned long)in[i] << 16) | ((unsigned long)in[i + 1] << 8);
        out[o++] = alphabet[(t >> 18) & 63];
        out[o++] = alphabet[(t >> 12) & 63];
        out[o++] = alphabet[(t >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    return (int)o;
}

void mc_printable(const char *in, char *out, size_t cap) {
    size_t i = 0;
    if (!out || cap == 0) {
        return;
    }
    if (in) {
        for (; in[i] && i + 1 < cap; ++i) {
            const unsigned char ch = (unsigned char)in[i];
            out[i] = (ch >= 0x20 && ch <= 0x7E) ? (char)ch : '?';
        }
    }
    out[i] = '\0';
}

int mc_parse_answer(const char *line, unsigned char sig[MC_MAX_SIG], size_t *sig_len,
                    unsigned char fp[MC_FP_SIZE], char *reason, size_t reason_cap) {
    const char *a;
    const char *b;
    int n;
    unsigned char fpbuf[MC_FP_SIZE + 3];

    if (!line || !sig || !sig_len || !fp) {
        return -1;
    }
    if (strncmp(line, "ERR ", 4) == 0) {
        if (reason && reason_cap) {
            mc_printable(line + 4, reason, reason_cap);
        }
        return 0;
    }
    if (strncmp(line, "OK ", 3) != 0) {
        return -1;
    }
    a = line + 3;
    b = strchr(a, ' ');
    if (!b || strchr(b + 1, ' ')) {
        return -1;
    }
    n = mc_b64decode(a, (size_t)(b - a), sig, MC_MAX_SIG);
    if (n < MC_MIN_SIG || n > MC_MAX_SIG) {
        return -1;
    }
    *sig_len = (size_t)n;
    n = mc_b64decode(b + 1, strlen(b + 1), fpbuf, sizeof(fpbuf));
    if (n != MC_FP_SIZE) {
        return -1;
    }
    memcpy(fp, fpbuf, MC_FP_SIZE);
    return 1;
}

EVP_PKEY *mc_parse_p256(const unsigned char *der, size_t len) {
    const unsigned char *cursor = der;
    EVP_PKEY *key;
    char group[64] = {0};
    size_t group_len = 0;

    if (!der || len == 0 || len > 256) {
        return NULL;
    }
    key = d2i_PUBKEY(NULL, &cursor, (long)len);
    if (!key) {
        return NULL;
    }
    if (cursor != der + len || EVP_PKEY_base_id(key) != EVP_PKEY_EC
        || EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group,
                                          sizeof(group), &group_len)
            != 1
        || (strcmp(group, "prime256v1") != 0 && strcmp(group, "P-256") != 0
            && strcmp(group, "secp256r1") != 0)) {
        EVP_PKEY_free(key);
        return NULL;
    }
    return key;
}

int mc_parse_approvers(const char *buf, size_t len, mc_key *keys, int max) {
    size_t pos = 0;
    int count = 0;

    if (!buf || !keys || max <= 0) {
        return 0;
    }
    while (pos < len && count < max) {
        size_t end = pos;
        size_t sp;
        unsigned char der[257];
        int der_len;
        EVP_PKEY *key;

        while (end < len && buf[end] != '\n') {
            ++end;
        }
        /* Lines are short; a long one is not ours and is skipped. */
        if (end - pos == 0 || end - pos > 1024 || buf[pos] == '#') {
            pos = end + 1;
            continue;
        }
        sp = pos;
        while (sp < end && buf[sp] != ' ') {
            ++sp;
        }
        der_len = mc_b64decode(buf + pos, sp - pos, der, sizeof(der));
        key = der_len > 0 ? mc_parse_p256(der, (size_t)der_len) : NULL;
        if (key) {
            char raw[MC_MAX_LABEL];
            size_t label_len = 0;
            if (sp < end) {
                label_len = end - sp - 1;
                if (label_len >= sizeof(raw)) {
                    label_len = sizeof(raw) - 1;
                }
                memcpy(raw, buf + sp + 1, label_len);
            }
            raw[label_len] = '\0';
            keys[count].key = key;
            mc_printable(raw, keys[count].label, sizeof(keys[count].label));
            ++count;
        }
        pos = end + 1;
    }
    return count;
}

void mc_free_keys(mc_key *keys, int count) {
    int i;
    for (i = 0; keys && i < count; ++i) {
        EVP_PKEY_free(keys[i].key);
        keys[i].key = NULL;
    }
}

int mc_verify(EVP_PKEY *key, const unsigned char *input, size_t input_len,
              const unsigned char *sig, size_t sig_len) {
    EVP_MD_CTX *ctx;
    int ok;

    if (!key || !input || !sig || sig_len < MC_MIN_SIG || sig_len > MC_MAX_SIG) {
        return 0;
    }
    ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return 0;
    }
    ok = EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, key) == 1
        && EVP_DigestVerify(ctx, sig, sig_len, input, input_len) == 1;
    EVP_MD_CTX_free(ctx);
    return ok;
}

int mc_valid_username(const char *user) {
    size_t i;
    if (!user || !user[0] || strlen(user) > MC_MAX_USER) {
        return 0;
    }
    if (strcmp(user, ".") == 0 || strcmp(user, "..") == 0) {
        return 0;
    }
    if (!((user[0] >= 'a' && user[0] <= 'z') || user[0] == '_')) {
        return 0;
    }
    for (i = 1; user[i]; ++i) {
        const char ch = user[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.'
              || ch == '-')) {
            return 0;
        }
    }
    return 1;
}

int mc_json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    size_t i;
    if (!in || !out || cap == 0) {
        return -1;
    }
    for (i = 0; in[i]; ++i) {
        const unsigned char ch = (unsigned char)in[i];
        if (ch == '"' || ch == '\\') {
            if (o + 2 >= cap) return -1;
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch < 0x20 || ch == 0x7F) {
            /* Callers never pass these; refusing is safer than encoding. */
            return -1;
        } else {
            if (o + 1 >= cap) return -1;
            out[o++] = (char)ch;
        }
    }
    out[o] = '\0';
    return 0;
}
