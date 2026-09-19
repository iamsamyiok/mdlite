/* MDLite - materialized wiki backlinks (双向链接)
 *
 * Semantics: when note A links to [[B]], the relation must be visible in
 * B itself. After a save, every note in the workspace gets an
 * auto-managed block at the end of its file:
 *
 *     <!-- mdlite:backlinks -->
 *     - [[A]]
 *     - [[C]]
 *
 * listing the notes whose (user-typed) content links to it. The block is
 * rebuilt from the typed content of all notes on every sync, so stale
 * entries disappear automatically when a link is removed, and the block
 * is deleted entirely when nothing links to the note anymore. */

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include "backlinks.h"

#define BL_MARKER      L"<!-- mdlite:backlinks -->"
#define BL_MAXFILES    2048
#define BL_MAXNAME     96
#define BL_TARGET_MAX  256

/* ------------------------------------------------------------------ */
/* tiny dynamic wide-string list                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t **v;
    int n, cap;
} BlList;

static void BlListFree(BlList *l)
{
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL; l->n = l->cap = 0;
}

static void BlListAdd(BlList *l, const wchar_t *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = (wchar_t **)realloc(l->v, l->cap * sizeof(wchar_t *));
    }
    int n = (int)lstrlenW(s);
    if (n > BL_MAXNAME - 1) n = BL_MAXNAME - 1;
    l->v[l->n] = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!l->v[l->n]) return;
    memcpy(l->v[l->n], s, (size_t)n * sizeof(wchar_t));
    l->v[l->n][n] = 0;
    l->n++;
}

static BOOL BlListHas(const BlList *l, const wchar_t *s)
{
    for (int i = 0; i < l->n; i++)
        if (lstrcmpiW(l->v[i], s) == 0) return TRUE;
    return FALSE;
}

static int BlCmp(const void *a, const void *b)
{
    const wchar_t *x = *(const wchar_t * const *)a;
    const wchar_t *y = *(const wchar_t * const *)b;
    return lstrcmpiW(x, y);
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static const wchar_t *BlBaseOf(const wchar_t *path)
{
    const wchar_t *b = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') b = p + 1;
    return b;
}

/* basename without extension, lowercased (link identity) */
static void BlKey(const wchar_t *pathOrName, wchar_t *out, int cch)
{
    const wchar_t *b = BlBaseOf(pathOrName);
    int n = 0;
    for (; b[n] && n < cch - 1; n++) {
        if (b[n] == L'.') break;   /* drop the extension */
        out[n] = (wchar_t)towlower(b[n]);
    }
    out[n] = 0;
}

static BOOL BlEndsWithMd(const wchar_t *name)
{
    size_t n = lstrlenW(name);
    return n >= 3 && lstrcmpiW(name + n - 3, L".md") == 0;
}

/* ------------------------------------------------------------------ */
/* workspace .md collection (recursive, dot-dirs skipped)              */
/* ------------------------------------------------------------------ */

static void BlWalkRec(BlList *files, const wchar_t *dir)
{
    wchar_t pat[MAX_PATH];
    _snwprintf(pat, MAX_PATH - 1, L"%s\\*.*", dir);
    pat[MAX_PATH - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;          /* ., .., dot-dirs */
        wchar_t full[MAX_PATH];
        _snwprintf(full, MAX_PATH - 1, L"%s\\%s", dir, fd.cFileName);
        full[MAX_PATH - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            BlWalkRec(files, full);
        } else if (BlEndsWithMd(fd.cFileName) && files->n < BL_MAXFILES) {
            BlListAdd(files, full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ------------------------------------------------------------------ */
/* per-file helpers                                                    */
/* ------------------------------------------------------------------ */

/* load a note as wide text; reports whether the file carried a BOM */
static BOOL BlLoad(const wchar_t *path, wchar_t **wout, int *wlen, BOOL *hasBom)
{
    char *u8 = NULL;
    int u8len = 0;
    *wout = NULL; *wlen = 0; *hasBom = FALSE;
    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD size = GetFileSize(h, NULL), read = 0;
    if (size == INVALID_FILE_SIZE) { CloseHandle(h); return FALSE; }
    u8 = (char *)malloc((size_t)size + 1);
    if (!u8) { CloseHandle(h); return FALSE; }
    if (!ReadFile(h, u8, size, &read, NULL)) {
        free(u8); CloseHandle(h); return FALSE;
    }
    CloseHandle(h);
    u8len = (int)read;
    int skip = 0;
    if (u8len >= 3 && (unsigned char)u8[0] == (char)0xEF
        && (unsigned char)u8[1] == (char)0xBB
        && (unsigned char)u8[2] == (char)0xBF) {
        skip = 3;
        *hasBom = TRUE;
    }
    int wl = MultiByteToWideChar(CP_UTF8, 0, u8 + skip, u8len - skip, NULL, 0);
    if (wl <= 0) { free(u8); return FALSE; }
    wchar_t *w = (wchar_t *)malloc((wl + 1) * sizeof(wchar_t));
    if (!w) { free(u8); return FALSE; }
    MultiByteToWideChar(CP_UTF8, 0, u8 + skip, u8len - skip, w, wl);
    w[wl] = 0;
    free(u8);
    *wout = w;
    *wlen = wl;
    return TRUE;
}

static BOOL BlSave(const wchar_t *path, const wchar_t *w, BOOL hasBom)
{
    int u8len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (u8len <= 0) return FALSE;
    u8len -= 1;   /* drop the NUL that -1 added */
    char *u8 = (char *)malloc((size_t)u8len + 3);
    if (!u8) return FALSE;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, (int)u8len + 1, NULL, NULL);
    int total = u8len;
    if (hasBom) {
        memmove(u8 + 3, u8, (size_t)u8len);
        u8[0] = (char)0xEF; u8[1] = (char)0xBB; u8[2] = (char)0xBF;
        total = u8len + 3;
    }
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok = FALSE;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = WriteFile(h, u8, (DWORD)total, &written, NULL)
             && written == (DWORD)total;
        CloseHandle(h);
    }
    free(u8);
    return ok;
}

/* ------------------------------------------------------------------ */
/* line model                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int start, end;      /* char offsets of the line (excl. line break) */
    BOOL managed;        /* part of the auto backlink block */
} BlLine;

static BOOL BlIsMarker(const wchar_t *p, int len)
{
    return len == (int)lstrlenW(BL_MARKER)
           && memcmp(p, BL_MARKER, lstrlenW(BL_MARKER) * sizeof(wchar_t)) == 0;
}

/* parse lines and flag the managed block: the marker line opens it,
 * consecutive "- [[...]]" lines continue it, anything else closes it */
static void BlParseLines(const wchar_t *w, int wlen,
                         BlLine **linesOut, int *nOut,
                         int *markerIdx, BOOL *hasCRLF)
{
    BlLine *lines = (BlLine *)malloc((size_t)(wlen + 4) * sizeof(BlLine));
    int n = 0, ls = 0, i = 0;
    BOOL inBlock = FALSE;
    *markerIdx = -1;
    *hasCRLF = FALSE;
    while (i <= wlen) {
        if (i == wlen || w[i] == L'\n') {
            int e = i;
            if (e > ls && w[e - 1] == L'\r') { e--; *hasCRLF = TRUE; }
            if (i == wlen && e == ls && n > 0) break;   /* no trailing blank */
            lines[n].start = ls;
            lines[n].end = e;
            int len = e - ls;
            const wchar_t *p = w + ls;
            int k = 0;
            while (k < len && (p[k] == L' ' || p[k] == L'\t')) k++;
            if (BlIsMarker(p + k, len - k)) {
                lines[n].managed = TRUE;
                if (*markerIdx < 0) { *markerIdx = n; inBlock = TRUE; }
            } else {
                lines[n].managed = inBlock && len - k > 4
                    && p[k] == L'-' && p[k + 1] == L' '
                    && p[k + 2] == L'[' && p[k + 3] == L'['
                    && wcsstr(p + k, L"]]") != NULL;
                if (!lines[n].managed) inBlock = FALSE;
            }
            n++;
            ls = i + 1;
        }
        if (i >= wlen) break;
        if (w[i] == L'\r' && i + 1 < wlen && w[i + 1] == L'\n') {
            if (i + 1 < wlen && w[i + 1] == L'\n') i++;
        }
        i++;
    }
    if (!*hasCRLF)
        for (int k = 0; k + 1 < wlen; k++)
            if (w[k] == L'\r' && w[k + 1] == L'\n') { *hasCRLF = TRUE; break; }
    *linesOut = lines;
    *nOut = n;
}

/* collect outgoing wiki targets from user-typed content (the managed
 * block and fenced code are excluded). Targets resolve against the
 * workspace keys; unresolved targets are ignored. */
static void BlOutgoing(const wchar_t *w, BlLine *lines, int nLines,
                       const BlList *fileKeys, BlList *outPaths)
{
    BOOL inFence = FALSE;
    for (int li = 0; li < nLines; li++) {
        if (lines[li].managed) continue;
        const wchar_t *p = w + lines[li].start;
        int len = lines[li].end - lines[li].start;
        if (len >= 3 && p[0] == L'`' && p[1] == L'`' && p[2] == L'`')
            inFence = !inFence;
        if (inFence) continue;
        for (int i = 0; i + 1 < len; i++) {
            if (p[i] == L'[' && p[i + 1] == L'[') {
                int j = i + 2;
                while (j + 1 < len && !(p[j] == L']' && p[j + 1] == L']')) j++;
                int closed = (j < len && p[j] == L']' && p[j + 1] == L']');
                if (closed) {
                    int tlen = j - (i + 2);
                    if (tlen > 0 && tlen < BL_TARGET_MAX) {
                        wchar_t raw[BL_TARGET_MAX];
                        memcpy(raw, p + i + 2, (size_t)tlen * sizeof(wchar_t));
                        raw[tlen] = 0;
                        wchar_t *pipe = wcschr(raw, L'|');
                        if (pipe) *pipe = 0;   /* [[name|alias]] -> name */
                        wchar_t key[BL_TARGET_MAX];
                        BlKey(raw, key, BL_TARGET_MAX);
                        if (key[0])
                            for (int k = 0; k < fileKeys->n; k++)
                                if (lstrcmpW(fileKeys->v[k], key) == 0) {
                                    if (!BlListHas(outPaths, key))
                                        BlListAdd(outPaths, key);
                                    break;
                                }
                    }
                    i = j + 1;
                }
            }
        }
    }
}

/* extract the [[name]] of one managed list line */
static void BlBlockEntryName(const wchar_t *w, const BlLine *ln, wchar_t *nm)
{
    const wchar_t *p = w + ln->start;
    int len = ln->end - ln->start;
    int k = 0;
    while (k < len && (p[k] == L' ' || p[k] == L'\t')) k++;
    int b = k + 3;               /* skip "- [[" */
    int e = len - 2;             /* before "]]" */
    int nn = 0;
    for (int q = b; q < e && nn < BL_MAXNAME - 1; q++) nm[nn++] = p[q];
    nm[nn] = 0;
}

/* ------------------------------------------------------------------ */
/* workspace sync                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t path[MAX_PATH];
    wchar_t key[BL_MAXNAME];
    wchar_t *text;       /* wide content (loaded once) */
    int textLen;
    BOOL hasBom, hasCRLF;
    BlLine *lines; int nLines, markerIdx;
    BlList outgoing;     /* resolved keys of typed [[links]] */
} BlNote;

void BacklinksSyncDir(const wchar_t *dir)
{
    if (!dir || !dir[0]) return;

    BlList files; files.v = NULL; files.n = files.cap = 0;
    BlWalkRec(&files, dir);
    int nN = files.n;
    if (nN < 1 || nN > BL_MAXFILES) { BlListFree(&files); return; }

    BlNote *notes = (BlNote *)calloc((size_t)nN, sizeof(BlNote));
    BlList *incoming = (BlList *)calloc((size_t)nN, sizeof(BlList));
    if (!notes || !incoming) {
        free(notes); free(incoming); BlListFree(&files);
        return;
    }

    /* pass 1: load each note, index its key, collect typed outgoing */
    for (int i = 0; i < nN; i++) {
        lstrcpynW(notes[i].path, files.v[i], MAX_PATH);
        BlKey(files.v[i], notes[i].key, BL_MAXNAME);
        if (!BlLoad(files.v[i], &notes[i].text, &notes[i].textLen,
                    &notes[i].hasBom))
            continue;
        notes[i].hasCRLF = FALSE;
        BlParseLines(notes[i].text, notes[i].textLen,
                     &notes[i].lines, &notes[i].nLines,
                     &notes[i].markerIdx, &notes[i].hasCRLF);
        /* file keys for resolution */
        BlList keys; keys.v = NULL; keys.n = keys.cap = 0;
        for (int k = 0; k < nN; k++) {
            wchar_t kk[BL_MAXNAME];
            BlKey(files.v[k], kk, BL_MAXNAME);
            BlListAdd(&keys, kk);
        }
        BlOutgoing(notes[i].text, notes[i].lines, notes[i].nLines,
                   &keys, &notes[i].outgoing);
        BlListFree(&keys);
    }

    /* pass 2: reverse map - who links to whom (typed links only) */
    for (int i = 0; i < nN; i++) {
        for (int k = 0; k < notes[i].outgoing.n; k++) {
            const wchar_t *tgt = notes[i].outgoing.v[k];
            for (int j = 0; j < nN; j++) {
                if (lstrcmpW(notes[j].key, tgt) != 0) continue;
                if (j != i && !BlListHas(&incoming[j], notes[i].key))
                    BlListAdd(&incoming[j], notes[i].key);
                break;
            }
        }
    }

    /* pass 3: rebuild the managed block of every note that changed */
    for (int i = 0; i < nN; i++) {
        BlNote *nt = &notes[i];
        if (!nt->text) continue;

        /* desired entries: sorted alphabetically for stable output */
        BlList want; want.v = NULL; want.n = want.cap = 0;
        for (int k = 0; k < incoming[i].n; k++)
            BlListAdd(&want, incoming[i].v[k]);
        if (want.n > 1) qsort(want.v, (size_t)want.n,
                              sizeof(wchar_t *), BlCmp);

        /* current managed entries */
        BlList cur; cur.v = NULL; cur.n = cur.cap = 0;
        BOOL hasBlock = nt->markerIdx >= 0;
        if (hasBlock)
            for (int li = nt->markerIdx + 1; li < nt->nLines; li++)
                if (nt->lines[li].managed) {
                    wchar_t nm[BL_MAXNAME];
                    BlBlockEntryName(nt->text, &nt->lines[li], nm);
                    BlListAdd(&cur, nm);
                }

        BOOL same = (hasBlock == (want.n > 0)) && (want.n == cur.n);
        if (same)
            for (int k = 0; k < want.n; k++)
                if (lstrcmpiW(cur.v[k], want.v[k]) != 0) { same = FALSE; break; }
        if (same) {
            BlListFree(&want); BlListFree(&cur);
            free(nt->lines);
            continue;
        }

        /* rebuild text: head (before marker) + block + tail (after block) */
        wchar_t eol[3] = L"\n";
        if (nt->hasCRLF) { eol[0] = L'\r'; eol[1] = L'\n'; eol[2] = 0; }
        int eolN = nt->hasCRLF ? 2 : 1;

        int headEnd = hasBlock ? nt->lines[nt->markerIdx].start : nt->textLen;
        while (headEnd > 0 && (nt->text[headEnd - 1] == L'\n'
                               || nt->text[headEnd - 1] == L'\r'))
            headEnd--;
        int tailStart = (hasBlock && nt->markerIdx + 1 < nt->nLines)
                        ? nt->lines[nt->markerIdx + 1].start : -1;
        if (hasBlock) {
            /* tail starts after the last managed line, not after the marker */
            int last = nt->markerIdx;
            for (int li = nt->markerIdx + 1; li < nt->nLines; li++)
                if (nt->lines[li].managed) last = li;
            tailStart = (last + 1 < nt->nLines)
                        ? nt->lines[last + 1].start : nt->textLen;
        }

        int cap = nt->textLen + (want.n + 8) * (BL_MAXNAME + 16) + 64;
        wchar_t *nw = (wchar_t *)malloc((size_t)cap * sizeof(wchar_t));
        if (!nw) {
            BlListFree(&want); BlListFree(&cur);
            free(nt->lines);
            continue;
        }
        int n2 = 0;
        memcpy(nw, nt->text, (size_t)headEnd * sizeof(wchar_t));
        n2 = headEnd;
        if (want.n > 0) {
            if (n2 > 0) { nw[n2++] = eol[0]; if (eolN == 2) nw[n2++] = eol[1]; }
            for (const wchar_t *q = BL_MARKER; *q; q++) nw[n2++] = *q;
            nw[n2++] = eol[0]; if (eolN == 2) nw[n2++] = eol[1];
            for (int k = 0; k < want.n; k++) {
                for (const wchar_t *q = L"- [["; *q; q++) nw[n2++] = *q;
                for (const wchar_t *q = want.v[k]; *q; q++) nw[n2++] = *q;
                for (const wchar_t *q = L"]]"; *q; q++) nw[n2++] = *q;
                nw[n2++] = eol[0]; if (eolN == 2) nw[n2++] = eol[1];
            }
        }
        if (tailStart >= 0) {
            memcpy(nw + n2, nt->text + tailStart,
                   (size_t)(nt->textLen - tailStart) * sizeof(wchar_t));
            n2 += nt->textLen - tailStart;
        }
        nw[n2] = 0;
        BlSave(nt->path, nw, nt->hasBom);

        free(nw);
        BlListFree(&want); BlListFree(&cur);
        free(nt->lines);
    }

    /* cleanup */
    for (int i = 0; i < nN; i++) {
        BlListFree(&notes[i].outgoing);
        BlListFree(&incoming[i]);
        free(notes[i].lines);
        free(notes[i].text);
    }
    free(incoming);
    free(notes);
    BlListFree(&files);
}
