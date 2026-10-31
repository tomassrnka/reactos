/*
 * PROJECT:     ReactOS crypt32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     PKCS #12 (RFC 7292) reader and writer, independent of the Windows headers
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * Specifications: RFC 7292 (PKCS #12), RFC 8018 (PKCS #5 v2.1), RFC 2268 (RC2), FIPS 180-4.
 * Everything here parses untrusted data: each length is checked against the bytes that are left,
 * recursion and item counts are bounded and key material is wiped after use.
 */

#include <stdlib.h>
#include <string.h>
#include "pkcs12.h"
#include "tomcrypt.h"

/* ------------------------------------------------------------------------------------------ */
/* helpers                                                                                    */
/* ------------------------------------------------------------------------------------------ */

void p12_wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

void p12_free(void *p)
{
    free(p);
}

static void *xmalloc(size_t n)
{
    return malloc(n ? n : 1);
}

static void *xdup(const void *p, size_t n)
{
    void *r = xmalloc(n);
    if (r && n) memcpy(r, p, n);
    return r;
}

typedef struct { const uint8_t *p; size_t n; } span;

struct buf
{
    uint8_t *p;
    size_t   len, cap;
    int      err;
};

static void b_free(struct buf *b)
{
    if (b->p) { p12_wipe(b->p, b->cap); free(b->p); }
    b->p = NULL; b->len = b->cap = 0;
}

static void b_add(struct buf *b, const void *d, size_t n)
{
    if (b->err) return;
    if (n > (size_t)-1 - b->len) { b->err = P12_ERR_NOMEM; return; }
    if (b->len + n > b->cap)
    {
        size_t nc = b->cap ? b->cap : 256;
        uint8_t *np;
        while (nc < b->len + n) nc *= 2;
        if (!(np = xmalloc(nc))) { b->err = P12_ERR_NOMEM; return; }
        if (b->len) memcpy(np, b->p, b->len);
        if (b->p) { p12_wipe(b->p, b->cap); free(b->p); }
        b->p = np; b->cap = nc;
    }
    if (n) memcpy(b->p + b->len, d, n);
    b->len += n;
}

static void b_byte(struct buf *b, uint8_t c) { b_add(b, &c, 1); }

/* ------------------------------------------------------------------------------------------ */
/* BER/DER reading                                                                            */
/* ------------------------------------------------------------------------------------------ */

#define T_BOOLEAN   0x01
#define T_INTEGER   0x02
#define T_OCTETS    0x04
#define T_NULL      0x05
#define T_OID       0x06
#define T_BMP       0x1e
#define T_SEQ       0x30
#define T_SET       0x31
#define T_CTX0      0xa0

struct tlv
{
    uint8_t tag;
    int     cons;
    span    val;
};

static int next_tlv(span *s, struct tlv *t, int depth);

/* length of the content of an indefinite-length element that starts at 'rest' */
static int scan_indefinite(span rest, int depth, size_t *content_len)
{
    size_t off = 0;

    if (depth > P12_MAX_DEPTH) return P12_ERR_LIMIT;
    for (;;)
    {
        span sub;
        struct tlv t;
        int r;

        if (rest.n - off < 2) return P12_ERR_FORMAT;
        if (rest.p[off] == 0 && rest.p[off + 1] == 0) { *content_len = off; return P12_OK; }
        sub.p = rest.p + off; sub.n = rest.n - off;
        if ((r = next_tlv(&sub, &t, depth + 1))) return r;
        off += (rest.n - off) - sub.n;
    }
}

static int next_tlv(span *s, struct tlv *t, int depth)
{
    size_t len, hdr = 2, trailer = 0;
    uint8_t l;

    if (s->n < 2) return P12_ERR_FORMAT;
    t->tag = s->p[0];
    if ((t->tag & 0x1f) == 0x1f) return P12_ERR_FORMAT;
    t->cons = (t->tag & 0x20) != 0;
    l = s->p[1];
    if (l < 0x80) len = l;
    else if (l == 0x80)
    {
        span rest;
        int r;
        if (!t->cons) return P12_ERR_FORMAT;
        rest.p = s->p + 2; rest.n = s->n - 2;
        if ((r = scan_indefinite(rest, depth, &len))) return r;
        trailer = 2;
    }
    else
    {
        unsigned nb = l & 0x7f, i;
        if (nb > 4 || s->n < 2 + nb) return P12_ERR_FORMAT;
        len = 0;
        for (i = 0; i < nb; i++) len = (len << 8) | s->p[2 + i];
        hdr = 2 + nb;
    }
    if (len > s->n - hdr || trailer > s->n - hdr - len) return P12_ERR_FORMAT;
    t->val.p = s->p + hdr;
    t->val.n = len;
    s->p += hdr + len + trailer;
    s->n -= hdr + len + trailer;
    return P12_OK;
}

static int get_tlv(span *s, uint8_t tag, struct tlv *t)
{
    int r = next_tlv(s, t, 0);
    if (r) return r;
    return t->tag == tag ? P12_OK : P12_ERR_FORMAT;
}

static int get_seq(span *s, span *inner)
{
    struct tlv t;
    int r = get_tlv(s, T_SEQ, &t);
    if (!r) *inner = t.val;
    return r;
}

static int oid_is(span v, const uint8_t *oid, size_t n)
{
    return v.n == n && !memcmp(v.p, oid, n);
}
#define OID_IS(v, o) oid_is((v), (o), sizeof(o))

static int get_oid(span *s, span *oid)
{
    struct tlv t;
    int r = get_tlv(s, T_OID, &t);
    if (!r) *oid = t.val;
    return r;
}

/* small non-negative INTEGER */
static int get_uint(span *s, uint32_t *v)
{
    struct tlv t;
    size_t i = 0;
    uint32_t r = 0;
    int ret = get_tlv(s, T_INTEGER, &t);
    if (ret) return ret;
    if (!t.val.n || (t.val.p[0] & 0x80)) return P12_ERR_FORMAT;
    while (i < t.val.n && !t.val.p[i]) i++;
    if (t.val.n - i > 4) return P12_ERR_LIMIT;
    for (; i < t.val.n; i++) r = (r << 8) | t.val.p[i];
    *v = r;
    return P12_OK;
}

/* OCTET STRING, primitive or constructed (BER). *owned is set when the result is a fresh copy. */
static int get_octets_from(const struct tlv *t, span *out, uint8_t **owned, int depth)
{
    *owned = NULL;
    if (!t->cons) { *out = t->val; return P12_OK; }
    else
    {
        struct buf b = {0};
        span in = t->val;
        if (depth > P12_MAX_DEPTH) return P12_ERR_LIMIT;
        while (in.n)
        {
            struct tlv c;
            span part;
            uint8_t *o2;
            int r = next_tlv(&in, &c, depth);
            if (r || c.tag != (c.cons ? 0x24 : T_OCTETS)) { b_free(&b); return r ? r : P12_ERR_FORMAT; }
            if ((r = get_octets_from(&c, &part, &o2, depth + 1))) { b_free(&b); return r; }
            b_add(&b, part.p, part.n);
            free(o2);
        }
        if (b.err) { b_free(&b); return P12_ERR_NOMEM; }
        if (!b.p) { b.p = xmalloc(1); if (!b.p) return P12_ERR_NOMEM; }
        out->p = *owned = b.p; out->n = b.len;
        return P12_OK;
    }
}

static int get_octets(span *s, span *out, uint8_t **owned)
{
    struct tlv t;
    int r = next_tlv(s, &t, 0);
    if (r) return r;
    if (t.tag != T_OCTETS && t.tag != 0x24) return P12_ERR_FORMAT;
    return get_octets_from(&t, out, owned, 0);
}

/* ------------------------------------------------------------------------------------------ */
/* DER writing                                                                                */
/* ------------------------------------------------------------------------------------------ */

static void put_len(struct buf *b, size_t n)
{
    if (n < 0x80) b_byte(b, (uint8_t)n);
    else
    {
        uint8_t tmp[4];
        int i = 0;
        while (n) { tmp[i++] = (uint8_t)n; n >>= 8; }
        b_byte(b, (uint8_t)(0x80 | i));
        while (i) b_byte(b, tmp[--i]);
    }
}

static void put_tlv(struct buf *b, uint8_t tag, const void *d, size_t n)
{
    b_byte(b, tag);
    put_len(b, n);
    b_add(b, d, n);
}

/* wraps and releases 'inner' */
static void put_wrap(struct buf *b, uint8_t tag, struct buf *inner)
{
    if (inner->err) b->err = inner->err;
    else put_tlv(b, tag, inner->p, inner->len);
    b_free(inner);
}

static void put_uint(struct buf *b, uint32_t v)
{
    uint8_t t[5];
    int n = 0, i;
    for (i = 3; i >= 0; i--)
    {
        uint8_t c = (uint8_t)(v >> (8 * i));
        if (n || c) t[n++] = c;
    }
    if (!n) t[n++] = 0;
    if (t[0] & 0x80) { memmove(t + 1, t, n); t[0] = 0; n++; }
    put_tlv(b, T_INTEGER, t, n);
}

/* big-endian magnitude -> INTEGER */
static void put_bignum(struct buf *b, const uint8_t *d, size_t n)
{
    struct buf c = {0};
    while (n > 1 && !*d) { d++; n--; }
    if (!n || (d[0] & 0x80)) b_byte(&c, 0);
    b_add(&c, d, n);
    put_wrap(b, T_INTEGER, &c);
}

/* ------------------------------------------------------------------------------------------ */
/* OIDs (DER content octets)                                                                  */
/* ------------------------------------------------------------------------------------------ */

#define P12OID 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x0c
#define P9OID  0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09
#define P5OID  0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x05
#define P7OID  0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07

static const uint8_t oid_sha1[]       = { 0x2b, 0x0e, 0x03, 0x02, 0x1a };
static const uint8_t oid_sha256[]     = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };
static const uint8_t oid_data[]       = { P7OID, 0x01 };
static const uint8_t oid_encdata[]    = { P7OID, 0x06 };
static const uint8_t oid_pbe_rc4_128[]  = { P12OID, 0x01, 0x01 };
static const uint8_t oid_pbe_rc4_40[]   = { P12OID, 0x01, 0x02 };
static const uint8_t oid_pbe_3des[]     = { P12OID, 0x01, 0x03 };
static const uint8_t oid_pbe_2des[]     = { P12OID, 0x01, 0x04 };
static const uint8_t oid_pbe_rc2_128[]  = { P12OID, 0x01, 0x05 };
static const uint8_t oid_pbe_rc2_40[]   = { P12OID, 0x01, 0x06 };
static const uint8_t oid_keybag[]       = { P12OID, 0x0a, 0x01, 0x01 };
static const uint8_t oid_shroudedbag[]  = { P12OID, 0x0a, 0x01, 0x02 };
static const uint8_t oid_certbag[]      = { P12OID, 0x0a, 0x01, 0x03 };
static const uint8_t oid_safecontbag[]  = { P12OID, 0x0a, 0x01, 0x06 };
static const uint8_t oid_x509cert[]     = { P9OID, 0x16, 0x01 };
static const uint8_t oid_friendlyname[] = { P9OID, 0x14 };
static const uint8_t oid_localkeyid[]   = { P9OID, 0x15 };
static const uint8_t oid_pbes2[]        = { P5OID, 0x0d };
static const uint8_t oid_pbkdf2[]       = { P5OID, 0x0c };
static const uint8_t oid_hmac_sha1[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x07 };
static const uint8_t oid_hmac_sha256[]  = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x09 };
static const uint8_t oid_aes128_cbc[]   = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x02 };
static const uint8_t oid_aes192_cbc[]   = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x16 };
static const uint8_t oid_aes256_cbc[]   = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x2a };
static const uint8_t oid_des_ede3_cbc[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x03, 0x07 };
static const uint8_t oid_rsa[]          = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };

static void put_oid(struct buf *b, const uint8_t *oid, size_t n) { put_tlv(b, T_OID, oid, n); }
#define PUT_OID(b, o) put_oid((b), (o), sizeof(o))

/* ------------------------------------------------------------------------------------------ */
/* SHA-1 / SHA-256, HMAC, PBKDF2, PKCS #12 KDF                                                */
/* ------------------------------------------------------------------------------------------ */

struct hctx
{
    uint32_t h[8];
    uint8_t  blk[64];
    uint64_t total;
    size_t   fill;
};

struct halg
{
    size_t outlen;      /* u */
    size_t blocklen;    /* v */
    void (*init)(struct hctx *);
    void (*block)(struct hctx *, const uint8_t *);
};

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha1_init(struct hctx *c)
{
    c->h[0] = 0x67452301; c->h[1] = 0xefcdab89; c->h[2] = 0x98badcfe; c->h[3] = 0x10325476; c->h[4] = 0xc3d2e1f0;
    c->total = 0; c->fill = 0;
}

static void sha1_block(struct hctx *c, const uint8_t *p)
{
    uint32_t w[80], a, b, d, e, f, k, t, cc;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (i = 16; i < 80; i++) w[i] = ROL32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (i = 0; i < 80; i++)
    {
        if (i < 20)      { f = (b & cc) | (~b & d);          k = 0x5a827999; }
        else if (i < 40) { f = b ^ cc ^ d;                   k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdc; }
        else             { f = b ^ cc ^ d;                   k = 0xca62c1d6; }
        t = ROL32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = ROL32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static const uint32_t K256[64] =
{
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_init(struct hctx *c)
{
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(c->h, iv, sizeof(iv));
    c->total = 0; c->fill = 0;
}

static void sha256_block(struct hctx *c, const uint8_t *p)
{
    uint32_t w[64], v[8], t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (i = 16; i < 64; i++)
    {
        uint32_t s0 = ROR32(w[i-15], 7) ^ ROR32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR32(w[i-2], 17) ^ ROR32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    memcpy(v, c->h, sizeof(v));
    for (i = 0; i < 64; i++)
    {
        t1 = v[7] + (ROR32(v[4], 6) ^ ROR32(v[4], 11) ^ ROR32(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + K256[i] + w[i];
        t2 = (ROR32(v[0], 2) ^ ROR32(v[0], 13) ^ ROR32(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++) c->h[i] += v[i];
}

static const struct halg alg_sha1   = { 20, 64, sha1_init,   sha1_block };
static const struct halg alg_sha256 = { 32, 64, sha256_init, sha256_block };

static void h_update(const struct halg *a, struct hctx *c, const uint8_t *d, size_t n)
{
    c->total += n;
    if (c->fill)
    {
        size_t k = 64 - c->fill;
        if (k > n) k = n;
        memcpy(c->blk + c->fill, d, k);
        c->fill += k; d += k; n -= k;
        if (c->fill < 64) return;
        a->block(c, c->blk);
        c->fill = 0;
    }
    while (n >= 64) { a->block(c, d); d += 64; n -= 64; }
    if (n) { memcpy(c->blk, d, n); c->fill = n; }
}

static void h_final(const struct halg *a, struct hctx *c, uint8_t *out)
{
    uint64_t bits = c->total * 8;
    uint8_t pad[72];
    size_t padlen = (c->fill < 56) ? 56 - c->fill : 120 - c->fill;
    size_t i, words = a->outlen / 4;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
    h_update(a, c, pad, padlen + 8);
    for (i = 0; i < words; i++)
    {
        out[4*i] = (uint8_t)(c->h[i] >> 24); out[4*i+1] = (uint8_t)(c->h[i] >> 16);
        out[4*i+2] = (uint8_t)(c->h[i] >> 8); out[4*i+3] = (uint8_t)c->h[i];
    }
    p12_wipe(c, sizeof(*c));
}

static void h_oneshot(const struct halg *a, const uint8_t *d, size_t n, uint8_t *out)
{
    struct hctx c;
    a->init(&c);
    h_update(a, &c, d, n);
    h_final(a, &c, out);
}

struct hmac
{
    const struct halg *alg;
    struct hctx in, out;
};

static void hmac_init(struct hmac *h, const struct halg *a, const uint8_t *key, size_t keylen)
{
    uint8_t k[64], pad[64];
    size_t i;
    h->alg = a;
    memset(k, 0, sizeof(k));
    if (keylen > 64) h_oneshot(a, key, keylen, k);
    else if (keylen) memcpy(k, key, keylen);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    a->init(&h->in); h_update(a, &h->in, pad, 64);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    a->init(&h->out); h_update(a, &h->out, pad, 64);
    p12_wipe(k, sizeof(k)); p12_wipe(pad, sizeof(pad));
}

/* finishes a message whose first parts were fed into the copy of h->in held in *c */
static void hmac_finish(const struct hmac *h, struct hctx *c, uint8_t *out)
{
    uint8_t inner[32];
    struct hctx o = h->out;
    h_final(h->alg, c, inner);
    h_update(h->alg, &o, inner, h->alg->outlen);
    h_final(h->alg, &o, out);
    p12_wipe(inner, sizeof(inner));
}

static void hmac_oneshot(const struct halg *a, const uint8_t *key, size_t keylen, const uint8_t *d, size_t n, uint8_t *out)
{
    struct hmac h;
    struct hctx c;
    hmac_init(&h, a, key, keylen);
    c = h.in;
    h_update(a, &c, d, n);
    hmac_finish(&h, &c, out);
    p12_wipe(&h, sizeof(h));
}

struct budget { uint64_t left; };

static int spend(struct budget *b, uint32_t iter)
{
    if (iter > P12_MAX_ITERATIONS || iter > b->left) return P12_ERR_LIMIT;
    b->left -= iter;
    return P12_OK;
}

static int pbkdf2(const struct halg *a, const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t slen,
                  uint32_t iter, uint8_t *out, size_t dklen)
{
    struct hmac h;
    uint8_t u[32], t[32];
    uint32_t blk, i;
    size_t j, off = 0;

    hmac_init(&h, a, pw, pwlen);
    for (blk = 1; off < dklen; blk++)
    {
        struct hctx c = h.in;
        uint8_t be[4] = { (uint8_t)(blk >> 24), (uint8_t)(blk >> 16), (uint8_t)(blk >> 8), (uint8_t)blk };
        size_t n;
        h_update(a, &c, salt, slen);
        h_update(a, &c, be, 4);
        hmac_finish(&h, &c, u);
        memcpy(t, u, a->outlen);
        for (i = 1; i < iter; i++)
        {
            c = h.in;
            h_update(a, &c, u, a->outlen);
            hmac_finish(&h, &c, u);
            for (j = 0; j < a->outlen; j++) t[j] ^= u[j];
        }
        n = dklen - off < a->outlen ? dklen - off : a->outlen;
        memcpy(out + off, t, n);
        off += n;
    }
    p12_wipe(&h, sizeof(h)); p12_wipe(u, sizeof(u)); p12_wipe(t, sizeof(t));
    return P12_OK;
}

/* RFC 7292 appendix B.2. pw is the BMPString password with its terminator, or empty. */
static int p12_kdf(const struct halg *a, uint8_t id, const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t slen,
                   uint32_t iter, uint8_t *out, size_t n)
{
    size_t u = a->outlen, v = a->blocklen, ilen, i, j, off = 0;
    size_t slen_r = slen ? ((slen + v - 1) / v) * v : 0, plen_r = pwlen ? ((pwlen + v - 1) / v) * v : 0;
    uint8_t d[64], *I, A[32], B[64];
    uint32_t r;

    if (slen > P12_MAX_SALT || pwlen > 4 * 1024) return P12_ERR_LIMIT;
    ilen = slen_r + plen_r;
    if (!(I = xmalloc(ilen))) return P12_ERR_NOMEM;
    memset(d, id, v);
    for (i = 0; i < slen_r; i++) I[i] = salt[i % slen];
    for (i = 0; i < plen_r; i++) I[slen_r + i] = pw[i % pwlen];

    while (off < n)
    {
        struct hctx c;
        size_t take;
        a->init(&c);
        h_update(a, &c, d, v);
        h_update(a, &c, I, ilen);
        h_final(a, &c, A);
        for (r = 1; r < iter; r++) h_oneshot(a, A, u, A);
        take = n - off < u ? n - off : u;
        memcpy(out + off, A, take);
        off += take;
        if (off >= n) break;
        for (i = 0; i < v; i++) B[i] = A[i % u];
        for (j = 0; j < ilen; j += v)
        {
            unsigned carry = 1;
            for (i = v; i-- > 0;)
            {
                unsigned t = I[j + i] + B[i] + carry;
                I[j + i] = (uint8_t)t;
                carry = t >> 8;
            }
        }
    }
    p12_wipe(I, ilen); free(I);
    p12_wipe(A, sizeof(A)); p12_wipe(B, sizeof(B));
    return P12_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* RC2, RC4, CBC                                                                              */
/* ------------------------------------------------------------------------------------------ */

static const uint8_t rc2_pitable[256] =
{
    0xd9,0x78,0xf9,0xc4,0x19,0xdd,0xb5,0xed,0x28,0xe9,0xfd,0x79,0x4a,0xa0,0xd8,0x9d,
    0xc6,0x7e,0x37,0x83,0x2b,0x76,0x53,0x8e,0x62,0x4c,0x64,0x88,0x44,0x8b,0xfb,0xa2,
    0x17,0x9a,0x59,0xf5,0x87,0xb3,0x4f,0x13,0x61,0x45,0x6d,0x8d,0x09,0x81,0x7d,0x32,
    0xbd,0x8f,0x40,0xeb,0x86,0xb7,0x7b,0x0b,0xf0,0x95,0x21,0x22,0x5c,0x6b,0x4e,0x82,
    0x54,0xd6,0x65,0x93,0xce,0x60,0xb2,0x1c,0x73,0x56,0xc0,0x14,0xa7,0x8c,0xf1,0xdc,
    0x12,0x75,0xca,0x1f,0x3b,0xbe,0xe4,0xd1,0x42,0x3d,0xd4,0x30,0xa3,0x3c,0xb6,0x26,
    0x6f,0xbf,0x0e,0xda,0x46,0x69,0x07,0x57,0x27,0xf2,0x1d,0x9b,0xbc,0x94,0x43,0x03,
    0xf8,0x11,0xc7,0xf6,0x90,0xef,0x3e,0xe7,0x06,0xc3,0xd5,0x2f,0xc8,0x66,0x1e,0xd7,
    0x08,0xe8,0xea,0xde,0x80,0x52,0xee,0xf7,0x84,0xaa,0x72,0xac,0x35,0x4d,0x6a,0x2a,
    0x96,0x1a,0xd2,0x71,0x5a,0x15,0x49,0x74,0x4b,0x9f,0xd0,0x5e,0x04,0x18,0xa4,0xec,
    0xc2,0xe0,0x41,0x6e,0x0f,0x51,0xcb,0xcc,0x24,0x91,0xaf,0x50,0xa1,0xf4,0x70,0x39,
    0x99,0x7c,0x3a,0x85,0x23,0xb8,0xb4,0x7a,0xfc,0x02,0x36,0x5b,0x25,0x55,0x97,0x31,
    0x2d,0x5d,0xfa,0x98,0xe3,0x8a,0x92,0xae,0x05,0xdf,0x29,0x10,0x67,0x6c,0xba,0xc9,
    0xd3,0x00,0xe6,0xcf,0xe1,0x9e,0xa8,0x2c,0x63,0x16,0x01,0x3f,0x58,0xe2,0x89,0xa9,
    0x0d,0x38,0x34,0x1b,0xab,0x33,0xff,0xb0,0xbb,0x48,0x0c,0x5f,0xb9,0xb1,0xcd,0x2e,
    0xc5,0xf3,0xdb,0x47,0xe5,0xa5,0x9c,0x77,0x0a,0xa6,0x20,0x68,0xfe,0x7f,0xc1,0xad
};

struct rc2key { uint16_t K[64]; };

static void rc2k_setup(struct rc2key *k, const uint8_t *key, size_t t, unsigned bits)
{
    uint8_t L[128];
    unsigned t8 = (bits + 7) / 8, tm = 255 & ((1u << (8 + bits - 8 * t8)) - 1);
    int i;
    memcpy(L, key, t);
    for (i = (int)t; i < 128; i++) L[i] = rc2_pitable[(uint8_t)(L[i - 1] + L[i - t])];
    L[128 - t8] = rc2_pitable[L[128 - t8] & tm];
    for (i = 127 - (int)t8; i >= 0; i--) L[i] = rc2_pitable[L[i + 1] ^ L[i + t8]];
    for (i = 0; i < 64; i++) k->K[i] = (uint16_t)(L[2*i] | (L[2*i+1] << 8));
    p12_wipe(L, sizeof(L));
}

#define R16L(x, n) ((uint16_t)(((x) << (n)) | ((x) >> (16 - (n)))))
#define R16R(x, n) ((uint16_t)(((x) >> (n)) | ((x) << (16 - (n)))))

static void rc2_encrypt(const struct rc2key *k, const uint8_t *in, uint8_t *out)
{
    uint16_t r[4];
    int i, j = 0;
    static const int sh[4] = { 1, 2, 3, 5 };
    for (i = 0; i < 4; i++) r[i] = (uint16_t)(in[2*i] | (in[2*i+1] << 8));
    for (i = 0; i < 16; i++)
    {
        int m;
        for (m = 0; m < 4; m++)
        {
            r[m] = (uint16_t)(r[m] + k->K[j++] + (r[(m+3)&3] & r[(m+2)&3]) + ((~r[(m+3)&3]) & r[(m+1)&3]));
            r[m] = R16L(r[m], sh[m]);
        }
        if (i == 4 || i == 10)
            for (m = 0; m < 4; m++) r[m] = (uint16_t)(r[m] + k->K[r[(m+3)&3] & 63]);
    }
    for (i = 0; i < 4; i++) { out[2*i] = (uint8_t)r[i]; out[2*i+1] = (uint8_t)(r[i] >> 8); }
}

static void rc2_decrypt(const struct rc2key *k, const uint8_t *in, uint8_t *out)
{
    uint16_t r[4];
    int i, j = 63;
    static const int sh[4] = { 1, 2, 3, 5 };
    for (i = 0; i < 4; i++) r[i] = (uint16_t)(in[2*i] | (in[2*i+1] << 8));
    for (i = 15; i >= 0; i--)
    {
        int m;
        for (m = 3; m >= 0; m--)
        {
            r[m] = R16R(r[m], sh[m]);
            r[m] = (uint16_t)(r[m] - k->K[j--] - (r[(m+3)&3] & r[(m+2)&3]) - ((~r[(m+3)&3]) & r[(m+1)&3]));
        }
        if (i == 5 || i == 11)
            for (m = 3; m >= 0; m--) r[m] = (uint16_t)(r[m] - k->K[r[(m+3)&3] & 63]);
    }
    for (i = 0; i < 4; i++) { out[2*i] = (uint8_t)r[i]; out[2*i+1] = (uint8_t)(r[i] >> 8); }
}

static void rc4_crypt(const uint8_t *key, size_t keylen, const uint8_t *in, size_t n, uint8_t *out)
{
    uint8_t S[256], t;
    unsigned i, j = 0, x = 0, y = 0;
    for (i = 0; i < 256; i++) S[i] = (uint8_t)i;
    for (i = 0; i < 256; i++)
    {
        j = (j + S[i] + key[i % keylen]) & 255;
        t = S[i]; S[i] = S[j]; S[j] = t;
    }
    while (n--)
    {
        x = (x + 1) & 255; y = (y + S[x]) & 255;
        t = S[x]; S[x] = S[y]; S[y] = t;
        *out++ = *in++ ^ S[(S[x] + S[y]) & 255];
    }
    p12_wipe(S, sizeof(S));
}

enum cipher { C_3DES, C_RC2, C_RC4, C_AES };

struct cparams
{
    enum cipher cipher;
    size_t keylen;      /* bytes of key material (2-key 3DES: 16, expanded internally) */
    unsigned rc2bits;
    size_t ivlen;       /* block size for CBC, 0 for RC4 */
};

/* CBC with PKCS #7 padding. For decryption in/inlen must be a block multiple. *out is allocated. */
static int cbc(const struct cparams *cp, const uint8_t *key, const uint8_t *iv, int encrypt,
               const uint8_t *in, size_t inlen, uint8_t **out, size_t *outlen)
{
    union { des3_key d3; aes_key aes; struct rc2key rc2; } ctx;
    size_t bs = cp->ivlen, total, i, off;
    uint8_t *o, chain[16], blk[16];
    uint8_t k3[24];

    if (cp->cipher == C_RC4)
    {
        if (!(o = xmalloc(inlen))) return P12_ERR_NOMEM;
        rc4_crypt(key, cp->keylen, in, inlen, o);
        *out = o; *outlen = inlen;
        return P12_OK;
    }
    if (!encrypt && (!inlen || inlen % bs)) return P12_ERR_DECRYPT;
    total = encrypt ? (inlen / bs + 1) * bs : inlen;
    if (!(o = xmalloc(total))) return P12_ERR_NOMEM;

    memset(&ctx, 0, sizeof(ctx));
    switch (cp->cipher)
    {
    case C_3DES:
        memcpy(k3, key, cp->keylen);
        if (cp->keylen == 16) memcpy(k3 + 16, key, 8);
        if (des3_setup(k3, 24, 0, &ctx.d3)) { free(o); return P12_ERR_UNSUPPORTED; }
        p12_wipe(k3, sizeof(k3));
        break;
    case C_AES:
        if (aes_setup(key, (int)cp->keylen, 0, &ctx.aes)) { free(o); return P12_ERR_UNSUPPORTED; }
        break;
    default:
        rc2k_setup(&ctx.rc2, key, cp->keylen, cp->rc2bits);
        break;
    }
    memcpy(chain, iv, bs);
    for (off = 0; off < total; off += bs)
    {
        if (encrypt)
        {
            size_t n = inlen - off > bs ? bs : (off <= inlen ? inlen - off : 0);
            memset(blk, (int)(bs - n), bs);             /* PKCS #7 padding bytes */
            if (n) memcpy(blk, in + off, n);
            for (i = 0; i < bs; i++) blk[i] ^= chain[i];
            switch (cp->cipher)
            {
            case C_3DES: des3_ecb_encrypt(blk, o + off, &ctx.d3); break;
            case C_AES:  aes_ecb_encrypt(blk, o + off, &ctx.aes); break;
            default:     rc2_encrypt(&ctx.rc2, blk, o + off); break;
            }
            memcpy(chain, o + off, bs);
        }
        else
        {
            switch (cp->cipher)
            {
            case C_3DES: des3_ecb_decrypt(in + off, blk, &ctx.d3); break;
            case C_AES:  aes_ecb_decrypt(in + off, blk, &ctx.aes); break;
            default:     rc2_decrypt(&ctx.rc2, in + off, blk); break;
            }
            for (i = 0; i < bs; i++) blk[i] ^= chain[i];
            memcpy(chain, in + off, bs);
            memcpy(o + off, blk, bs);
        }
    }
    p12_wipe(&ctx, sizeof(ctx)); p12_wipe(blk, sizeof(blk));
    if (encrypt) { *out = o; *outlen = total; return P12_OK; }
    {
        uint8_t pad = o[total - 1];
        unsigned bad = (pad == 0 || pad > bs);
        for (i = 0; i < bs; i++)
            if (i < pad && o[total - 1 - i] != pad) bad = 1;
        if (bad) { p12_wipe(o, total); free(o); return P12_ERR_DECRYPT; }
        *out = o; *outlen = total - pad;
    }
    return P12_OK;
}

/* Content encryption for CMS EnvelopedData (crypt32 msg.c): CBC with PKCS #7 padding, or RC4.
 * Single DES runs as 3DES with the key repeated. */
int p12_cms_crypt(int cipher, const uint8_t *key, size_t keylen, unsigned rc2bits, const uint8_t *iv,
                  int encrypt, const uint8_t *in, size_t inlen, uint8_t **out, size_t *outlen)
{
    struct cparams cp;
    uint8_t k3[24];
    int r;

    memset(&cp, 0, sizeof(cp));
    memset(k3, 0, sizeof(k3));
    switch (cipher)
    {
    case P12_CIPHER_3DES:
        if (keylen != 24) return P12_ERR_UNSUPPORTED;
        cp.cipher = C_3DES; cp.keylen = 24; cp.ivlen = 8;
        break;
    case P12_CIPHER_DES:
        if (keylen != 8) return P12_ERR_UNSUPPORTED;
        memcpy(k3, key, 8); memcpy(k3 + 8, key, 8); memcpy(k3 + 16, key, 8);
        key = k3; keylen = 24;
        cp.cipher = C_3DES; cp.keylen = 24; cp.ivlen = 8;
        break;
    case P12_CIPHER_RC2:
        if (!keylen || keylen > 128 || !rc2bits || rc2bits > 1024) return P12_ERR_UNSUPPORTED;
        cp.cipher = C_RC2; cp.keylen = keylen; cp.rc2bits = rc2bits; cp.ivlen = 8;
        break;
    case P12_CIPHER_RC4:
        if (!keylen || keylen > 256) return P12_ERR_UNSUPPORTED;
        cp.cipher = C_RC4; cp.keylen = keylen;
        break;
    case P12_CIPHER_AES:
        if (keylen != 16 && keylen != 24 && keylen != 32) return P12_ERR_UNSUPPORTED;
        cp.cipher = C_AES; cp.keylen = keylen; cp.ivlen = 16;
        break;
    default:
        return P12_ERR_UNSUPPORTED;
    }
    r = cbc(&cp, key, iv, encrypt, in, inlen, out, outlen);
    p12_wipe(k3, sizeof(k3));
    return r;
}

/* ------------------------------------------------------------------------------------------ */
/* passwords                                                                                  */
/* ------------------------------------------------------------------------------------------ */

struct pwd
{
    const uint16_t *w;      /* NULL: no password */
    uint8_t *bmp;           /* big-endian UTF-16 with terminator; empty for NULL */
    size_t   bmp_len;
    uint8_t *utf8;          /* without terminator */
    size_t   utf8_len;
};

static int pwd_init(struct pwd *p, const uint16_t *w)
{
    size_t n = 0, i, o = 0;
    memset(p, 0, sizeof(*p));
    p->w = w;
    if (!w) { p->bmp = xmalloc(1); p->utf8 = xmalloc(1); return (p->bmp && p->utf8) ? P12_OK : P12_ERR_NOMEM; }
    while (w[n]) { if (++n > 1024) return P12_ERR_LIMIT; }
    p->bmp_len = 2 * (n + 1);
    p->bmp = xmalloc(p->bmp_len);
    p->utf8 = xmalloc(n * 3 + 4);
    if (!p->bmp || !p->utf8) return P12_ERR_NOMEM;
    for (i = 0; i < n; i++) { p->bmp[2*i] = (uint8_t)(w[i] >> 8); p->bmp[2*i+1] = (uint8_t)w[i]; }
    p->bmp[2*n] = p->bmp[2*n+1] = 0;
    for (i = 0; i < n; i++)
    {
        uint32_t c = w[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && w[i+1] >= 0xdc00 && w[i+1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (w[++i] - 0xdc00);
        if (c < 0x80) p->utf8[o++] = (uint8_t)c;
        else if (c < 0x800) { p->utf8[o++] = (uint8_t)(0xc0 | (c >> 6)); p->utf8[o++] = (uint8_t)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { p->utf8[o++] = (uint8_t)(0xe0 | (c >> 12)); p->utf8[o++] = (uint8_t)(0x80 | ((c >> 6) & 0x3f)); p->utf8[o++] = (uint8_t)(0x80 | (c & 0x3f)); }
        else { p->utf8[o++] = (uint8_t)(0xf0 | (c >> 18)); p->utf8[o++] = (uint8_t)(0x80 | ((c >> 12) & 0x3f)); p->utf8[o++] = (uint8_t)(0x80 | ((c >> 6) & 0x3f)); p->utf8[o++] = (uint8_t)(0x80 | (c & 0x3f)); }
    }
    p->utf8_len = o;
    return P12_OK;
}

static void pwd_free(struct pwd *p)
{
    if (p->bmp) { p12_wipe(p->bmp, p->bmp_len); free(p->bmp); }
    if (p->utf8) { p12_wipe(p->utf8, p->utf8_len); free(p->utf8); }
    memset(p, 0, sizeof(*p));
}

/* ------------------------------------------------------------------------------------------ */
/* password based encryption: PKCS #12 PBEs and PBES2                                         */
/* ------------------------------------------------------------------------------------------ */

static int is_pkcs12_pbe(span oid, struct cparams *cp)
{
    memset(cp, 0, sizeof(*cp));
    if (OID_IS(oid, oid_pbe_3des))         { cp->cipher = C_3DES; cp->keylen = 24; cp->ivlen = 8; }
    else if (OID_IS(oid, oid_pbe_2des))    { cp->cipher = C_3DES; cp->keylen = 16; cp->ivlen = 8; }
    else if (OID_IS(oid, oid_pbe_rc2_128)) { cp->cipher = C_RC2; cp->keylen = 16; cp->rc2bits = 128; cp->ivlen = 8; }
    else if (OID_IS(oid, oid_pbe_rc2_40))  { cp->cipher = C_RC2; cp->keylen = 5; cp->rc2bits = 40; cp->ivlen = 8; }
    else if (OID_IS(oid, oid_pbe_rc4_128)) { cp->cipher = C_RC4; cp->keylen = 16; }
    else if (OID_IS(oid, oid_pbe_rc4_40))  { cp->cipher = C_RC4; cp->keylen = 5; }
    else return 0;
    return 1;
}

/* AlgorithmIdentifier SEQUENCE content in 'alg'. Encrypts or decrypts in -> *out. */
static int pbe_crypt(span alg, const struct pwd *pw, int encrypt, const uint8_t *in, size_t inlen,
                     uint8_t **out, size_t *outlen, struct budget *bud)
{
    span oid, params;
    struct cparams cp;
    uint8_t key[32], iv[16];
    int r;

    if ((r = get_oid(&alg, &oid))) return r;
    params = alg;

    if (is_pkcs12_pbe(oid, &cp))
    {
        span ps, salt;
        uint8_t *salt_own;
        uint32_t iter;
        if ((r = get_seq(&params, &ps))) return r;
        if ((r = get_octets(&ps, &salt, &salt_own))) return r;
        if ((r = get_uint(&ps, &iter))) { free(salt_own); return r; }
        if (!iter) { free(salt_own); return P12_ERR_FORMAT; }
        /* two derivations, one for the key and one for the IV */
        if ((r = spend(bud, iter)) || (r = spend(bud, iter))) { free(salt_own); return r; }
        r = p12_kdf(&alg_sha1, 1, pw->bmp, pw->bmp_len, salt.p, salt.n, iter, key, cp.keylen);
        if (!r && cp.ivlen) r = p12_kdf(&alg_sha1, 2, pw->bmp, pw->bmp_len, salt.p, salt.n, iter, iv, cp.ivlen);
        free(salt_own);
        if (r) return r;
    }
    else if (OID_IS(oid, oid_pbes2))
    {
        span ps, kdf, kdfoid, kps, salt, enc, encoid;
        struct tlv t;
        uint8_t *salt_own;
        uint32_t iter, keylen = 0;
        const struct halg *prf = &alg_sha1;
        uint8_t *iv_own = NULL;
        span ivs;

        if ((r = get_seq(&params, &ps))) return r;
        if ((r = get_seq(&ps, &kdf)) || (r = get_oid(&kdf, &kdfoid))) return r;
        if (!OID_IS(kdfoid, oid_pbkdf2)) return P12_ERR_UNSUPPORTED;
        if ((r = get_seq(&kdf, &kps))) return r;
        if ((r = get_octets(&kps, &salt, &salt_own))) return r;   /* otherSource is not supported */
        if ((r = get_uint(&kps, &iter))) { free(salt_own); return r; }
        if (!iter) { free(salt_own); return P12_ERR_FORMAT; }
        if (kps.n)
        {
            span save = kps;
            if (!(r = next_tlv(&save, &t, 0)) && t.tag == T_INTEGER) { if ((r = get_uint(&kps, &keylen))) { free(salt_own); return r; } }
            if (kps.n)
            {
                span prfalg, prfoid;
                if ((r = get_seq(&kps, &prfalg)) || (r = get_oid(&prfalg, &prfoid))) { free(salt_own); return r; }
                if (OID_IS(prfoid, oid_hmac_sha1)) prf = &alg_sha1;
                else if (OID_IS(prfoid, oid_hmac_sha256)) prf = &alg_sha256;
                else { free(salt_own); return P12_ERR_UNSUPPORTED; }
            }
        }
        if ((r = get_seq(&ps, &enc)) || (r = get_oid(&enc, &encoid))) { free(salt_own); return r; }
        memset(&cp, 0, sizeof(cp));
        if (OID_IS(encoid, oid_aes256_cbc)) { cp.cipher = C_AES; cp.keylen = 32; cp.ivlen = 16; }
        else if (OID_IS(encoid, oid_aes192_cbc)) { cp.cipher = C_AES; cp.keylen = 24; cp.ivlen = 16; }
        else if (OID_IS(encoid, oid_aes128_cbc)) { cp.cipher = C_AES; cp.keylen = 16; cp.ivlen = 16; }
        else if (OID_IS(encoid, oid_des_ede3_cbc)) { cp.cipher = C_3DES; cp.keylen = 24; cp.ivlen = 8; }
        else { free(salt_own); return P12_ERR_UNSUPPORTED; }
        if (keylen && keylen != cp.keylen) { free(salt_own); return P12_ERR_FORMAT; }
        if ((r = get_octets(&enc, &ivs, &iv_own)) || ivs.n != cp.ivlen)
        {
            free(salt_own); free(iv_own);
            return r ? r : P12_ERR_FORMAT;
        }
        memcpy(iv, ivs.p, cp.ivlen);
        free(iv_own);
        if (salt.n > P12_MAX_SALT) { free(salt_own); return P12_ERR_LIMIT; }
        if ((r = spend(bud, iter))) { free(salt_own); return r; }
        r = pbkdf2(prf, pw->utf8, pw->utf8_len, salt.p, salt.n, iter, key, cp.keylen);
        free(salt_own);
        if (r) return r;
    }
    else return P12_ERR_UNSUPPORTED;

    r = cbc(&cp, key, iv, encrypt, in, inlen, out, outlen);
    p12_wipe(key, sizeof(key)); p12_wipe(iv, sizeof(iv));
    return r;
}

/* ------------------------------------------------------------------------------------------ */
/* parsing                                                                                    */
/* ------------------------------------------------------------------------------------------ */

struct ctx
{
    const struct pwd *pw;
    struct budget     bud;
    struct p12_contents *out;
    size_t            nbags, certs_cap, keys_cap;
    int               used_encryption;
};

static int item_add(struct p12_item **arr, size_t *n, size_t *cap, struct p12_item *it)
{
    if (*n == *cap)
    {
        size_t nc = *cap ? *cap * 2 : 8;
        struct p12_item *na = xmalloc(nc * sizeof(*na));
        if (!na) return P12_ERR_NOMEM;
        if (*n) memcpy(na, *arr, *n * sizeof(*na));
        free(*arr);
        *arr = na; *cap = nc;
    }
    (*arr)[(*n)++] = *it;
    return P12_OK;
}

static void item_free(struct p12_item *it)
{
    if (it->data) { p12_wipe(it->data, it->len); free(it->data); }
    free(it->id); free(it->name);
    memset(it, 0, sizeof(*it));
}

void p12_free_contents(struct p12_contents *c)
{
    size_t i;
    for (i = 0; i < c->ncerts; i++) item_free(&c->certs[i]);
    for (i = 0; i < c->nkeys; i++) item_free(&c->keys[i]);
    free(c->certs); free(c->keys);
    memset(c, 0, sizeof(*c));
}

/* SET OF Attribute */
static int parse_attrs(span set, struct p12_item *it)
{
    while (set.n)
    {
        span attr, oid, vals;
        struct tlv v;
        int r;
        if ((r = get_seq(&set, &attr)) || (r = get_oid(&attr, &oid))) return r;
        if ((r = get_tlv(&attr, T_SET, &v))) return r;
        vals = v.val;
        if (OID_IS(oid, oid_friendlyname) && !it->name)
        {
            struct tlv s;
            size_t i, n;
            if (!vals.n) continue;
            if ((r = get_tlv(&vals, T_BMP, &s))) return r;
            if (s.val.n & 1) return P12_ERR_FORMAT;
            n = s.val.n / 2;
            if (!(it->name = xmalloc((n + 1) * sizeof(uint16_t)))) return P12_ERR_NOMEM;
            for (i = 0; i < n; i++) it->name[i] = (uint16_t)((s.val.p[2*i] << 8) | s.val.p[2*i+1]);
            it->name[n] = 0;
        }
        else if (OID_IS(oid, oid_localkeyid) && !it->id)
        {
            span id;
            uint8_t *own;
            if (!vals.n) continue;
            if ((r = get_octets(&vals, &id, &own))) return r;
            it->id = own ? own : xdup(id.p, id.n);
            it->id_len = id.n;
            if (!it->id) return P12_ERR_NOMEM;
        }
    }
    return P12_OK;
}

static int parse_safecontents(struct ctx *cx, span sc, int depth);

static int parse_bag(struct ctx *cx, span bag, int depth)
{
    span oid, attrs = { NULL, 0 };
    struct tlv val;
    struct p12_item it;
    int r;

    memset(&it, 0, sizeof(it));
    if ((r = get_oid(&bag, &oid))) return r;
    if ((r = get_tlv(&bag, T_CTX0, &val))) return r;
    if (bag.n)
    {
        struct tlv a;
        if ((r = get_tlv(&bag, T_SET, &a))) return r;
        attrs = a.val;
    }
    if (++cx->nbags > P12_MAX_BAGS) return P12_ERR_LIMIT;

    if (OID_IS(oid, oid_keybag) || OID_IS(oid, oid_shroudedbag))
    {
        span in = val.val, pki;
        struct tlv t;
        uint8_t *plain = NULL;
        size_t plain_len = 0;

        if ((r = next_tlv(&in, &t, 0))) return r;
        if (t.tag != T_SEQ) return P12_ERR_FORMAT;
        if (OID_IS(oid, oid_keybag))
        {
            if (!(it.data = xdup(val.val.p, val.val.n - in.n))) return P12_ERR_NOMEM;
            it.len = val.val.n - in.n;
        }
        else
        {
            span epki = t.val, alg, ct;
            uint8_t *ct_own;
            struct tlv a;
            if ((r = get_tlv(&epki, T_SEQ, &a))) return r;
            alg = a.val;
            if ((r = get_octets(&epki, &ct, &ct_own))) return r;
            cx->used_encryption = 1;
            r = pbe_crypt(alg, cx->pw, 0, ct.p, ct.n, &plain, &plain_len, &cx->bud);
            free(ct_own);
            if (r) return r;
            pki.p = plain; pki.n = plain_len;
            /* sanity check: must be a SEQUENCE */
            {
                span c2 = pki;
                if ((r = next_tlv(&c2, &t, 0)) || t.tag != T_SEQ) { p12_wipe(plain, plain_len); free(plain); return P12_ERR_DECRYPT; }
            }
            it.data = plain; it.len = plain_len;
        }
        if ((r = parse_attrs(attrs, &it))) { item_free(&it); return r; }
        if ((r = item_add(&cx->out->keys, &cx->out->nkeys, &cx->keys_cap, &it))) { item_free(&it); return r; }
        return P12_OK;
    }
    if (OID_IS(oid, oid_certbag))
    {
        span cb = val.val, cid, cert;
        struct tlv seq, cv;
        uint8_t *own;
        if ((r = get_tlv(&cb, T_SEQ, &seq))) return r;
        cb = seq.val;
        if ((r = get_oid(&cb, &cid))) return r;
        if (!OID_IS(cid, oid_x509cert)) return P12_OK;      /* sdsiCertificate: not interesting */
        if ((r = get_tlv(&cb, T_CTX0, &cv))) return r;
        {
            span inner = cv.val;
            if ((r = get_octets(&inner, &cert, &own))) return r;
        }
        {
            span c2 = cert;
            struct tlv t;
            if ((r = next_tlv(&c2, &t, 0)) || t.tag != T_SEQ) { free(own); return r ? r : P12_ERR_FORMAT; }
        }
        it.data = own ? own : xdup(cert.p, cert.n);
        it.len = cert.n;
        if (!it.data) return P12_ERR_NOMEM;
        if ((r = parse_attrs(attrs, &it))) { item_free(&it); return r; }
        if ((r = item_add(&cx->out->certs, &cx->out->ncerts, &cx->certs_cap, &it))) { item_free(&it); return r; }
        return P12_OK;
    }
    if (OID_IS(oid, oid_safecontbag))
    {
        span in = val.val;
        struct tlv t;
        if (depth > 4) return P12_ERR_LIMIT;
        if ((r = get_tlv(&in, T_SEQ, &t))) return r;
        return parse_safecontents(cx, t.val, depth + 1);
    }
    return P12_OK;      /* crlBag, secretBag, unknown bags: ignored */
}

static int parse_safecontents(struct ctx *cx, span sc, int depth)
{
    while (sc.n)
    {
        span bag;
        int r;
        if ((r = get_seq(&sc, &bag)) || (r = parse_bag(cx, bag, depth))) return r;
    }
    return P12_OK;
}

static int parse_authsafe(struct ctx *cx, span as)
{
    while (as.n)
    {
        span ci, oid;
        struct tlv c0;
        int r;
        if ((r = get_seq(&as, &ci)) || (r = get_oid(&ci, &oid))) return r;
        if (!ci.n) return P12_ERR_FORMAT;
        if ((r = get_tlv(&ci, T_CTX0, &c0))) return r;
        if (OID_IS(oid, oid_data))
        {
            span inner = c0.val, sc;
            uint8_t *own;
            struct tlv t;
            if ((r = get_octets(&inner, &sc, &own))) return r;
            if ((r = get_tlv(&sc, T_SEQ, &t))) { free(own); return r; }
            r = parse_safecontents(cx, t.val, 0);
            free(own);
            if (r) return r;
        }
        else if (OID_IS(oid, oid_encdata))
        {
            span ed = c0.val, eci, alg, ct, coid;
            struct tlv t, a, c;
            uint8_t *own = NULL, *plain;
            size_t plain_len;
            uint32_t ver;
            if ((r = get_tlv(&ed, T_SEQ, &t))) return r;
            ed = t.val;
            if ((r = get_uint(&ed, &ver))) return r;
            if ((r = get_seq(&ed, &eci)) || (r = get_oid(&eci, &coid))) return r;
            if (!OID_IS(coid, oid_data)) return P12_ERR_FORMAT;
            if ((r = get_tlv(&eci, T_SEQ, &a))) return r;
            alg = a.val;
            /* encryptedContent [0] IMPLICIT OCTET STRING (primitive 0x80 or constructed 0xa0) */
            if ((r = next_tlv(&eci, &c, 0))) return r;
            if (c.tag != 0x80 && c.tag != 0xa0) return P12_ERR_FORMAT;
            {
                struct tlv asoct = c;
                asoct.tag = c.cons ? 0x24 : T_OCTETS;
                if ((r = get_octets_from(&asoct, &ct, &own, 0))) return r;
            }
            cx->used_encryption = 1;
            r = pbe_crypt(alg, cx->pw, 0, ct.p, ct.n, &plain, &plain_len, &cx->bud);
            free(own);
            if (r) return r;
            {
                span sc = { plain, plain_len };
                struct tlv st;
                r = get_tlv(&sc, T_SEQ, &st);
                if (r) r = P12_ERR_DECRYPT;
                else r = parse_safecontents(cx, st.val, 0);
            }
            p12_wipe(plain, plain_len); free(plain);
            if (r) return r;
        }
        else return P12_ERR_UNSUPPORTED;
    }
    return P12_OK;
}

struct pfx_hdr
{
    span authsafe;          /* the octets covered by the MAC */
    uint8_t *authsafe_own;
    int has_mac;
    const struct halg *mac_alg;
    span mac_digest, mac_salt;
    uint32_t mac_iter;
    uint8_t *own_digest, *own_salt;
};

static void hdr_free(struct pfx_hdr *h)
{
    free(h->authsafe_own); free(h->own_digest); free(h->own_salt);
    memset(h, 0, sizeof(*h));
}

static int parse_pfx_hdr(const uint8_t *buf, size_t len, struct pfx_hdr *h)
{
    span in = { buf, len }, pfx, ci, oid;
    struct tlv t;
    uint32_t ver;
    int r;

    memset(h, 0, sizeof(*h));
    if ((r = get_seq(&in, &pfx))) return r;
    if ((r = get_uint(&pfx, &ver))) return r;
    if (ver != 3) return P12_ERR_FORMAT;
    if ((r = get_seq(&pfx, &ci)) || (r = get_oid(&ci, &oid))) return r;
    if (!OID_IS(oid, oid_data)) return P12_ERR_UNSUPPORTED;     /* signedData: public-key integrity mode */
    if ((r = get_tlv(&ci, T_CTX0, &t))) return r;
    {
        span inner = t.val;
        if ((r = get_octets(&inner, &h->authsafe, &h->authsafe_own))) return r;
    }
    if (pfx.n)
    {
        span md, di, dalg, aoid;
        if ((r = get_seq(&pfx, &md)) || (r = get_seq(&md, &di)) || (r = get_seq(&di, &dalg))) { hdr_free(h); return r ? r : P12_ERR_FORMAT; }
        if ((r = get_oid(&dalg, &aoid))) { hdr_free(h); return r; }
        if (OID_IS(aoid, oid_sha1)) h->mac_alg = &alg_sha1;
        else if (OID_IS(aoid, oid_sha256)) h->mac_alg = &alg_sha256;
        else { hdr_free(h); return P12_ERR_UNSUPPORTED; }
        if ((r = get_octets(&di, &h->mac_digest, &h->own_digest))) { hdr_free(h); return r; }
        if ((r = get_octets(&md, &h->mac_salt, &h->own_salt))) { hdr_free(h); return r; }
        h->mac_iter = 1;
        if (md.n && (r = get_uint(&md, &h->mac_iter))) { hdr_free(h); return r; }
        if (!h->mac_iter) { hdr_free(h); return P12_ERR_FORMAT; }
        if (h->mac_digest.n != h->mac_alg->outlen) { hdr_free(h); return P12_ERR_FORMAT; }
        h->has_mac = 1;
    }
    return P12_OK;
}

static int verify_mac(const struct pfx_hdr *h, const struct pwd *pw, struct budget *bud)
{
    uint8_t key[32], mac[32];
    unsigned diff = 0;
    size_t i;
    int r;

    if ((r = spend(bud, h->mac_iter))) return r;
    r = p12_kdf(h->mac_alg, 3, pw->bmp, pw->bmp_len, h->mac_salt.p, h->mac_salt.n, h->mac_iter, key, h->mac_alg->outlen);
    if (r) return r;
    hmac_oneshot(h->mac_alg, key, h->mac_alg->outlen, h->authsafe.p, h->authsafe.n, mac);
    for (i = 0; i < h->mac_alg->outlen; i++) diff |= mac[i] ^ h->mac_digest.p[i];
    p12_wipe(key, sizeof(key)); p12_wipe(mac, sizeof(mac));
    return diff ? P12_ERR_PASSWORD : P12_OK;
}

static int parse_with(const uint8_t *buf, size_t len, const uint16_t *password, struct p12_contents *out, int verify_only)
{
    struct pfx_hdr h;
    struct pwd pw;
    struct ctx cx;
    span as;
    struct tlv t;
    int r;

    if ((r = parse_pfx_hdr(buf, len, &h))) return r;
    if ((r = pwd_init(&pw, password))) { hdr_free(&h); return r; }
    memset(&cx, 0, sizeof(cx));
    cx.pw = &pw; cx.bud.left = P12_MAX_TOTAL_ITERATIONS; cx.out = out;
    if (h.has_mac && (r = verify_mac(&h, &pw, &cx.bud))) goto done;
    as = h.authsafe;
    if ((r = get_tlv(&as, T_SEQ, &t))) goto done;
    r = parse_authsafe(&cx, t.val);
    if (!h.has_mac)
    {
        /* Without a MAC nothing proves the password. Windows only rejects a password for a file
         * that is neither authenticated nor encrypted, and reports a failed decryption as bad data
         * but accepts the password in PFXVerifyPassword. */
        if (!r && !cx.used_encryption && password && password[0]) r = P12_ERR_PASSWORD;
        else if (r == P12_ERR_DECRYPT && verify_only) r = P12_OK;
    }
done:
    pwd_free(&pw);
    hdr_free(&h);
    if (r) p12_free_contents(out);
    return r;
}

static int parse_retry(const uint8_t *buf, size_t len, const uint16_t *password, struct p12_contents *out, int verify_only)
{
    static const uint16_t empty[1] = { 0 };
    int r;

    memset(out, 0, sizeof(*out));
    r = parse_with(buf, len, password, out, verify_only);
    if ((r == P12_ERR_PASSWORD || r == P12_ERR_DECRYPT) && (!password || !password[0]))
    {
        memset(out, 0, sizeof(*out));
        r = parse_with(buf, len, password ? NULL : empty, out, verify_only);
    }
    return r;
}

int p12_parse(const uint8_t *buf, size_t len, const uint16_t *password, struct p12_contents *out)
{
    return parse_retry(buf, len, password, out, 0);
}

int p12_verify_password(const uint8_t *buf, size_t len, const uint16_t *password)
{
    struct p12_contents c;
    int r = parse_retry(buf, len, password, &c, 1);
    p12_free_contents(&c);
    return r;
}

/* ------------------------------------------------------------------------------------------ */
/* RSA keys: PKCS #8 <-> CryptoAPI PRIVATEKEYBLOB                                             */
/* ------------------------------------------------------------------------------------------ */

#define RSA2_MAGIC 0x32415352
#define PRIVATEKEYBLOB_ID 7
#define CALG_RSA_KEYX_ID 0xa400

static int get_bignum(span *s, span *v)
{
    struct tlv t;
    int r = get_tlv(s, T_INTEGER, &t);
    if (r) return r;
    if (!t.val.n || (t.val.p[0] & 0x80)) return P12_ERR_FORMAT;
    *v = t.val;
    while (v->n > 1 && !v->p[0]) { v->p++; v->n--; }
    return P12_OK;
}

static int parse_rsa_pki(const uint8_t *pkcs8, size_t len, span *rsa_priv)
{
    span in = { pkcs8, len }, pki, alg, oid, os;
    struct tlv t;
    uint32_t ver;
    uint8_t *own;
    int r;
    if ((r = get_seq(&in, &pki))) return r;
    if ((r = get_uint(&pki, &ver))) return r;
    if ((r = get_seq(&pki, &alg)) || (r = get_oid(&alg, &oid))) return r;
    if (!OID_IS(oid, oid_rsa)) return P12_ERR_UNSUPPORTED;
    if ((r = get_octets(&pki, &os, &own))) return r;
    if (own) { free(own); return P12_ERR_UNSUPPORTED; }
    if ((r = get_tlv(&os, T_SEQ, &t))) return r;
    *rsa_priv = t.val;
    return P12_OK;
}

int p12_pkcs8_modulus(const uint8_t *pkcs8, size_t len, const uint8_t **mod, size_t *mod_len)
{
    span rp, v;
    uint32_t ver;
    int r;
    if ((r = parse_rsa_pki(pkcs8, len, &rp)) || (r = get_uint(&rp, &ver)) || (r = get_bignum(&rp, &v))) return r;
    *mod = v.p; *mod_len = v.n;
    return P12_OK;
}

int p12_cert_modulus(const uint8_t *cert, size_t len, const uint8_t **mod, size_t *mod_len)
{
    span in = { cert, len }, c, tbs, spki, alg, oid, bits, rsa;
    struct tlv t;
    span v;
    int r, i;
    if ((r = get_seq(&in, &c)) || (r = get_seq(&c, &tbs))) return r;
    if (tbs.n && tbs.p[0] == T_CTX0 && (r = next_tlv(&tbs, &t, 0))) return r;   /* version */
    for (i = 0; i < 5; i++)     /* serial, signature, issuer, validity, subject */
        if ((r = next_tlv(&tbs, &t, 0))) return r;
    if ((r = get_seq(&tbs, &spki))) return r;
    if ((r = get_seq(&spki, &alg)) || (r = get_oid(&alg, &oid))) return r;
    if (!OID_IS(oid, oid_rsa)) return P12_ERR_UNSUPPORTED;
    if ((r = get_tlv(&spki, 0x03, &t))) return r;
    bits = t.val;
    if (bits.n < 2 || bits.p[0]) return P12_ERR_FORMAT;
    bits.p++; bits.n--;
    if ((r = get_seq(&bits, &rsa)) || (r = get_bignum(&rsa, &v))) return r;
    *mod = v.p; *mod_len = v.n;
    return P12_OK;
}

static void put_le(uint8_t *dst, span v, size_t width)
{
    size_t i;
    memset(dst, 0, width);
    for (i = 0; i < v.n; i++) dst[i] = v.p[v.n - 1 - i];
}

int p12_pkcs8_to_rsa_blob(const uint8_t *pkcs8, size_t len, uint8_t **blob, size_t *blob_len)
{
    span rp, f[8];
    uint32_t ver;
    size_t modlen, half, total, i;
    uint32_t exp = 0;
    uint8_t *b, *p;
    int r;

    if ((r = parse_rsa_pki(pkcs8, len, &rp)) || (r = get_uint(&rp, &ver))) return r;
    if (ver != 0) return P12_ERR_UNSUPPORTED;
    for (i = 0; i < 8; i++) if ((r = get_bignum(&rp, &f[i]))) return r;
    /* n e d p q dp dq qinv -> blob order: n e | p q dp dq qinv d */
    modlen = f[0].n;
    if (modlen < 8 || modlen > 1024) return P12_ERR_UNSUPPORTED;
    half = (modlen + 1) / 2;
    if (f[1].n > 4) return P12_ERR_UNSUPPORTED;
    for (i = 0; i < f[1].n; i++) exp = (exp << 8) | f[1].p[i];
    if (f[2].n > modlen || f[3].n > half || f[4].n > half || f[5].n > half || f[6].n > half || f[7].n > half)
        return P12_ERR_FORMAT;
    total = 20 + modlen + 5 * half + modlen;
    if (!(b = xmalloc(total))) return P12_ERR_NOMEM;
    memset(b, 0, total);
    b[0] = PRIVATEKEYBLOB_ID; b[1] = 2;
    b[4] = (uint8_t)CALG_RSA_KEYX_ID; b[5] = (uint8_t)(CALG_RSA_KEYX_ID >> 8);
    b[8] = (uint8_t)RSA2_MAGIC; b[9] = (uint8_t)(RSA2_MAGIC >> 8); b[10] = (uint8_t)(RSA2_MAGIC >> 16); b[11] = (uint8_t)(RSA2_MAGIC >> 24);
    {
        uint32_t bits = (uint32_t)(modlen * 8);
        b[12] = (uint8_t)bits; b[13] = (uint8_t)(bits >> 8); b[14] = (uint8_t)(bits >> 16); b[15] = (uint8_t)(bits >> 24);
    }
    b[16] = (uint8_t)exp; b[17] = (uint8_t)(exp >> 8); b[18] = (uint8_t)(exp >> 16); b[19] = (uint8_t)(exp >> 24);
    p = b + 20;
    put_le(p, f[0], modlen); p += modlen;
    put_le(p, f[3], half); p += half;
    put_le(p, f[4], half); p += half;
    put_le(p, f[5], half); p += half;
    put_le(p, f[6], half); p += half;
    put_le(p, f[7], half); p += half;
    put_le(p, f[2], modlen);
    *blob = b; *blob_len = total;
    return P12_OK;
}

int p12_rsa_blob_to_pkcs8(const uint8_t *blob, size_t len, uint8_t **pkcs8, size_t *pkcs8_len)
{
    size_t modlen, half, i;
    uint32_t bits, exp;
    const uint8_t *p;
    uint8_t tmp[1024], eb[4];
    struct buf rsa = {0}, pki = {0}, out = {0};
    int r = P12_OK;

    if (len < 20 || blob[0] != PRIVATEKEYBLOB_ID) return P12_ERR_FORMAT;
    if ((blob[8] | (blob[9] << 8) | (blob[10] << 16) | ((uint32_t)blob[11] << 24)) != RSA2_MAGIC) return P12_ERR_FORMAT;
    bits = blob[12] | (blob[13] << 8) | (blob[14] << 16) | ((uint32_t)blob[15] << 24);
    exp = blob[16] | (blob[17] << 8) | (blob[18] << 16) | ((uint32_t)blob[19] << 24);
    if (!bits || (bits & 7) || bits > 8192) return P12_ERR_UNSUPPORTED;
    modlen = bits / 8; half = (modlen + 1) / 2;
    if (len < 20 + 2 * modlen + 5 * half) return P12_ERR_FORMAT;
    p = blob + 20;

    put_uint(&rsa, 0);
#define PUT_LE(w) do { for (i = 0; i < (w); i++) tmp[i] = p[(w) - 1 - i]; put_bignum(&rsa, tmp, (w)); p += (w); } while (0)
    {
        /* blob order n | p q dp dq qinv | d  ->  n e d p q dp dq qinv */
        const uint8_t *n = p, *pp = p + modlen, *d = p + modlen + 5 * half;
        for (i = 0; i < modlen; i++) tmp[i] = n[modlen - 1 - i];
        put_bignum(&rsa, tmp, modlen);
        eb[0] = (uint8_t)(exp >> 24); eb[1] = (uint8_t)(exp >> 16); eb[2] = (uint8_t)(exp >> 8); eb[3] = (uint8_t)exp;
        put_bignum(&rsa, eb, 4);
        for (i = 0; i < modlen; i++) tmp[i] = d[modlen - 1 - i];
        put_bignum(&rsa, tmp, modlen);
        p = pp;
        PUT_LE(half); PUT_LE(half); PUT_LE(half); PUT_LE(half); PUT_LE(half);
    }
#undef PUT_LE
    put_uint(&pki, 0);
    {
        struct buf alg = {0};
        PUT_OID(&alg, oid_rsa);
        put_tlv(&alg, T_NULL, NULL, 0);
        put_wrap(&pki, T_SEQ, &alg);
    }
    {
        struct buf seq = {0};
        put_wrap(&seq, T_SEQ, &rsa);
        put_wrap(&pki, T_OCTETS, &seq);
    }
    put_wrap(&out, T_SEQ, &pki);
    p12_wipe(tmp, sizeof(tmp));
    if (out.err) { r = out.err; b_free(&out); return r; }
    *pkcs8 = out.p; *pkcs8_len = out.len;
    return r;
}

/* ------------------------------------------------------------------------------------------ */
/* writing                                                                                    */
/* ------------------------------------------------------------------------------------------ */

#define P12_DEFAULT_ITER_V1     2000
#define P12_DEFAULT_ITER_PBES2  10000

/* SET OF Attribute for friendlyName / localKeyId, appended to 'bag' if there are any */
static void put_bag_attrs(struct buf *bag, const struct p12_item *it)
{
    struct buf set = {0};

    if (it->name)
    {
        struct buf a = {0}, vs = {0}, str = {0};
        size_t i, n = 0;
        while (it->name[n]) n++;
        for (i = 0; i < n; i++) { b_byte(&str, (uint8_t)(it->name[i] >> 8)); b_byte(&str, (uint8_t)it->name[i]); }
        PUT_OID(&a, oid_friendlyname);
        if (!str.err) put_tlv(&vs, T_BMP, str.p, str.len);
        b_free(&str);
        put_wrap(&a, T_SET, &vs);
        put_wrap(&set, T_SEQ, &a);
    }
    if (it->id)
    {
        struct buf a = {0}, vs = {0};
        PUT_OID(&a, oid_localkeyid);
        put_tlv(&vs, T_OCTETS, it->id, it->id_len);
        put_wrap(&a, T_SET, &vs);
        put_wrap(&set, T_SEQ, &a);
    }
    if (set.len || set.err) put_wrap(bag, T_SET, &set);
    else b_free(&set);
}

/* Builds the AlgorithmIdentifier SEQUENCE of a fresh PBE (random salt/IV). */
static int make_alg(const struct p12_write_params *wp, int for_key, uint32_t iter, struct buf *alg)
{
    uint8_t salt[16], iv[16];
    struct buf a = {0}, ps = {0};

    if (wp->flags & P12_WRITE_PBES2)
    {
        struct buf kdf = {0}, kps = {0}, prf = {0}, enc = {0}, ps2 = {0};
        if (p12_random(salt, 16) || p12_random(iv, 16)) return P12_ERR_RANDOM;
        PUT_OID(&a, oid_pbes2);
        PUT_OID(&kdf, oid_pbkdf2);
        put_tlv(&kps, T_OCTETS, salt, 16);
        put_uint(&kps, iter);
        PUT_OID(&prf, oid_hmac_sha256);
        put_tlv(&prf, T_NULL, NULL, 0);
        put_wrap(&kps, T_SEQ, &prf);
        put_wrap(&kdf, T_SEQ, &kps);
        put_wrap(&ps2, T_SEQ, &kdf);
        PUT_OID(&enc, oid_aes256_cbc);
        put_tlv(&enc, T_OCTETS, iv, 16);
        put_wrap(&ps2, T_SEQ, &enc);
        put_wrap(&a, T_SEQ, &ps2);
    }
    else
    {
        if (p12_random(salt, 8)) return P12_ERR_RANDOM;
        PUT_OID(&a, oid_pbe_3des);      /* Windows 10 uses 3-key 3DES for the certificates as well */
        put_tlv(&ps, T_OCTETS, salt, 8);
        put_uint(&ps, iter);
        put_wrap(&a, T_SEQ, &ps);
    }
    put_wrap(alg, T_SEQ, &a);
    return alg->err;
}

/* Encrypts 'plain' with a new PBE; *alg receives the AlgorithmIdentifier (full SEQUENCE). */
static int encrypt_blob(const struct p12_write_params *wp, const struct pwd *pw, int for_key, uint32_t iter,
                        const uint8_t *plain, size_t plen, struct buf *alg, uint8_t **ct, size_t *ctlen)
{
    struct budget bud = { P12_MAX_TOTAL_ITERATIONS };
    span s, inner;
    int r;

    if ((r = make_alg(wp, for_key, iter, alg))) return r;
    s.p = alg->p; s.n = alg->len;
    if ((r = get_seq(&s, &inner))) return r;
    return pbe_crypt(inner, pw, 1, plain, plen, ct, ctlen, &bud);
}

static int add_key_bag(struct buf *sc, const struct p12_write_params *wp, const struct pwd *pw, uint32_t iter,
                       const struct p12_item *k)
{
    struct buf alg = {0}, epki = {0}, bag = {0}, c0 = {0};
    uint8_t *ct = NULL;
    size_t ctlen = 0;
    int r = encrypt_blob(wp, pw, 1, iter, k->data, k->len, &alg, &ct, &ctlen);

    if (r) { b_free(&alg); return r; }
    b_add(&epki, alg.p, alg.len);
    b_free(&alg);
    put_tlv(&epki, T_OCTETS, ct, ctlen);
    free(ct);
    PUT_OID(&bag, oid_shroudedbag);
    put_wrap(&c0, T_SEQ, &epki);
    put_wrap(&bag, T_CTX0, &c0);
    put_bag_attrs(&bag, k);
    put_wrap(sc, T_SEQ, &bag);
    return sc->err;
}

static int add_cert_bag(struct buf *sc, const struct p12_item *c)
{
    struct buf cb = {0}, o = {0}, c0 = {0}, bag = {0};

    PUT_OID(&cb, oid_x509cert);
    put_tlv(&o, T_OCTETS, c->data, c->len);
    put_wrap(&cb, T_CTX0, &o);
    put_wrap(&c0, T_SEQ, &cb);
    PUT_OID(&bag, oid_certbag);
    put_wrap(&bag, T_CTX0, &c0);
    put_bag_attrs(&bag, c);
    put_wrap(sc, T_SEQ, &bag);
    return sc->err;
}

int p12_write(const struct p12_item *certs, size_t ncerts, const struct p12_item *keys, size_t nkeys,
              const uint16_t *password, const struct p12_write_params *params, uint8_t **out, size_t *out_len)
{
    struct p12_write_params wp = { 0, 0 };
    struct buf authsafe = {0}, pfx = {0}, result = {0};
    struct pwd pw;
    const struct halg *mac_alg;
    uint32_t iter;
    size_t i;
    int r;

    if (params) wp = *params;
    iter = wp.iterations ? wp.iterations : ((wp.flags & P12_WRITE_PBES2) ? P12_DEFAULT_ITER_PBES2 : P12_DEFAULT_ITER_V1);
    if (iter > P12_MAX_ITERATIONS) return P12_ERR_LIMIT;
    if ((r = pwd_init(&pw, password))) return r;
    mac_alg = (wp.flags & P12_WRITE_PBES2) ? &alg_sha256 : &alg_sha1;

    /* keys: shrouded bags in a plain "data" ContentInfo */
    if (nkeys)
    {
        struct buf sc = {0}, seq = {0}, os = {0}, ci = {0};
        for (i = 0; i < nkeys && !(r = add_key_bag(&sc, &wp, &pw, iter, &keys[i])); i++) ;
        if (r) { b_free(&sc); goto fail; }
        put_wrap(&seq, T_SEQ, &sc);
        if (!seq.err) put_tlv(&os, T_OCTETS, seq.p, seq.len);
        b_free(&seq);
        PUT_OID(&ci, oid_data);
        put_wrap(&ci, T_CTX0, &os);
        put_wrap(&authsafe, T_SEQ, &ci);
    }
    /* certificates: an encryptedData ContentInfo, or a plain one */
    if (ncerts)
    {
        struct buf sc = {0}, seq = {0}, alg = {0}, eci = {0}, ed = {0}, ci = {0}, c0 = {0};
        uint8_t *ct = NULL;
        size_t ctlen = 0;
        for (i = 0; i < ncerts && !(r = add_cert_bag(&sc, &certs[i])); i++) ;
        if (r) { b_free(&sc); goto fail; }
        put_wrap(&seq, T_SEQ, &sc);
        if (seq.err) { r = seq.err; b_free(&seq); goto fail; }
        if (wp.flags & P12_WRITE_PLAIN_CERTS)
        {
            struct buf os = {0};
            put_tlv(&os, T_OCTETS, seq.p, seq.len);
            b_free(&seq);
            PUT_OID(&ci, oid_data);
            put_wrap(&ci, T_CTX0, &os);
            put_wrap(&authsafe, T_SEQ, &ci);
        }
        else
        {
            r = encrypt_blob(&wp, &pw, 0, iter, seq.p, seq.len, &alg, &ct, &ctlen);
            b_free(&seq);
            if (r) { b_free(&alg); goto fail; }
            PUT_OID(&eci, oid_data);
            b_add(&eci, alg.p, alg.len);
            b_free(&alg);
            put_tlv(&eci, 0x80, ct, ctlen);
            free(ct);
            put_uint(&ed, 0);
            put_wrap(&ed, T_SEQ, &eci);
            put_wrap(&c0, T_SEQ, &ed);
            PUT_OID(&ci, oid_encdata);
            put_wrap(&ci, T_CTX0, &c0);
            put_wrap(&authsafe, T_SEQ, &ci);
        }
    }
    if (authsafe.err) { r = authsafe.err; goto fail; }
    {
        struct buf as = {0};
        put_tlv(&as, T_SEQ, authsafe.p, authsafe.len);
        b_free(&authsafe);
        authsafe = as;
        if (as.err) { r = as.err; goto fail; }
    }

    /* PFX */
    put_uint(&pfx, 3);
    {
        struct buf ci = {0}, os = {0};
        PUT_OID(&ci, oid_data);
        put_tlv(&os, T_OCTETS, authsafe.p, authsafe.len);
        put_wrap(&ci, T_CTX0, &os);
        put_wrap(&pfx, T_SEQ, &ci);
    }
    {
        uint8_t salt[20], key[32], mac[32];
        struct buf md = {0}, di = {0}, dalg = {0};
        if (p12_random(salt, sizeof(salt))) { r = P12_ERR_RANDOM; goto fail; }
        if ((r = p12_kdf(mac_alg, 3, pw.bmp, pw.bmp_len, salt, sizeof(salt), iter, key, mac_alg->outlen))) goto fail;
        hmac_oneshot(mac_alg, key, mac_alg->outlen, authsafe.p, authsafe.len, mac);
        p12_wipe(key, sizeof(key));
        if (mac_alg == &alg_sha1) PUT_OID(&dalg, oid_sha1); else PUT_OID(&dalg, oid_sha256);
        put_tlv(&dalg, T_NULL, NULL, 0);
        put_wrap(&di, T_SEQ, &dalg);
        put_tlv(&di, T_OCTETS, mac, mac_alg->outlen);
        put_wrap(&md, T_SEQ, &di);
        put_tlv(&md, T_OCTETS, salt, sizeof(salt));
        put_uint(&md, iter);
        put_wrap(&pfx, T_SEQ, &md);
    }
    put_wrap(&result, T_SEQ, &pfx);
    if (result.err) { r = result.err; goto done_err; }
    *out = result.p; *out_len = result.len;
    b_free(&authsafe); pwd_free(&pw);
    return P12_OK;
fail:
    b_free(&pfx);
done_err:
    b_free(&result); b_free(&authsafe); pwd_free(&pw);
    return r;
}
