/* zliblite implementation - see zliblite.h */
#include "zliblite.h"
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* shared length/distance tables                                        */
/* ------------------------------------------------------------------ */

static const unsigned short ZL_LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,
    115,131,163,195,227,258
};
static const unsigned char ZL_LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const unsigned short ZL_DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const unsigned char ZL_DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static unsigned zl_adler32(const unsigned char *p, int len)
{
    unsigned a = 1, b = 0;
    while (len > 0) {
        int n = len > 5552 ? 5552 : len;
        len -= n;
        while (n--) { a += *p++; b += a; }
        a %= 65521u; b %= 65521u;
    }
    return (b << 16) | a;
}

/* ------------------------------------------------------------------ */
/* compressor                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *buf;
    int cap, len;
    unsigned acc;
    int nbits;
} ZlBW;

static int bw_byte(ZlBW *w, unsigned v, int n) /* LSB-first bits */
{
    w->acc |= v << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        if (w->len >= w->cap) return 0;
        w->buf[w->len++] = (unsigned char)(w->acc & 0xFF);
        w->acc >>= 8;
        w->nbits -= 8;
    }
    return 1;
}

static int bw_flush(ZlBW *w)
{
    if (w->nbits > 0) {
        if (w->len >= w->cap) return 0;
        w->buf[w->len++] = (unsigned char)(w->acc & 0xFF);
        w->acc = 0;
        w->nbits = 0;
    }
    return 1;
}

static unsigned zl_rev(unsigned v, int n)
{
    unsigned r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

/* fixed-huffman literal/length code (MSB-first emission) */
static int put_lit(ZlBW *w, int sym)
{
    unsigned code;
    int n;
    if (sym < 144)       { code = 0x30u + (unsigned)sym;        n = 8; }
    else if (sym < 256)  { code = 0x190u + (unsigned)sym - 144; n = 9; }
    else if (sym < 280)  { code = (unsigned)sym - 256;          n = 7; }
    else                 { code = 0xC0u + (unsigned)sym - 280;  n = 8; }
    return bw_byte(w, zl_rev(code, n), n);
}

static int put_dsym(ZlBW *w, int dsym)
{
    return bw_byte(w, zl_rev((unsigned)dsym, 5), 5);
}

#define ZL_WSIZE  32768
#define ZL_HSIZE  32768            /* 15-bit hash of 3 bytes */
#define ZL_MINM   3
#define ZL_MAXM   258
#define ZL_MAXC   1024             /* hash chain budget per position */

static unsigned zl_hash3(const unsigned char *p)
{
    return (unsigned)(((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5)
                      ^ (unsigned)p[2]) & (ZL_HSIZE - 1);
}

static int zl_lz(ZlBW *w, const unsigned char *src, int len,
                 int *head, int *prev)
{
    int pos = 0;
    while (pos < len) {
        int bestLen = 0, bestDist = 0;
        if (pos + ZL_MINM <= len) {
            unsigned h = zl_hash3(src + pos);
            int cand = head[h];
            int limit = pos - ZL_WSIZE;
            if (limit < 0) limit = 0;
            int chain = ZL_MAXC;
            int maxm = len - pos;
            if (maxm > ZL_MAXM) maxm = ZL_MAXM;
            while (cand >= limit && cand >= 0 && chain-- > 0) {
                if (src[cand + bestLen] == src[pos + bestLen]) {
                    int l = 0;
                    while (l < maxm && src[cand + l] == src[pos + l]) l++;
                    if (l > bestLen) {
                        bestLen = l;
                        bestDist = pos - cand;
                        if (l >= maxm) break;
                    }
                }
                cand = prev[cand & (ZL_WSIZE - 1)];
            }
        }
        if (bestLen >= ZL_MINM) {
            int ls = 28;
            while (ls > 0 && ZL_LEN_BASE[ls] > bestLen) ls--;
            if (!put_lit(w, 257 + ls)) return 0;
            int eb = ZL_LEN_EXTRA[ls];
            if (eb && !bw_byte(w, (unsigned)(bestLen - ZL_LEN_BASE[ls]), eb))
                return 0;
            int ds = 29;
            while (ds > 0 && ZL_DIST_BASE[ds] > bestDist) ds--;
            if (!put_dsym(w, ds)) return 0;
            int db = ZL_DIST_EXTRA[ds];
            if (db && !bw_byte(w, (unsigned)(bestDist - ZL_DIST_BASE[ds]), db))
                return 0;
            for (int k = 0; k < bestLen; k++) {
                if (pos + k + ZL_MINM <= len) {
                    unsigned h = zl_hash3(src + pos + k);
                    prev[(pos + k) & (ZL_WSIZE - 1)] = head[h];
                    head[h] = pos + k;
                }
            }
            pos += bestLen;
        } else {
            if (!put_lit(w, src[pos])) return 0;
            if (pos + ZL_MINM <= len) {
                unsigned h = zl_hash3(src + pos);
                prev[pos & (ZL_WSIZE - 1)] = head[h];
                head[h] = pos;
            }
            pos++;
        }
    }
    return put_lit(w, 256); /* end of block */
}

int zl_compress(unsigned char *dst, int dstCap,
                const unsigned char *src, int len)
{
    if (len < 0 || dstCap < len + 11) return 0;
    dst[0] = 0x78;
    dst[1] = 0x01;

    /* try a real fixed-huffman deflate block first */
    if (len > 0) {
        int *head = (int *)malloc(ZL_HSIZE * sizeof(int));
        int *prev = (int *)malloc(ZL_WSIZE * sizeof(int));
        if (head && prev) {
            for (int i = 0; i < ZL_HSIZE; i++) head[i] = -1;
            ZlBW w;
            w.buf = dst; w.cap = dstCap - 4; w.len = 2;
            w.acc = 0; w.nbits = 0;
            if (bw_byte(&w, 1, 1) && bw_byte(&w, 1, 2)
                && zl_lz(&w, src, len, head, prev) && bw_flush(&w)) {
                free(head); free(prev);
                unsigned ad = zl_adler32(src, len);
                dst[w.len++] = (unsigned char)(ad >> 24);
                dst[w.len++] = (unsigned char)(ad >> 16);
                dst[w.len++] = (unsigned char)(ad >> 8);
                dst[w.len++] = (unsigned char)ad;
                return w.len;
            }
        }
        free(head); free(prev);
    }

    /* stored fallback (incompressible / alloc failure / overflow) */
    int o = 2, off = 0;
    do {
        int chunk = len - off;
        if (chunk > 65535) chunk = 65535;
        int final = (off + chunk >= len);
        dst[o++] = (unsigned char)(final ? 1 : 0);
        dst[o++] = (unsigned char)(chunk & 0xFF);
        dst[o++] = (unsigned char)((chunk >> 8) & 0xFF);
        dst[o++] = (unsigned char)(~chunk & 0xFF);
        dst[o++] = (unsigned char)((~chunk >> 8) & 0xFF);
        for (int i = 0; i < chunk; i++) dst[o++] = src[off + i];
        off += chunk;
    } while (off < len);
    unsigned ad = zl_adler32(src, len);
    dst[o++] = (unsigned char)(ad >> 24);
    dst[o++] = (unsigned char)(ad >> 16);
    dst[o++] = (unsigned char)(ad >> 8);
    dst[o++] = (unsigned char)ad;
    return o;
}

/* ------------------------------------------------------------------ */
/* inflate (stored / fixed / dynamic)                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const unsigned char *src;
    int srclen, pos;
    unsigned acc;
    int nbits;
    unsigned char *dst;
    int dstCap, dlen;
    int err;
} ZlBR;

static int br_bit(ZlBR *r)
{
    if (r->nbits == 0) {
        if (r->pos >= r->srclen) { r->err = 1; return 0; }
        r->acc = r->src[r->pos++];
        r->nbits = 8;
    }
    int b = (int)(r->acc & 1);
    r->acc >>= 1;
    r->nbits--;
    return b;
}

static unsigned br_bits(ZlBR *r, int n) /* LSB-first multi-bit field */
{
    unsigned v = 0;
    for (int i = 0; i < n; i++) v |= (unsigned)br_bit(r) << i;
    return v;
}

typedef struct {
    short count[16];
    short sym[288];
} ZlHuff;

static void huff_build(ZlHuff *h, const unsigned char *lens, int n)
{
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    short offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; i++)
        offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++)
        if (lens[i]) h->sym[offs[lens[i]]++] = (short)i;
}

static int huff_decode(ZlBR *r, const ZlHuff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= br_bit(r);
        if (r->err) return -1;
        int count = h->count[len];
        if (code - first < count) return h->sym[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    r->err = 1;
    return -1;
}

static int inflate_block(ZlBR *r, const ZlHuff *lh, const ZlHuff *dh)
{
    for (;;) {
        int sym = huff_decode(r, lh);
        if (sym < 0) return 0;
        if (sym < 256) {
            if (r->dlen >= r->dstCap) { r->err = 1; return 0; }
            r->dst[r->dlen++] = (unsigned char)sym;
        } else if (sym == 256) {
            return 1;
        } else {
            sym -= 257;
            if (sym >= 29) { r->err = 1; return 0; }
            int len = ZL_LEN_BASE[sym] + (int)br_bits(r, ZL_LEN_EXTRA[sym]);
            int ds = huff_decode(r, dh);
            if (ds < 0 || ds >= 30) { r->err = 1; return 0; }
            int dist = ZL_DIST_BASE[ds] + (int)br_bits(r, ZL_DIST_EXTRA[ds]);
            if (dist > r->dlen || len > r->dstCap - r->dlen) {
                r->err = 1;
                return 0;
            }
            while (len--) {
                r->dst[r->dlen] = r->dst[r->dlen - dist];
                r->dlen++;
            }
        }
        if (r->err) return 0;
    }
}

int zl_decompress(const unsigned char *src, int srclen,
                  unsigned char *dst, int dstCap)
{
    if (srclen < 6 || dstCap < 0) return -1;
    if ((src[0] & 0x0F) != 8) return -1;               /* CM = deflate */
    if ((((unsigned)src[0] << 8) | src[1]) % 31 != 0) return -1;
    ZlBR r;
    r.src = src; r.srclen = srclen; r.pos = 2;
    r.acc = 0; r.nbits = 0;
    r.dst = dst; r.dstCap = dstCap; r.dlen = 0;
    r.err = 0;

    int final = 0;
    while (!final) {
        final = br_bit(&r);
        int type = (int)br_bits(&r, 2);
        if (r.err) return -1;
        if (type == 0) {
            r.nbits = 0;                     /* align to byte boundary */
            if (r.pos + 4 > r.srclen) return -1;
            unsigned len = (unsigned)src[r.pos] | ((unsigned)src[r.pos + 1] << 8);
            unsigned nlen = (unsigned)src[r.pos + 2] | ((unsigned)src[r.pos + 3] << 8);
            r.pos += 4;
            if ((len ^ 0xFFFFu) != nlen) return -1;
            if (r.pos + (int)len > r.srclen
                || r.dlen + (int)len > dstCap) return -1;
            for (unsigned i = 0; i < len; i++)
                dst[r.dlen++] = src[r.pos++];
        } else if (type == 1) {
            unsigned char lens[288];
            int n = 0;
            for (int i = 0; i < 144; i++) lens[n++] = 8;
            for (int i = 0; i < 112; i++) lens[n++] = 9;
            for (int i = 0; i < 24; i++) lens[n++] = 7;
            for (int i = 0; i < 8; i++) lens[n++] = 8;
            ZlHuff lh, dh;
            huff_build(&lh, lens, 288);
            unsigned char dl[30];
            for (int i = 0; i < 30; i++) dl[i] = 5;
            huff_build(&dh, dl, 30);
            if (!inflate_block(&r, &lh, &dh)) return -1;
        } else if (type == 2) {
            static const unsigned char ORD[19] = {
                16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
            };
            int hlit = (int)br_bits(&r, 5) + 257;
            int hdist = (int)br_bits(&r, 5) + 1;
            int hclen = (int)br_bits(&r, 4) + 4;
            if (r.err) return -1;
            unsigned char cl[19];
            for (int i = 0; i < 19; i++) cl[i] = 0;
            for (int i = 0; i < hclen; i++) cl[ORD[i]] = (unsigned char)br_bits(&r, 3);
            ZlHuff ch;
            huff_build(&ch, cl, 19);
            unsigned char lens[320];
            int n = 0;
            while (n < hlit + hdist) {
                int s = huff_decode(&r, &ch);
                if (s < 0) return -1;
                if (s < 16) {
                    lens[n++] = (unsigned char)s;
                } else if (s == 16) {
                    if (n == 0) return -1;
                    unsigned char p = lens[n - 1];
                    int rep = 3 + (int)br_bits(&r, 2);
                    while (rep-- && n < hlit + hdist) lens[n++] = p;
                } else if (s == 17) {
                    int rep = 3 + (int)br_bits(&r, 3);
                    while (rep-- && n < hlit + hdist) lens[n++] = 0;
                } else {
                    int rep = 11 + (int)br_bits(&r, 7);
                    while (rep-- && n < hlit + hdist) lens[n++] = 0;
                }
            }
            ZlHuff lh, dh;
            huff_build(&lh, lens, hlit);
            huff_build(&dh, lens + hlit, hdist);
            if (!inflate_block(&r, &lh, &dh)) return -1;
        } else {
            return -1;
        }
        if (r.err) return -1;
    }
    /* trailer adler32 check */
    if (r.pos + 4 > r.srclen) return -1;
    unsigned ad = ((unsigned)src[r.pos] << 24) | ((unsigned)src[r.pos + 1] << 16)
                | ((unsigned)src[r.pos + 2] << 8) | (unsigned)src[r.pos + 3];
    if (ad != zl_adler32(dst, r.dlen)) return -1;
    return r.dlen;
}
