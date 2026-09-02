/* MDLite - workspace file tree sidebar: recursive directory view of the
 * current document's folder. Lazily enumerates children on first expand,
 * keeps expansion state across rebuilds (matched by path). */
#define _WIN32_WINNT 0x0601
#include "tree.h"
#include <stdlib.h>
#include <string.h>

static int TreeHeaderH(void) { return SC(32); }
static int TreeRowH(void)    { return SC(26); }
#define TREE_MAX_VIS 4096
#define TREE_MAX_PER_DIR 500

typedef struct TreeNode {
    wchar_t name[80];
    wchar_t path[MAX_PATH];
    BOOL isDir;
    BOOL expanded;
    BOOL loaded;          /* children enumerated at least once */
    int  depth;
    struct TreeNode *child, *next, *parent;
} TreeNode;

static TreeNode *g_root;             /* synthetic node for the workspace dir */
static wchar_t g_wsDir[MAX_PATH];    /* "" = no workspace */
static BOOL g_shown;
static int  g_scroll;
static int  g_hover;                 /* visible row under cursor, -1 none */

static TreeNode *g_vis[TREE_MAX_VIS];
static int g_visN;

/* ---- helpers ---- */

static void FreeNodes(TreeNode *n)
{
    while (n) {
        TreeNode *nx = n->next;
        FreeNodes(n->child);
        free(n);
        n = nx;
    }
}

static TreeNode *NewNode(const wchar_t *dir, const wchar_t *name, int depth)
{
    TreeNode *n = (TreeNode *)calloc(1, sizeof(TreeNode));
    if (!n) return NULL;
    lstrcpynW(n->name, name, 80);
    wsprintfW(n->path, L"%s\\%s", dir, name);
    n->depth = depth;
    n->isDir = FALSE;
    return n;
}

static BOOL IsMdName(const wchar_t *name)
{
    const wchar_t *dot = wcsrchr(name, L'.');
    if (!dot) return FALSE;
    return lstrcmpiW(dot, L".md") == 0
        || lstrcmpiW(dot, L".markdown") == 0
        || lstrcmpiW(dot, L".mdown") == 0
        || lstrcmpiW(dot, L".txt") == 0;
}

/* dirs first, then files, both alphabetical */
static int NodeCmp(const TreeNode *a, const TreeNode *b)
{
    if (a->isDir != b->isDir) return a->isDir ? -1 : 1;
    int c = lstrcmpiW(a->name, b->name);
    return c;
}

static void SortChildren(TreeNode *dir)
{
    /* insertion sort on the linked list; per-dir counts are small */
    TreeNode *sorted = NULL;
    TreeNode *n = dir->child;
    while (n) {
        TreeNode *nx = n->next;
        if (!sorted || NodeCmp(n, sorted) < 0) {
            n->next = sorted;
            sorted = n;
        } else {
            TreeNode *p = sorted;
            while (p->next && NodeCmp(n, p->next) >= 0) p = p->next;
            n->next = p->next;
            p->next = n;
        }
        n = nx;
    }
    dir->child = sorted;
    for (TreeNode *c = dir->child; c; c = c->next) c->parent = dir;
}

/* look up the previous tree's expansion for the same path */
static BOOL PrevExpanded(const TreeNode *oldRoot, const wchar_t *path)
{
    if (!oldRoot) return FALSE;
    /* BFS-ish scan: expansion only matters for dirs we had opened */
    const TreeNode *stack[512];
    int sp = 0;
    if (oldRoot->child) stack[sp++] = oldRoot;
    while (sp > 0) {
        const TreeNode *n = stack[--sp];
        if (lstrcmpiW(n->path, path) == 0) return n->expanded;
        if (n->child) {
            const TreeNode *c = n->child;
            while (c && sp < 510) { stack[sp++] = c; c = c->next; }
        }
    }
    return FALSE;
}

static void EnumChildren(TreeNode *dir, const TreeNode *oldRoot)
{
    if (dir->loaded) return;
    dir->loaded = TRUE;
    wchar_t pat[MAX_PATH];
    wsprintfW(pat, L"%s\\*", dir->path);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    TreeNode **tail = &dir->child;
    int count = 0;
    do {
        if (fd.cFileName[0] == L'.') continue;
        BOOL isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!isDir && !IsMdName(fd.cFileName)) continue;
        if (++count > TREE_MAX_PER_DIR) break;
        TreeNode *n = NewNode(dir->path, fd.cFileName, dir->depth + 1);
        if (!n) break;
        n->isDir = isDir;
        n->expanded = PrevExpanded(oldRoot, n->path) && isDir;
        *tail = n;
        tail = &n->next;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    SortChildren(dir);
    /* pre-expand inherited-open dirs */
    for (TreeNode *c = dir->child; c; c = c->next)
        if (c->isDir && c->expanded) EnumChildren(c, oldRoot);
}

/* ---- visible-row flattening ---- */

static void Flatten(void)
{
    g_visN = 0;
    if (!g_root) return;
    TreeNode *stack[256];
    int sp = 0;
    TreeNode *c = g_root->child;
    while (c && sp < 255) { stack[sp++] = c; c = c->next; }
    while (sp > 0 && g_visN < TREE_MAX_VIS) {
        TreeNode *n = stack[--sp];
        g_vis[g_visN++] = n;
        if (n->isDir && n->expanded) {
            TreeNode *k = n->child;
            while (k && sp < 255) { stack[sp++] = k; k = k->next; }
        }
    }
}

/* main.c owns the find bar; the tree starts below it like the editor */
extern BOOL FindBarActive(void);

static void TreeViewRect(RECT *rc, const RECT *rcClient)
{
    rc->left = 0;
    rc->top = HeaderH() + (FindBarActive() ? SC(40) : 0);
    rc->right = TreeWidth();
    rc->bottom = rcClient->bottom - StatusH();
}

/* ---- public api ---- */

int TreeWidth(void)
{
    return g_shown ? SC(200) : 0;
}

BOOL TreeShown(void)
{
    return g_shown;
}

void TreeSetShown(BOOL on)
{
    g_shown = on && g_wsDir[0] != 0;
}

void TreeToggle(void)
{
    if (!g_wsDir[0]) {
        MessageBoxW(g_hwnd,
            L"当前文档尚未保存到磁盘，暂无工作区。\n"
            L"先保存（Ctrl+S）或打开一个 .md 文件。",
            APP_NAME, MB_ICONINFORMATION);
        return;
    }
    g_shown = !g_shown;
    InvalidateRect(g_hwnd, NULL, TRUE);
}

void TreeSync(const wchar_t *docPath)
{
    wchar_t dir[MAX_PATH];
    dir[0] = 0;
    if (docPath && docPath[0]) {
        lstrcpynW(dir, docPath, MAX_PATH);
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0;
        else dir[0] = 0;
    }
    if (dir[0] && (!g_wsDir[0] || lstrcmpiW(dir, g_wsDir) != 0)) {
        /* workspace changed: rebuild, remembering expansion */
        TreeNode *old = g_root;
        g_root = NewNode(dir, dir, -1);
        if (g_root) {
            g_root->isDir = TRUE;
            g_root->expanded = TRUE;
            lstrcpynW(g_root->path, dir, MAX_PATH);
            EnumChildren(g_root, old);
        }
        FreeNodes(old);
        lstrcpynW(g_wsDir, dir, MAX_PATH);
        g_scroll = 0;
    } else if (!dir[0]) {
        FreeNodes(g_root);
        g_root = NULL;
        g_wsDir[0] = 0;
        g_scroll = 0;
    }
    /* same folder: the current-file highlight is computed at draw time */
    g_shown = g_shown && g_wsDir[0] != 0;
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, TRUE);
}

void TreeDraw(HDC dc, const RECT *rcClient)
{
    if (!g_shown) return;
    RECT rc;
    TreeViewRect(&rc, rcClient);
    if (rc.right <= 0 || rc.bottom <= rc.top) return;

    HBRUSH bg = CreateSolidBrush(RGB(0xF7, 0xF7, 0xF9));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    Flatten();

    /* header row: folder name */
    {
        const wchar_t *base = wcsrchr(g_wsDir, L'\\');
        base = base ? base + 1 : g_wsDir;
        wchar_t title[96];
        wsprintfW(title, L"工作区：%s", base);
        RECT rh = { rc.left, rc.top, rc.right, rc.top + TreeHeaderH() };
        HBRUSH hb = CreateSolidBrush(RGB(0xEE, 0xEE, 0xF1));
        FillRect(dc, &rh, hb);
        DeleteObject(hb);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xD2, 0xD2, 0xD7));
        HPEN op = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, rh.left, rh.bottom - 1, NULL);
        LineTo(dc, rh.right, rh.bottom - 1);
        SelectObject(dc, op);
        DeleteObject(pen);
        HFONT old = (HFONT)SelectObject(dc, g_fontHeader);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x1D, 0x1D, 0x1F));
        RECT txt = { rh.left + SC(12), rh.top, rh.right - SC(8), rh.bottom };
        DrawTextW(dc, title, -1, &txt,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
    }

    if (!g_root) {
        HFONT old = (HFONT)SelectObject(dc, g_fontHeader);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x8E, 0x8E, 0x93));
        RECT txt = { rc.left + SC(12), rc.top + TreeHeaderH() + SC(8),
                     rc.right - SC(8), rc.top + TreeHeaderH() + SC(60) };
        DrawTextW(dc, L"（无工作区）", -1, &txt, DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        return;
    }

    /* rows */
    int rows = (rc.bottom - rc.top - TreeHeaderH()) / TreeRowH();
    if (rows < 1) rows = 1;
    int maxScroll = g_visN - rows;
    if (maxScroll < 0) maxScroll = 0;
    if (g_scroll > maxScroll) g_scroll = maxScroll;

    HFONT old = (HFONT)SelectObject(dc, g_fontHeader);
    SetBkMode(dc, TRANSPARENT);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    int tyOff = (TreeRowH() - tm.tmHeight) / 2;

    for (int i = 0; i < rows; i++) {
        int idx = g_scroll + i;
        if (idx >= g_visN) break;
        TreeNode *n = g_vis[idx];
        RECT row = { rc.left, rc.top + TreeHeaderH() + i * TreeRowH(),
                     rc.right, rc.top + TreeHeaderH() + (i + 1) * TreeRowH() };
        BOOL cur = !n->isDir && g_path[0]
                   && lstrcmpiW(n->path, g_path) == 0;
        if (cur) {
            HBRUSH b = CreateSolidBrush(RGB(0xE3, 0xEE, 0xFF));
            FillRect(dc, &row, b);
            DeleteObject(b);
        } else if (idx == g_hover) {
            HBRUSH b = CreateSolidBrush(RGB(0xE8, 0xE8, 0xED));
            FillRect(dc, &row, b);
            DeleteObject(b);
        }
        int x = rc.left + SC(10) + n->depth * SC(14);
        if (n->isDir) {
            const wchar_t *ar = n->expanded ? L"\x25BE" : L"\x25B8";
            SetTextColor(dc, n->expanded ? RGB(0x00, 0x7A, 0xFF)
                                         : RGB(0x6E, 0x6E, 0x73));
            TextOutW(dc, x, row.top + tyOff, ar, 1);
            SetTextColor(dc, RGB(0x1D, 0x1D, 0x1F));
            TextOutW(dc, x + SC(14), row.top + tyOff,
                     n->name, lstrlenW(n->name));
        } else {
            SetTextColor(dc, cur ? RGB(0x00, 0x62, 0xCC) : RGB(0x1D, 0x1D, 0x1F));
            int wName = text_w(dc, g_fontHeader, n->name, lstrlenW(n->name));
            int avail = rc.right - SC(10) - (x + SC(14));
            if (wName > avail) {
                wchar_t fit[84];
                lstrcpynW(fit, n->name, 84);
                while (lstrlenW(fit) > 1
                       && text_w(dc, g_fontHeader, fit, lstrlenW(fit)) > avail)
                    fit[lstrlenW(fit) - 1] = 0;
                int fl = lstrlenW(fit);
                if (fl >= 1) fit[fl - 1] = 0x2026;
                TextOutW(dc, x + SC(14), row.top + tyOff, fit, lstrlenW(fit));
            } else {
                TextOutW(dc, x + SC(14), row.top + tyOff,
                         n->name, lstrlenW(n->name));
            }
        }
    }
    SelectObject(dc, old);

    /* slim scrollbar when content overflows */
    if (maxScroll > 0) {
        int viewH = rc.bottom - rc.top - TreeHeaderH();
        int thumbH = viewH * rows / g_visN;
        if (thumbH < SC(18)) thumbH = SC(18);
        int thumbY = rc.top + TreeHeaderH()
                     + (viewH - thumbH) * g_scroll / maxScroll;
        RECT rcThumb = { rc.right - SC(5), thumbY, rc.right, thumbY + thumbH };
        HBRUSH tb = CreateSolidBrush(RGB(0xC7, 0xC7, 0xCC));
        FillRect(dc, &rcThumb, tb);
        DeleteObject(tb);
    }

    /* right border */
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xD2, 0xD2, 0xD7));
    HPEN op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, rc.right - 1, rc.top, NULL);
    LineTo(dc, rc.right - 1, rc.bottom);
    SelectObject(dc, op);
    DeleteObject(pen);
}

BOOL TreePtIn(POINT pt)
{
    if (!g_shown) return FALSE;
    return pt.x >= 0 && pt.x < TreeWidth() && pt.y >= 0;
}

static int RowAt(POINT pt, const RECT *rcClient)
{
    RECT rc;
    TreeViewRect(&rc, rcClient);
    if (!PtInRect(&rc, pt)) return -1;
    int y = pt.y - rc.top - TreeHeaderH();
    if (y < 0) return -1;
    Flatten();
    return g_scroll + y / TreeRowH();
}

BOOL TreeClick(POINT pt)
{
    if (!TreePtIn(pt)) return FALSE;
    RECT rcClient;
    GetClientRect(g_hwnd, &rcClient);
    int row = RowAt(pt, &rcClient);
    if (row < 0 || row >= g_visN) return TRUE;   /* header/blank: consume */
    TreeNode *n = g_vis[row];
    if (!n) return TRUE;
    if (n->isDir) {
        n->expanded = !n->expanded;
        if (n->expanded)
            EnumChildren(n, NULL);   /* fresh load keeps no prior state */
    } else {
        if (ConfirmDiscard())
            LoadFile(n->path);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
    return TRUE;
}

void TreeMouseMove(POINT pt)
{
    if (!g_shown) return;
    RECT rcClient;
    GetClientRect(g_hwnd, &rcClient);
    int row = RowAt(pt, &rcClient);
    if (row >= g_visN) row = -1;
    if (row != g_hover) {
        g_hover = row;
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

void TreeWheel(int delta)
{
    Flatten();
    RECT rcClient;
    GetClientRect(g_hwnd, &rcClient);
    int rows = (rcClient.bottom - StatusH() - HeaderH() - TreeHeaderH())
               / TreeRowH();
    if (rows < 1) rows = 1;
    int maxScroll = g_visN - rows;
    if (maxScroll <= 0) return;
    int ns = g_scroll - delta;   /* delta already in row units */
    if (ns < 0) ns = 0;
    if (ns > maxScroll) ns = maxScroll;
    if (ns != g_scroll) {
        g_scroll = ns;
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

/* ------------------------------------------------------------------ */
/* workspace link index: [[wiki-link]] backlinks & orphans             */
/* ------------------------------------------------------------------ */

#include <wctype.h>

typedef struct {
    wchar_t src[MAX_PATH];   /* file containing the link */
    wchar_t dst[260];        /* wiki target name */
} LinkEdge;

static LinkEdge *g_edges;
static int g_edgeN, g_edgeCap;
static wchar_t (*g_mdFiles)[MAX_PATH];
static int g_fileN, g_fileCap;

static void CollectMd(TreeNode *n)
{
    for (; n; n = n->next) {
        if (n->isDir) CollectMd(n->child);
        else {
            if (g_fileN == g_fileCap) {
                g_fileCap = g_fileCap ? g_fileCap * 2 : 32;
                g_mdFiles = (wchar_t (*)[MAX_PATH])realloc(
                    g_mdFiles, g_fileCap * sizeof(*g_mdFiles));
            }
            if (g_mdFiles)
                lstrcpynW(g_mdFiles[g_fileN++], n->path, MAX_PATH);
        }
    }
}

static void AddEdge(const wchar_t *src, const wchar_t *dst, int dstLen)
{
    if (dstLen <= 0 || dstLen >= 260) return;
    if (g_edgeN == g_edgeCap) {
        g_edgeCap = g_edgeCap ? g_edgeCap * 2 : 32;
        g_edges = (LinkEdge *)realloc(g_edges, g_edgeCap * sizeof(LinkEdge));
    }
    if (!g_edges) return;
    lstrcpynW(g_edges[g_edgeN].src, src, MAX_PATH);
    int n = dstLen < 259 ? dstLen : 259;
    memcpy(g_edges[g_edgeN].dst, dst, n * sizeof(wchar_t));
    g_edges[g_edgeN].dst[n] = 0;
    g_edgeN++;
}

/* scan file text for [[wiki targets]]; fenced code blocks are skipped */
static void ScanFileForLinks(const wchar_t *path, const wchar_t *text)
{
    int inFence = 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] == L'`' && (i == 0 || text[i-1] == L'\n')) {
            int j = i;
            while (text[j] == L'`') j++;
            if (j - i >= 3) {
                inFence = !inFence;
                i = j - 1;
                continue;
            }
        }
        if (inFence) continue;
        if (text[i] == L'[' && text[i+1] == L'[') {
            int j = i + 2;
            while (text[j] && !(text[j] == L']' && text[j+1] == L']')) j++;
            if (text[j] == L']' && j > i + 2) {
                AddEdge(path, text + i + 2, j - i - 2);
                i = j + 1;
            }
        }
    }
}

static void LowerW(wchar_t *s)
{
    for (; *s; s++) *s = (wchar_t)towlower(*s);
}

/* base name without extension, lowercased for case-insensitive match */
static BOOL BaseNameNoExt(const wchar_t *path, wchar_t *out, int outCap)
{
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    const wchar_t *dot = wcsrchr(base, L'.');
    int n = (dot && dot != base) ? (int)(dot - base) : lstrlenW(base);
    if (n <= 0 || n >= outCap) return FALSE;
    memcpy(out, base, n * sizeof(wchar_t));
    out[n] = 0;
    LowerW(out);
    return TRUE;
}

void TreeScanLinks(void)
{
    g_edgeN = 0;
    g_fileN = 0;
    if (!g_root) return;
    CollectMd(g_root);
    for (int f = 0; f < g_fileN; f++) {
        char *u8 = NULL;
        int u8len = 0;
        if (!ReadAllBytes(g_mdFiles[f], &u8, &u8len) || !u8) {
            free(u8);
            continue;
        }
        int skip = 0;
        if (u8len >= 3 && (unsigned char)u8[0] == 0xEF
            && (unsigned char)u8[1] == 0xBB
            && (unsigned char)u8[2] == 0xBF)
            skip = 3;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, u8 + skip, u8len - skip,
                                       NULL, 0);
        if (wlen > 0) {
            wchar_t *w = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
            if (w) {
                MultiByteToWideChar(CP_UTF8, 0, u8 + skip, u8len - skip,
                                    w, wlen);
                w[wlen] = 0;
                ScanFileForLinks(g_mdFiles[f], w);
                free(w);
            }
        }
        free(u8);
    }
}

/* strip an optional trailing .md so [[Note]] matches [[Note.md]] */
static void NormTarget(const wchar_t *dst, wchar_t *out, int outCap)
{
    lstrcpynW(out, dst, outCap);
    wchar_t *dot = wcsrchr(out, L'.');
    if (dot && lstrcmpiW(dot, L".md") == 0) *dot = 0;
    LowerW(out);
}

int TreeBacklinks(const wchar_t *path, wchar_t (**out)[MAX_PATH])
{
    *out = NULL;
    if (!g_edgeN) return 0;
    wchar_t me[260];
    if (!BaseNameNoExt(path, me, 260)) return 0;
    wchar_t (*v)[MAX_PATH] = NULL;
    int n = 0, cap = 0;
    for (int e = 0; e < g_edgeN; e++) {
        wchar_t dst[260];
        NormTarget(g_edges[e].dst, dst, 260);
        if (lstrcmpW(dst, me) != 0) continue;
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (lstrcmpiW(v[k], g_edges[e].src) == 0) { dup = 1; break; }
        if (dup) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            wchar_t (*nv)[MAX_PATH] = (wchar_t (*)[MAX_PATH])realloc(
                v, cap * sizeof(*nv));
            if (!nv) break;
            v = nv;
        }
        lstrcpynW(v[n++], g_edges[e].src, MAX_PATH);
    }
    *out = v;
    return n;
}

int TreeOrphans(wchar_t (**out)[MAX_PATH])
{
    *out = NULL;
    if (!g_fileN) return 0;
    wchar_t (*v)[MAX_PATH] = (wchar_t (*)[MAX_PATH])malloc(
        g_fileN * sizeof(*v));
    if (!v) return 0;
    int n = 0;
    for (int f = 0; f < g_fileN; f++) {
        wchar_t me[260];
        if (!BaseNameNoExt(g_mdFiles[f], me, 260)) continue;
        BOOL linked = FALSE;
        for (int e = 0; e < g_edgeN && !linked; e++) {
            wchar_t dst[260];
            NormTarget(g_edges[e].dst, dst, 260);
            if (lstrcmpW(dst, me) == 0) linked = TRUE;
        }
        if (!linked) lstrcpynW(v[n++], g_mdFiles[f], MAX_PATH);
    }
    *out = v;
    return n;
}

static BOOL FileExists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES
        && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* resolve a wiki target to a real file:
 * 1) relative to the current document's folder (with/without .md)
 * 2) any workspace .md whose base name matches (case-insensitive) */
BOOL TreeJumpResolve(const wchar_t *docPath, const wchar_t *target,
                     wchar_t *out, int outCap)
{
    if (!target || !target[0]) return FALSE;
    wchar_t dir[MAX_PATH];
    dir[0] = 0;
    if (docPath && docPath[0]) {
        lstrcpynW(dir, docPath, MAX_PATH);
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0;
        else dir[0] = 0;
    }
    if (dir[0]) {
        wchar_t cand[MAX_PATH];
        wsprintfW(cand, L"%s\\%s", dir, target);
        if (FileExists(cand)) {
            lstrcpynW(out, cand, outCap);
            return TRUE;
        }
        wsprintfW(cand, L"%s\\%s.md", dir, target);
        if (FileExists(cand)) {
            lstrcpynW(out, cand, outCap);
            return TRUE;
        }
    }
    wchar_t want[260];
    NormTarget(target, want, 260);
    if (want[0] && g_fileN) {
        for (int f = 0; f < g_fileN; f++) {
            wchar_t base[260];
            if (BaseNameNoExt(g_mdFiles[f], base, 260)
                && lstrcmpW(base, want) == 0) {
                lstrcpynW(out, g_mdFiles[f], outCap);
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* public traversal: call cb(path, ctx) for every leaf .md file        */
/* ------------------------------------------------------------------ */

static void ForEachFileRec(TreeNode *n,
                           void (*cb)(const wchar_t *path, void *ctx),
                           void *ctx)
{
    for (; n; n = n->next) {
        if (n->isDir) {
            if (n->child) ForEachFileRec(n->child, cb, ctx);
        } else {
            cb(n->path, ctx);
        }
    }
}

void TreeForEachFile(void (*cb)(const wchar_t *path, void *ctx), void *ctx)
{
    if (!g_root || !g_root->child) return;
    ForEachFileRec(g_root->child, cb, ctx);
}
