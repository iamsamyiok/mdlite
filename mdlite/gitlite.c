/* MDLite - git snapshot engine (loose objects, standard-readable) */
#define _WIN32_WINNT 0x0601
#include "mdlite.h"
#include "zliblite.h"
#include <stdlib.h>
#include <string.h>

BOOL WriteAllBytes(const wchar_t *path, const char *buf, int len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, buf, len, &wr, NULL) && (int)wr == len;
    CloseHandle(h);
    return ok;
}

BOOL ReadAllBytes(const wchar_t *path, char **buf, int *len)
{
    *buf = NULL;
    *len = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 32u * 1024 * 1024) {
        CloseHandle(h);
        return FALSE;
    }
    char *u8 = (char *)malloc(size ? size : 1);
    DWORD rd = 0;
    BOOL ok = ReadFile(h, u8, size, &rd, NULL);
    CloseHandle(h);
    if (!ok) { free(u8); return FALSE; }
    *buf = u8;
    *len = (int)rd;
    return TRUE;
}
/* dir = folder of g_path + \历史 (legacy layout, kept for one-time migration) */
BOOL BuildHistoryDir(wchar_t *dir, int cch)
{
    if (!g_path[0]) return FALSE;
    const wchar_t *slash = wcsrchr(g_path, L'\\');
    const wchar_t *slash2 = wcsrchr(g_path, L'/');
    if (slash2 > slash) slash = slash2;
    if (!slash) return FALSE;
    int dlen = (int)(slash - g_path);
    if (dlen <= 0 || dlen + 12 >= cch) return FALSE;
    memcpy(dir, g_path, dlen * sizeof(wchar_t));
    dir[dlen] = 0;
    lstrcpynW(dir + dlen, L"\\历史", cch - dlen);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* mini git - real loose-object repo, readable by standard git tools   */
/* repo layout:  <docdir>\.mdlite-git\{objects,refs,HEAD,config}       */
/* objects are zlib "stored" blocks (valid, uncompressed)             */
/* ------------------------------------------------------------------ */

static BOOL Sha1Buf(const BYTE *p, int len, BYTE out[20])
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    BOOL ok = FALSE;
    if (CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) {
            if (CryptHashData(hash, p, len, 0)) {
                DWORD hl = 20;
                ok = CryptGetHashParam(hash, HP_HASHVAL, out, &hl, 0)
                     && hl == 20;
            }
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(prov, 0);
    }
    return ok;
}

static void ShaHex(const BYTE sha[20], char hex[41])
{
    static const char HC[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        hex[i * 2]     = HC[sha[i] >> 4];
        hex[i * 2 + 1] = HC[sha[i] & 15];
    }
    hex[40] = 0;
}

/* pure-ascii char* -> wchar_t* (object names are always hex) */
static void AsciiToWide(const char *s, wchar_t *w, int cch)
{
    int i;
    for (i = 0; s[i] && i < cch - 1; i++)
        w[i] = (wchar_t)(BYTE)s[i];
    w[i] = 0;
}

/* sha1 of "<hdr>\0<body>" without touching disk */
static BOOL HashObject(const char *hdr, const char *body, int bodylen,
                       BYTE sha[20], char hexOut[41])
{
    int hdrlen = lstrlenA(hdr);
    int total = hdrlen + 1 + bodylen;
    BYTE *raw = (BYTE *)malloc(total);
    if (!raw) return FALSE;
    memcpy(raw, hdr, hdrlen);
    raw[hdrlen] = 0;
    if (bodylen) memcpy(raw + hdrlen + 1, body, bodylen);
    BOOL ok = Sha1Buf(raw, total, sha);
    free(raw);
    if (ok) ShaHex(sha, hexOut);
    return ok;
}

/* write object "<hdr>\0<body>" as loose object; fills sha + hex */
static BOOL WriteLoose(const wchar_t *repo, const char *hdr,
                       const char *body, int bodylen,
                       BYTE sha[20], char shaHexOut[41])
{
    if (!HashObject(hdr, body, bodylen, sha, shaHexOut)) return FALSE;

    wchar_t objDir[MAX_PATH + 96], objPath[MAX_PATH + 128], tmpPath[MAX_PATH + 128];
    wsprintfW(objDir,  L"%s\\objects\\%c%c", repo, shaHexOut[0], shaHexOut[1]);
    wchar_t tailW[48];
    AsciiToWide(shaHexOut + 2, tailW, 48);
    wsprintfW(objPath, L"%s\\%s", objDir, tailW);
    if (GetFileAttributesW(objPath) != INVALID_FILE_ATTRIBUTES)
        return TRUE; /* already known - content dedup for free */
    CreateDirectoryW(objDir, NULL);

    int total = lstrlenA(hdr) + 1 + bodylen;
    BYTE *raw = (BYTE *)malloc(total);
    if (!raw) return FALSE;
    memcpy(raw, hdr, lstrlenA(hdr));
    raw[lstrlenA(hdr)] = 0;
    if (bodylen) memcpy(raw + lstrlenA(hdr) + 1, body, bodylen);
    /* bound covers fixed-huffman worst case (9/8) + zlib framing */
    BYTE *z = (BYTE *)malloc(total + total / 8 + 256);
    if (!z) { free(raw); return FALSE; }
    int zlen = zl_compress(z, total + total / 8 + 256, raw, total);
    free(raw);
    if (zlen <= 0) return FALSE;

    wsprintfW(tmpPath, L"%s\\tmp_%s", objDir, tailW);
    BOOL ok = FALSE;
    if (WriteAllBytes(tmpPath, (const char *)z, zlen))
        ok = MoveFileExW(tmpPath, objPath, MOVEFILE_REPLACE_EXISTING);
    free(z);
    return ok;
}

/* read loose object, returns malloc'd body and length (header skipped) */
BOOL ReadLoose(const wchar_t *repo, const char *hex,
                      char **body, int *bodylen)
{
    *body = NULL;
    *bodylen = 0;
    wchar_t path[MAX_PATH + 128];
    wchar_t dirW[8], restW[48];
    AsciiToWide(hex, dirW, 3);
    AsciiToWide(hex + 2, restW, 48);
    wsprintfW(path, L"%s\\objects\\%s\\%s", repo, dirW, restW);
    char *raw = NULL;
    int rawlen = 0;
    if (!ReadAllBytes(path, &raw, &rawlen)) return FALSE;

    /* compressed objects expand; retry with a growing buffer */
    int cap = rawlen + 65536;
    BYTE *plain = NULL;
    int plen = -1;
    while (cap <= 384 * 1024 * 1024) {
        plain = (BYTE *)malloc(cap);
        if (!plain) break;
        plen = zl_decompress((const BYTE *)raw, rawlen, plain, cap);
        if (plen >= 0) break;
        free(plain);
        plain = NULL;
        cap *= 2;
    }
    free(raw);
    if (plen < 0 || !plain) { free(plain); return FALSE; }

    /* header: "<type> <len>\0" */
    int nul = -1;
    for (int i = 0; i < plen; i++)
        if (plain[i] == 0) { nul = i; break; }
    if (nul < 0) { free(plain); return FALSE; }
    int declared = 0;
    for (int i = 0; i < nul; i++) {
        if (plain[i] < '0' || plain[i] > '9') continue;
        declared = declared * 10 + (plain[i] - '0');
    }
    if (declared != plen - nul - 1) { free(plain); return FALSE; }
    memmove(plain, plain + nul + 1, declared);
    *body = (char *)plain;
    *bodylen = declared;
    return TRUE;
}

BOOL RepoRefPath(const wchar_t *repo, wchar_t *path, int cch)
{
    lstrcpynW(path, repo, cch);
    lstrcpynW(path + lstrlenW(path), L"\\refs\\heads\\master", cch - lstrlenW(path));
    return TRUE;
}

BOOL ReadMaster(const wchar_t *repo, char hex[41])
{
    hex[0] = 0;
    wchar_t path[MAX_PATH + 128];
    RepoRefPath(repo, path, MAX_PATH + 128);
    char *raw = NULL;
    int len = 0;
    if (!ReadAllBytes(path, &raw, &len)) return FALSE;
    if (len > 40) len = 40;
    int i;
    for (i = 0; i < len; i++)
        if (raw[i] == '\n' || raw[i] == '\r') break;
    len = i;
    if (len != 40) { free(raw); return FALSE; }
    memcpy(hex, raw, 40);
    hex[40] = 0;
    free(raw);
    return TRUE;
}

BOOL WriteMaster(const wchar_t *repo, const char *hex)
{
    char body[48];
    wsprintfA(body, "%s\n", hex);
    wchar_t path[MAX_PATH + 128];
    RepoRefPath(repo, path, MAX_PATH + 128);
    return WriteAllBytes(path, body, lstrlenA(body));
}

/* FNV-1a 64-bit hex of a document path: repo folder name (12 chars).
 * Case-folded like PathHash so paths match Windows semantics. */
static unsigned long long RepoNameHash(const wchar_t *path)
{
    unsigned long long h = 1469598103934665603ull;
    for (const wchar_t *p = path; *p; p++) {
        wchar_t c = *p;
        if (c >= L'A' && c <= L'Z') c += 32;
        h ^= (unsigned long long)c;
        h *= 1099511628211ull;
    }
    return h;
}

/* repo root: <exedir>\.mdlite\history\<path-hash> -- every document's
 * snapshot history lives in one dedicated folder beside the exe,
 * independent of where the document itself is stored. */
BOOL BuildRepoDir(wchar_t *repo, int cch)
{
    if (!g_path[0]) return FALSE;
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return FALSE;
    const wchar_t *slash = wcsrchr(exe, L'\\');
    const wchar_t *slash2 = wcsrchr(exe, L'/');
    if (slash2 > slash) slash = slash2;
    if (!slash) return FALSE;
    int dlen = (int)(slash - exe);
    if (dlen <= 0 || dlen + 40 >= cch) return FALSE;
    unsigned long long h = RepoNameHash(g_path);
    wchar_t hash[20];
    wsprintfW(hash, L"%08X%08X", (DWORD)(h >> 32), (DWORD)h);
    memcpy(repo, exe, dlen * sizeof(wchar_t));
    repo[dlen] = 0;
    lstrcpynW(repo + dlen, L"\\.mdlite\\history\\", cch - dlen);
    lstrcpynW(repo + lstrlenW(repo), hash, cch - lstrlenW(repo));
    return TRUE;
}

/* legacy layout: <docdir>\.mdlite-git (repo sat next to the document) */
static BOOL BuildOldRepoDir(wchar_t *repo, int cch)
{
    if (!g_path[0]) return FALSE;
    const wchar_t *slash = wcsrchr(g_path, L'\\');
    const wchar_t *slash2 = wcsrchr(g_path, L'/');
    if (slash2 > slash) slash = slash2;
    if (!slash) return FALSE;
    int dlen = (int)(slash - g_path);
    if (dlen <= 0 || dlen + 16 >= cch) return FALSE;
    memcpy(repo, g_path, dlen * sizeof(wchar_t));
    repo[dlen] = 0;
    lstrcpynW(repo + dlen, L"\\.mdlite-git", cch - dlen);
    return TRUE;
}

static BOOL MkParentDirs(const wchar_t *repo)
{
    /* create the three ancestors: .mdlite, .mdlite\history, <repo> */
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return FALSE;
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;
    wchar_t d1[MAX_PATH + 128], d2[MAX_PATH + 128];
    wsprintfW(d1, L"%s\\.mdlite", exe);
    wsprintfW(d2, L"%s\\history", d1);
    CreateDirectoryW(d1, NULL);
    CreateDirectoryW(d2, NULL);
    CreateDirectoryW(repo, NULL);
    return GetFileAttributesW(repo) != INVALID_FILE_ATTRIBUTES;
}

/* recursive copy of a loose-object repo (cross-volume migration path) */
static BOOL CopyTree(const wchar_t *src, const wchar_t *dst)
{
    if (!CreateDirectoryW(dst, NULL)) return FALSE;
    wchar_t pat[MAX_PATH + 128], s[MAX_PATH + 128], d[MAX_PATH + 128];
    wsprintfW(pat, L"%s\\*.*", src);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return TRUE; /* empty dir is fine */
    BOOL ok = TRUE;
    do {
        if (lstrcmpW(fd.cFileName, L".") == 0
            || lstrcmpW(fd.cFileName, L"..") == 0) continue;
        wsprintfW(s, L"%s\\%s", src, fd.cFileName);
        wsprintfW(d, L"%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!CopyTree(s, d)) ok = FALSE;
        } else {
            char *buf = NULL;
            int len = 0;
            if (ReadAllBytes(s, &buf, &len)) {
                if (!WriteAllBytes(d, buf, len)) ok = FALSE;
                free(buf);
            } else ok = FALSE;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* one-time move of a legacy doc-side ".mdlite-git" repo into the unified
 * exe-side store; on success the old folder is renamed away (kept as a
 * backup when a plain move is impossible, e.g. across volumes) */
static void MigrateOldRepo(const wchar_t *repo)
{
    char hex[41];
    if (ReadMaster(repo, hex)) return;          /* new store already live */
    wchar_t old[MAX_PATH];
    if (!BuildOldRepoDir(old, MAX_PATH)) return;
    if (GetFileAttributesW(old) == INVALID_FILE_ATTRIBUTES) return;
    if (!MkParentDirs(repo)) return;

    if (MoveFileW(old, repo)) return;           /* same volume: instant */
    if (!CopyTree(old, repo)) return;
    wchar_t bak[MAX_PATH];
    lstrcpynW(bak, old, MAX_PATH);
    lstrcpynW(bak + lstrlenW(bak), L"_迁移备份", MAX_PATH - lstrlenW(bak));
    MoveFileW(old, bak);
}

static BOOL EnsureRepo(const wchar_t *repo)
{
    wchar_t sub[MAX_PATH + 128];
    wsprintfW(sub, L"%s\\refs\\heads", repo);
    if (GetFileAttributesW(sub) == INVALID_FILE_ATTRIBUTES) {
        wchar_t objs[MAX_PATH + 128], refs[MAX_PATH + 128];
        wsprintfW(objs, L"%s\\objects", repo);
        wsprintfW(refs, L"%s\\refs", repo);
        CreateDirectoryW(repo, NULL);
        CreateDirectoryW(objs, NULL);
        CreateDirectoryW(refs, NULL);
        CreateDirectoryW(sub, NULL);
        if (GetFileAttributesW(sub) == INVALID_FILE_ATTRIBUTES) return FALSE;
    }
    wsprintfW(sub, L"%s\\HEAD", repo);
    if (GetFileAttributesW(sub) == INVALID_FILE_ATTRIBUTES)
        WriteAllBytes(sub, "ref: refs/heads/master\n", 23);
    wsprintfW(sub, L"%s\\config", repo);
    if (GetFileAttributesW(sub) == INVALID_FILE_ATTRIBUTES) {
        static const char CFG[] =
            "[core]\n\trepositoryformatversion = 0\n\tfilemode = false\n"
            "\tbare = true\n";
        WriteAllBytes(sub, CFG, sizeof(CFG) - 1);
    }
    return TRUE;
}

/* wsprintf has no %lld - format a 64-bit value by hand */
static void FmtLL(char *out, LONGLONG v)
{
    char tmp[24];
    int n = 0;
    unsigned long long uv = (v < 0)
        ? (unsigned long long)(-v) : (unsigned long long)v;
    do {
        tmp[n++] = (char)('0' + (int)(uv % 10));
        uv /= 10;
    } while (uv);
    if (v < 0) *out++ = '-';
    while (n) *out++ = tmp[--n];
    *out = 0;
}

/* local timezone offset in minutes east of UTC */
static int TzOffsetMin(void)
{
    TIME_ZONE_INFORMATION tzi;
    DWORD r = GetTimeZoneInformation(&tzi);
    int bias = tzi.Bias;
    if (r == TIME_ZONE_ID_DAYLIGHT) bias += tzi.DaylightBias;
    return -bias;
}

static LONGLONG LocalUnixNow(void)
{
    /* true UTC seconds (display side applies the local timezone) */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (LONGLONG)(u.QuadPart / 10000000) - 11644473600LL;
}

/* "YYYYMMDD_HHMMSS" (local time) -> unix seconds; -1 on bad input */
static LONGLONG StampToUnix(const wchar_t *s)
{
    int v[6] = { 0 }, vi = 0;
    for (int j = 0; s[j] && vi < 6; ) {
        if (s[j] >= L'0' && s[j] <= L'9') {
            int want = (vi == 0) ? 4 : 2, got = 0;
            while (s[j] >= L'0' && s[j] <= L'9' && got < want) {
                v[vi] = v[vi] * 10 + (s[j] - L'0');
                j++; got++;
            }
            if (got != want) return -1;
            vi++;
        } else j++;
    }
    if (vi != 6) return -1;
    if (v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31) return -1;
    SYSTEMTIME st = { 0 };
    st.wYear = (WORD)v[0]; st.wMonth = (WORD)v[1]; st.wDay = (WORD)v[2];
    st.wHour = (WORD)v[3]; st.wMinute = (WORD)v[4]; st.wSecond = (WORD)v[5];
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return -1;
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    LONGLONG unixSec = (LONGLONG)(u.QuadPart / 10000000) - 11644473600LL
                       - (LONGLONG)TzOffsetMin() * 60;
    return unixSec;
}

/* write blob+tree+commit; assumes repo exists. returns commit hex.
 * msgPrefix: "保存 " / "自动保存 "; parentOvr: NULL=chain to master,
 * ""=root commit (auto overwrite of the only entry), else hex override. */
static BOOL WriteSnapshot(const wchar_t *repo, const char *u8, int u8len,
                          LONGLONG unixSec, const char *msgPrefix,
                          const char *parentOvr, char commitHex[41])
{
    /* blob */
    char hdr[32];
    wsprintfA(hdr, "blob %d", u8len);
    BYTE sha[20];
    char blobHex[41];
    if (!WriteLoose(repo, hdr, u8, u8len, sha, blobHex)) return FALSE;

    /* tree: one entry "100644 <utf8name>\0<blob-sha-raw>" */
    char name8[MAX_PATH * 3];
    int n8 = WideCharToMultiByte(CP_UTF8, 0, g_name, -1, name8,
                                 MAX_PATH * 3, NULL, NULL);
    if (n8 <= 0) return FALSE;
    n8--; /* drop NUL count */
    char entry[MAX_PATH * 3 + 32];
    memcpy(entry, "100644 ", 7);
    memcpy(entry + 7, name8, n8);
    entry[7 + n8] = 0;
    memcpy(entry + 8 + n8, sha, 20);
    int elen = 8 + n8 + 20;
    wsprintfA(hdr, "tree %d", elen);
    char treeHex[41];
    if (!WriteLoose(repo, hdr, entry, elen, sha, treeHex)) return FALSE;

    /* commit */
    char parentHex[41];
    BOOL haveParent;
    if (parentOvr) {
        haveParent = parentOvr[0] != 0;
        if (haveParent) lstrcpynA(parentHex, parentOvr, 41);
    } else {
        haveParent = ReadMaster(repo, parentHex);
    }
    if (unixSec == 0) unixSec = LocalUnixNow();
    int tz = TzOffsetMin();
    char unixStr[24];
    FmtLL(unixStr, unixSec);
    char msg[MAX_PATH * 3 + 24];
    int mlen = wsprintfA(msg, "\n\n%s%s\n", msgPrefix, name8);
    int cap = 320 + mlen;
    char *body = (char *)malloc(cap);
    if (!body) return FALSE;
    int o = 0;
    o += wsprintfA(body + o, "tree %s\n", treeHex);
    if (haveParent) o += wsprintfA(body + o, "parent %s\n", parentHex);
    int tzh = tz / 60, tzm = tz % 60;
    char sign = (tz >= 0) ? '+' : '-';
    if (tzh < 0) tzh = -tzh;
    if (tzm < 0) tzm = -tzm;
    o += wsprintfA(body + o,
                   "author MDLite <mdlite@local> %s %c%02d%02d\n",
                   unixStr, sign, tzh, tzm);
    o += wsprintfA(body + o,
                   "committer MDLite <mdlite@local> %s %c%02d%02d\n",
                   unixStr, sign, tzh, tzm);
    memcpy(body + o, msg, mlen);
    o += mlen;
    wsprintfA(hdr, "commit %d", o);
    BOOL ok = WriteLoose(repo, hdr, body, o, sha, commitHex);
    free(body);
    if (!ok) return FALSE;
    return WriteMaster(repo, commitHex);
}

/* rebuild a commit with a different parent, preserving tree / author /
 * committer / message verbatim. newParent NULL or "" -> root commit.
 * writes the new commit object (loose) and returns its hex; does NOT
 * touch refs/heads/master -- caller rewrites the tip afterwards. */
const char *CommitLine(const char *body, int blen,
                              const char *prefix, int plen, int *vlen);

BOOL CloneCommit(const wchar_t *repo, const char *origHex,
                        const char *newParent, char outHex[41])
{
    char *body = NULL;
    int blen = 0;
    if (!ReadLoose(repo, origHex, &body, &blen)) return FALSE;
    /* find "author " line start: everything before it is tree/parent lines */
    const char *auth = NULL;
    for (int i = 0; i + 7 <= blen; i++) {
        if (body[i] == 'a'
            && memcmp(body + i, "author ", 7) == 0
            && (i == 0 || body[i - 1] == '\n')) {
            auth = body + i;
            break;
        }
    }
    if (!auth) { free(body); return FALSE; }
    /* tree line: "<tree value>" 40 hex */
    int tl = 0;
    const char *t = CommitLine(body, blen, "tree ", 5, &tl);
    if (!t || tl != 40) { free(body); return FALSE; }
    char treeHex[41];
    memcpy(treeHex, t, 40);
    treeHex[40] = 0;

    /* tail = from author line to end (author, committer, message) */
    int tailLen = (int)((body + blen) - auth);

    int needParent = (newParent && newParent[0] != 0);
    int cap = 64 + (needParent ? 48 : 0) + tailLen;
    char *nb = (char *)malloc(cap);
    if (!nb) { free(body); return FALSE; }
    int o = 0;
    o += wsprintfA(nb + o, "tree %s\n", treeHex);
    if (needParent) o += wsprintfA(nb + o, "parent %s\n", newParent);
    memcpy(nb + o, auth, tailLen);
    o += tailLen;
    char hdr[32];
    wsprintfA(hdr, "commit %d", o);
    BYTE sha[20];
    BOOL ok = WriteLoose(repo, hdr, nb, o, sha, outHex);
    free(nb);
    free(body);
    return ok;
}

/* parse "<prefix>" line value from a commit body; returns ptr+len */
const char *CommitLine(const char *body, int blen,
                              const char *prefix, int plen, int *vlen)
{
    for (int i = 0; i + plen <= blen; i++) {
        if (memcmp(body + i, prefix, plen) == 0
            && (i == 0 || body[i - 1] == '\n')) {
            int s = i + plen;
            int e = s;
            while (e < blen && body[e] != '\n') e++;
            if (vlen) *vlen = e - s;
            return body + s;
        }
    }
    return NULL;
}

/* one-time import of the legacy 历史 folder into the new repo */
static void MigrateLegacy(const wchar_t *repo)
{
    char hex[41];
    if (ReadMaster(repo, hex)) return; /* already initialized */

    wchar_t old[MAX_PATH];
    if (!BuildHistoryDir(old, MAX_PATH)) return;
    if (GetFileAttributesW(old) == INVALID_FILE_ATTRIBUTES) return;

    wchar_t (*names)[MAX_PATH] =
        (wchar_t(*)[MAX_PATH])malloc(64 * MAX_PATH * sizeof(wchar_t));
    if (!names) return;
    int count = 0;

    wchar_t pattern[MAX_PATH + 128];
    wsprintfW(pattern, L"%s\\%s_*.md", old, g_name);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (count >= 64) break;
            int i = count++; /* ascending order: oldest first */
            while (i > 0 && lstrcmpW(names[i - 1], fd.cFileName) > 0) {
                memcpy(names[i], names[i - 1], MAX_PATH * sizeof(wchar_t));
                i--;
            }
            lstrcpynW(names[i], fd.cFileName, MAX_PATH);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    for (int i = 0; i < count; i++) {
        wchar_t full[MAX_PATH];
        wsprintfW(full, L"%s\\%s", old, names[i]);
        char *u8 = NULL;
        int len = 0;
        if (!ReadAllBytes(full, &u8, &len)) continue;
        LONGLONG unixSec = StampToUnix(
            names[i] + lstrlenW(g_name) + 1);
        if (unixSec < 0) unixSec = 0;
        char chex[41];
        WriteSnapshot(repo, u8, len, unixSec, "保存 ", NULL, chex);
        free(u8);
    }
    free(names);
    if (count > 0) {
        wchar_t bak[MAX_PATH];
        lstrcpynW(bak, old, MAX_PATH);
        lstrcpynW(bak + lstrlenW(bak), L"_迁移备份", MAX_PATH - lstrlenW(bak));
        MoveFileW(old, bak); /* keep data, stop cluttering the folder */
    }
}

/* delete objects/<xx>/<rest> for a hex sha (missing file is fine) */
static void DeleteObj(const wchar_t *repo, const char *hex)
{
    wchar_t dirW[8], restW[48], path[MAX_PATH + 128];
    AsciiToWide(hex, dirW, 3);
    AsciiToWide(hex + 2, restW, 48);
    wsprintfW(path, L"%s\\objects\\%s\\%s", repo, dirW, restW);
    DeleteFileW(path);
}

/* physically drop root-side commits beyond the history cap so the
 * store stays small; pinned snapshots (and everything newer) survive.
 * The cut commit's parent link dangles, which the walker treats as the
 * end of history -- same result git's own gc reaches, just simpler. */
static void HistPrune(const wchar_t *repo)
{
    char tip[41];
    if (!ReadMaster(repo, tip)) return;

    struct Obj { char c[41], t[41], b[41]; };
    int maxN = 2200;
    struct Obj *arr = (struct Obj *)malloc(maxN * sizeof(struct Obj));
    if (!arr) return;
    int n = 0;
    char cur[41];
    lstrcpynA(cur, tip, 41);
    char next[41] = "";
    while (n < maxN) {
        char *body = NULL;
        int blen = 0;
        if (!ReadLoose(repo, cur, &body, &blen)) break;
        struct Obj *e = &arr[n++];
        lstrcpynA(e->c, cur, 41);
        e->t[0] = 0;
        e->b[0] = 0;
        int tl = 0;
        const char *t = CommitLine(body, blen, "tree ", 5, &tl);
        if (t && tl == 40) {
            memcpy(e->t, t, 40);
            e->t[40] = 0;
        }
        /* parent chain continues below the oldest entry we keep */
        int pl = 0;
        const char *p = CommitLine(body, blen, "parent ", 7, &pl);
        next[0] = 0;
        if (p && pl == 40) {
            memcpy(next, p, 40);
            next[40] = 0;
        }
        free(body);
        /* blob = last 20 bytes of the tree object (single-file repo) */
        if (e->t[0]) {
            char *tb = NULL;
            int tblen = 0;
            if (ReadLoose(repo, e->t, &tb, &tblen)) {
                if (tblen >= 20) ShaHex((const BYTE *)(tb + tblen - 20),
                                        e->b);
                free(tb);
            }
        }
        if (!next[0]) break;                    /* hit the root */
        lstrcpynA(cur, next, 41);
    }

    int cap = g_histMax < 1 ? 1 : g_histMax;
    int keep = cap - 1;
    for (int i = 0; i < n; i++)
        if (i > keep && PinHas(arr[i].c)) keep = i;
    if (n > keep + 1) {
        for (int i = keep + 1; i < n; i++) {
            BOOL tKept = FALSE, bKept = FALSE;
            for (int k = 0; k <= keep; k++) {
                if (arr[i].t[0] && lstrcmpA(arr[i].t, arr[k].t) == 0)
                    tKept = TRUE;
                if (arr[i].b[0] && lstrcmpA(arr[i].b, arr[k].b) == 0)
                    bKept = TRUE;
            }
            DeleteObj(repo, arr[i].c);
            if (arr[i].t[0] && !tKept) DeleteObj(repo, arr[i].t);
            if (arr[i].b[0] && !bKept) DeleteObj(repo, arr[i].b);
        }
    }
    free(arr);
}

/* archive on save: real git commit, skipped when content is unchanged */
void GitArchive(const char *u8, int u8len, BOOL isAuto)
{
    wchar_t repo[MAX_PATH];
    if (!BuildRepoDir(repo, MAX_PATH)) return;
    MigrateOldRepo(repo);
    if (!EnsureRepo(repo)) return;
    MigrateLegacy(repo);

    /* blob hash */
    BYTE sha[20];
    char hdr[32];
    wsprintfA(hdr, "blob %d", u8len);
    char blobHex[41];
    if (!HashObject(hdr, u8, u8len, sha, blobHex)) return;
    char name8[MAX_PATH * 3];
    int n8 = WideCharToMultiByte(CP_UTF8, 0, g_name, -1, name8,
                                 MAX_PATH * 3, NULL, NULL);
    if (n8 <= 0) return;
    n8--;
    char entry[MAX_PATH * 3 + 32];
    memcpy(entry, "100644 ", 7);
    memcpy(entry + 7, name8, n8);
    entry[7 + n8] = 0;
    memcpy(entry + 8 + n8, sha, 20);
    wsprintfA(hdr, "tree %d", 8 + n8 + 20);
    char treeHex[41];
    if (!HashObject(hdr, entry, 8 + n8 + 20, sha, treeHex)) return;

    /* content unchanged since the last archived point (e.g. right after
     * restoring an old version): nothing new to record */
    if (g_archivedTree[0] && lstrcmpA(g_archivedTree, treeHex) == 0) return;

    char tipHex[41];
    if (ReadMaster(repo, tipHex)) {
        char *body = NULL;
        int blen = 0;
        if (ReadLoose(repo, tipHex, &body, &blen)) {
            int tl = 0;
            const char *t = CommitLine(body, blen, "tree ", 5, &tl);
            BOOL same = (t && tl == 40 && memcmp(t, treeHex, 40) == 0);
            free(body);
            if (same) {
                lstrcpynA(g_archivedTree, treeHex, 41);
                return;
            }
        }
    }

    /* auto saves collapse: a new auto point replaces the previous auto
     * tip by chaining to that tip's parent, so the history list keeps
     * only the latest automatic snapshot between manual saves. */
    const char *parentOvr = NULL;
    char ovrHex[41] = "";
    if (isAuto && ReadMaster(repo, tipHex)) {
        char *body = NULL;
        int blen = 0;
        if (ReadLoose(repo, tipHex, &body, &blen)) {
            int ml = 0;
            if (CommitLine(body, blen, "自动保存 ", 13, &ml)) {
                int pl = 0;
                const char *p = CommitLine(body, blen, "parent ", 7, &pl);
                if (p && pl == 40) {
                    memcpy(ovrHex, p, 40);
                    ovrHex[40] = 0;
                }
                parentOvr = ovrHex; /* "" -> this auto save becomes root */
            }
            free(body);
        }
    }

    char commitHex[41];
    if (WriteSnapshot(repo, u8, u8len, 0,
                      isAuto ? "自动保存 " : "保存 ",
                      parentOvr, commitHex)) {
        lstrcpynA(g_archivedTree, treeHex, 41);
        PinLoad(g_path);        /* pins of THIS file protect old commits */
        HistPrune(repo);
    }
}