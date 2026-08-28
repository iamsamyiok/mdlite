/* zliblite - compact zlib (RFC1950/1951) compressor + full inflate.
 * Used for git loose-object storage: emits real deflate streams so the
 * objects stay readable by standard git/zlib tooling, while decompress
 * accepts stored, fixed and dynamic blocks (including all legacy
 * stored-block objects written by earlier MDLite versions).
 * Plain C99, no dependencies - also testable on POSIX hosts. */
#ifndef MDLITE_ZLIBLITE_H
#define MDLITE_ZLIBLITE_H

/* compress src into dst as a zlib stream (fixed-huffman deflate with
 * greedy lz77 matching; falls back to stored blocks when the output
 * would not fit). returns stream length, or 0 when dstCap is too
 * small even for the stored fallback (needs len + 11). */
int zl_compress(unsigned char *dst, int dstCap,
                const unsigned char *src, int len);

/* inflate a zlib stream body (stored / fixed / dynamic blocks).
 * returns body length, or -1 on any error (bad stream, bad adler32,
 * output larger than dstCap). */
int zl_decompress(const unsigned char *src, int srclen,
                  unsigned char *dst, int dstCap);

#endif
