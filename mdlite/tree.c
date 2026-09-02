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
static void TreeToolRect(RECT *r, int i, const RECT *rcTree);
static int  g_toolHover;             /* header tool hover, -1 none */
static int  RowAt(POINT pt, const RECT *rcClient);

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

    /* header row: fixed title + tool buttons */
    {
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
        RECT txt = { rh.left + SC(12), rh.top, rh.right - SC(100), rh.bottom };
        DrawTextW(dc, L"工作区", -1, &txt,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);

        /* tool buttons: new file / new folder / collapse all */
        for (int i = 0; i < 3; i++) {
            RECT r;
            TreeToolRect(&r, i, &rc);
            int hot = (g_toolHover == i);
            if (hot) {
                HBRUSH hb2 = CreateSolidBrush(RGB(0xDD, 0xDD, 0xE2));
                FillRect(dc, &r, hb2);
                DeleteObject(hb2);
            }
            HPEN tp = CreatePen(PS_SOLID, 1,
                                hot ? RGB(0x00, 0x62, 0xCC)
                                    : RGB(0x6E, 0x6E, 0x73));
            HPEN top = (HPEN)SelectObject(dc, tp);
            int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            int s = SC(8);
            if (i == 0) {            /* document sheet */
                MoveToEx(dc, cx - s / 2, cy - s, NULL);
                LineTo(dc, cx + s / 3, cy - s);
                LineTo(dc, cx + s / 2, cy - s * 2 / 3);
                LineTo(dc, cx + s / 2, cy + s);
                LineTo(dc, cx - s / 2, cy + s);
                LineTo(dc, cx - s / 2, cy - s);
                MoveToEx(dc, cx + s / 3, cy - s, NULL);
                LineTo(dc, cx + s / 3, cy - s * 2 / 3);
                LineTo(dc, cx + s / 2, cy - s * 2 / 3);
                /* small + */
                MoveToEx(dc, cx + s / 4, cy + s / 2, NULL);
                LineTo(dc, cx + s / 4 + SC(6), cy + s / 2);
                MoveToEx(dc, cx + s / 4 + SC(3), cy + s / 2 - SC(3), NULL);
                LineTo(dc, cx + s / 4 + SC(3), cy + s / 2 + SC(3));
            } else if (i == 1) {     /* folder */
                MoveToEx(dc, cx - s, cy - s / 3, NULL);
                LineTo(dc, cx - s / 3, cy - s / 3);
                LineTo(dc, cx, cy - s * 2 / 3);
                LineTo(dc, cx + s, cy - s * 2 / 3);
                LineTo(dc, cx + s, cy + s * 2 / 3);
                LineTo(dc, cx - s, cy + s * 2 / 3);
                LineTo(dc, cx - s, cy - s / 3);
                /* small + */
                MoveToEx(dc, cx + s / 3, cy, NULL);
                LineTo(dc, cx + s / 3 + SC(6), cy);
                MoveToEx(dc, cx + s / 3 + SC(3), cy - SC(3), NULL);
                LineTo(dc, cx + s / 3 + SC(3), cy + SC(3));
            } else {                 /* collapse: double chevron down */
                MoveToEx(dc, cx - SC(5), cy - SC(3), NULL);
                LineTo(dc, cx, cy + SC(1));
                LineTo(dc, cx + SC(5), cy - SC(3));
                MoveToEx(dc, cx - SC(5), cy + SC(2), NULL);
                LineTo(dc, cx, cy + SC(6));
                LineTo(dc, cx + SC(5), cy + SC(2));
            }
            SelectObject(dc, top);
            DeleteObject(tp);
        }
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

/* ------------------------------------------------------------------ */
/* VSCode-style tree tools: header buttons (new file / new folder /   */
/* collapse all), context menu, and an inline rename/create editor.   */
/* ------------------------------------------------------------------ */

static HWND   g_teWnd;               /* inline EDIT while active */
static WNDPROC g_teProc;
static int    g_teMode;              /* 1 new file 2 new dir 3 rename */
static TreeNode *g_teDir;            /* target dir (create) */
static TreeNode *g_teNode;           /* target node (rename) */
static wchar_t g_teOld[MAX_PATH];    /* rename: previous path */
static int    g_toolHover = -1;      /* 0 new file 1 new dir 2 collapse */

/* rect of header tool button i (0 file, 1 folder, 2 collapse) */
static void TreeToolRect(RECT *r, int i, const RECT *rcTree)
{
    int sz = SC(22);
    r->right = rcTree->right - SC(8) - (2 - i) * (sz + SC(6));
    r->left = r->right - sz;
    r->top = rcTree->top + (TreeHeaderH() - sz) / 2;
    r->bottom = r->top + sz;
}

static int TreeToolHit(POINT pt, const RECT *rcClient)
{
    if (!TreePtIn(pt)) return -1;
    RECT rcTree;
    TreeViewRect(&rcTree, rcClient);
    if (pt.y < rcTree.top || pt.y > rcTree.top + TreeHeaderH()) return -1;
    for (int i = 0; i < 3; i++) {
        RECT r;
        TreeToolRect(&r, i, &rcTree);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

void TreeToolHover(POINT pt)
{
    RECT rcClient;
    GetClientRect(g_hwnd, &rcClient);
    int h = g_teWnd ? -1 : TreeToolHit(pt, &rcClient);
    if (h != g_toolHover) {
        g_toolHover = h;
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

static void CollapseAll(TreeNode *n)
{
    for (; n; n = n->next) {
        if (n->isDir) {
            n->expanded = FALSE;
            CollapseAll(n->child);
        }
    }
}

static void TreeCollapseAll(void)
{
    if (!g_root) return;
    g_root->expanded = TRUE;      /* keep the root level visible */
    CollapseAll(g_root->child);
    g_scroll = 0;
    Flatten();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void CopyClip(const wchar_t *text)
{
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    int bytes = (lstrlenW(text) + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) {
        void *p = GlobalLock(g);
        if (p) {
            memcpy(p, text, bytes);
            GlobalUnlock(g);
            SetClipboardData(CF_UNICODETEXT, g);
        } else {
            GlobalFree(g);
        }
    }
    CloseClipboard();
}

/* refresh one directory node (re-enumerate children) */
static void ReEnumDir(TreeNode *dir)
{
    if (!dir) return;
    FreeNodes(dir->child);
    dir->child = NULL;
    dir->loaded = FALSE;
    EnumChildren(dir, NULL);
}

static void TreeEditCommit(void);
static void TreeEditCancel(void);
static LRESULT CALLBACK TeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

/* place the inline editor at a row (rowIdx is a flattened index) */
static void TreeEditShow(int rowIdx, int depth, const wchar_t *initial)
{
    RECT rcClient, rcTree;
    GetClientRect(g_hwnd, &rcClient);
    TreeViewRect(&rcTree, &rcClient);
    if (rowIdx < g_scroll) g_scroll = rowIdx;
    int x = rcTree.left + SC(10) + depth * SC(14);
    int y = rcTree.top + TreeHeaderH() + (rowIdx - g_scroll) * TreeRowH();
    RECT er = { x, y + SC(2), rcTree.right - SC(10), y + TreeRowH() - SC(2) };
    g_teWnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        er.left, er.top, er.right - er.left, er.bottom - er.top,
        g_hwnd, (HMENU)0x7E01, NULL, NULL);
    if (!g_teWnd) return;
    g_teProc = (WNDPROC)SetWindowLongPtrW(g_teWnd, GWLP_WNDPROC,
                                          (LONG_PTR)TeProc);
    SendMessageW(g_teWnd, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
    SendMessageW(g_teWnd, EM_SETSEL, 0, -1);
    SetFocus(g_teWnd);
}

static LRESULT CALLBACK TeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { TreeEditCommit(); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { TreeEditCancel();  return 0; }
    if (msg == WM_KILLFOCUS) { TreeEditCommit(); return 0; }
    if (msg == WM_CHAR && wp == L'\r') return 0;   /* Enter handled above */
    return CallWindowProcW(g_teProc, h, msg, wp, lp);
}

/* begin creating a file / folder inside `dir` */
static void TreeBeginCreate(TreeNode *dir, int mode)
{
    if (g_teWnd) TreeEditCommit();
    if (!dir || !dir->isDir) return;
    if (!dir->expanded) {
        dir->expanded = TRUE;
        EnumChildren(dir, NULL);
    }
    ReEnumDir(dir);
    Flatten();
    int rowIdx = 0;
    for (int i = 0; i < g_visN; i++)
        if (g_vis[i] == dir) { rowIdx = i + 1; break; }
    g_teMode = mode;
    g_teDir = dir;
    g_teNode = NULL;
    g_teOld[0] = 0;
    TreeEditShow(rowIdx, dir->depth + 1,
                 mode == 1 ? L"新建笔记.md" : L"新建文件夹");
}

static void TreeBeginRename(TreeNode *n)
{
    if (!n || n == g_root) return;
    if (g_teWnd) TreeEditCommit();
    Flatten();
    int rowIdx = 0;
    for (int i = 0; i < g_visN; i++)
        if (g_vis[i] == n) { rowIdx = i; break; }
    g_teMode = 3;
    g_teDir = NULL;
    g_teNode = n;
    lstrcpynW(g_teOld, n->path, MAX_PATH);
    TreeEditShow(rowIdx, n->depth, n->name);
}

static void TreeEditCancel(void)
{
    if (!g_teWnd) return;
    DestroyWindow(g_teWnd);
    g_teWnd = NULL;
    InvalidateRect(g_hwnd, NULL, FALSE);
    SetFocus(g_edit);
}

static void TreeEditCommit(void)
{
    if (!g_teWnd) return;
    wchar_t name[MAX_PATH];
    GetWindowTextW(g_teWnd, name, MAX_PATH);
    /* strip surrounding spaces */
    wchar_t *a = name, *b = name + lstrlenW(name);
    while (*a == L' ') a++;
    while (b > a && b[-1] == L' ') b--;
    *b = 0;
    HWND te = g_teWnd;
    g_teWnd = NULL;          /* prevent KILLFOCUS re-entry */
    DestroyWindow(te);

    if (name[0] && name[0] != L'.' && wcschr(name, L'\\') == NULL
        && wcschr(name, L'/') == NULL) {
        if (g_teMode == 1 || g_teMode == 2) {
            wchar_t path[MAX_PATH];
            wsprintfW(path, L"%s\\%s", g_teDir->path, name);
            if (g_teMode == 1) {
                HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                                       CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                       NULL);
                if (f != INVALID_HANDLE_VALUE) {
                    CloseHandle(f);
                    ReEnumDir(g_teDir);
                    Flatten();
                    InvalidateRect(g_hwnd, NULL, FALSE);
                    if (ConfirmDiscard()) LoadFile(path);
                }
            } else {
                if (CreateDirectoryW(path, NULL)) {
                    ReEnumDir(g_teDir);
                    Flatten();
                    InvalidateRect(g_hwnd, NULL, FALSE);
                }
            }
        } else if (g_teMode == 3 && g_teNode) {
            wchar_t newPath[MAX_PATH];
            wsprintfW(newPath, L"%s\\%s", g_teOld, name);
            wchar_t *slash = wcsrchr(newPath, L'\\');
            if (slash) lstrcpyW(slash + 1, name);
            if (MoveFileW(g_teOld, newPath)) {
                if (g_path[0] && lstrcmpiW(g_path, g_teOld) == 0)
                    lstrcpynW(g_path, newPath, MAX_PATH);
                TreeNode *parent = g_teNode->parent;
                if (parent) ReEnumDir(parent);
                Flatten();
                InvalidateRect(g_hwnd, NULL, FALSE);
            }
        }
    }
    g_teDir = NULL;
    g_teNode = NULL;
    InvalidateRect(g_hwnd, NULL, FALSE);
    SetFocus(g_edit);
}

BOOL TreeContextMenu(LPARAM lp)
{
    if (!g_shown) return FALSE;
    if (g_teWnd) TreeEditCommit();
    POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
    POINT client = pt;
    ScreenToClient(g_hwnd, &client);
    if (!TreePtIn(client)) return FALSE;
    RECT rcClient;
    GetClientRect(g_hwnd, &rcClient);
    int row = RowAt(client, &rcClient);
    if (row < 0 || row >= g_visN) return TRUE;   /* header: consume */
    TreeNode *n = g_vis[row];
    if (!n) return TRUE;

    HMENU pm = CreatePopupMenu();
    enum { C_OPEN = 1, C_RENAME, C_COPYPATH, C_COPYNAME, C_DELETE,
           C_NEWFILE, C_NEWDIR };
    if (n->isDir) {
        AppendMenuW(pm, MF_STRING, C_NEWFILE, L"新建文件");
        AppendMenuW(pm, MF_STRING, C_NEWDIR,  L"新建文件夹");
        AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
        AppendMenuW(pm, MF_STRING, C_RENAME,   L"重命名");
        AppendMenuW(pm, MF_STRING, C_COPYPATH, L"复制文件夹路径");
        AppendMenuW(pm, MF_STRING, C_COPYNAME, L"复制文件夹名");
    } else {
        AppendMenuW(pm, MF_STRING, C_OPEN,     L"打开");
        AppendMenuW(pm, MF_STRING, C_RENAME,   L"重命名");
        AppendMenuW(pm, MF_STRING, C_COPYPATH, L"复制文件地址");
        AppendMenuW(pm, MF_STRING, C_COPYNAME, L"复制文件名");
    }
    AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(pm, MF_STRING, C_DELETE, n->isDir ? L"删除文件夹"
                                                  : L"删除文件");
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(pm, TPM_RIGHTBUTTON | TPM_RETURNCMD
                                  | TPM_NONOTIFY,
                             pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(pm);
    if (cmd == C_OPEN) {
        if (ConfirmDiscard()) LoadFile(n->path);
    } else if (cmd == C_NEWFILE) {
        TreeBeginCreate(n, 1);
    } else if (cmd == C_NEWDIR) {
        TreeBeginCreate(n, 2);
    } else if (cmd == C_RENAME) {
        TreeBeginRename(n);
    } else if (cmd == C_COPYPATH) {
        CopyClip(n->path);
    } else if (cmd == C_COPYNAME) {
        CopyClip(n->name);
    } else if (cmd == C_DELETE) {
        wchar_t msg[MAX_PATH + 96];
        wsprintfW(msg, L"确定删除「%s」到回收站吗？", n->name);
        if (MessageBoxW(g_hwnd, msg, APP_NAME,
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
            wchar_t buf[MAX_PATH + 2];
            lstrcpynW(buf, n->path, MAX_PATH);
            buf[lstrlenW(buf) + 1] = 0;      /* double-NUL terminate */
            SHFILEOPSTRUCTW op;
            ZeroMemory(&op, sizeof(op));
            op.hwnd = g_hwnd;
            op.wFunc = FO_DELETE;
            op.pFrom = buf;
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
            if (SHFileOperationW(&op) == 0 && n->parent) {
                ReEnumDir(n->parent);
                Flatten();
                InvalidateRect(g_hwnd, NULL, FALSE);
            }
        }
    }
    return TRUE;
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
    if (g_teWnd) TreeEditCommit();
    RECT rcClient;
    GetClientRect(g_hwnd, &rcClient);
    int tool = TreeToolHit(pt, &rcClient);
    if (tool == 0) {                    /* new file in workspace root */
        Flatten();
        TreeBeginCreate(g_root, 1);
        return TRUE;
    }
    if (tool == 1) {                    /* new folder in workspace root */
        Flatten();
        TreeBeginCreate(g_root, 2);
        return TRUE;
    }
    if (tool == 2) {
        TreeCollapseAll();
        return TRUE;
    }
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
    TreeToolHover(pt);
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

static void CollectMd(const wchar_t *path, void *ctx)
{
    (void)ctx;
    if (g_fileN == g_fileCap) {
        g_fileCap = g_fileCap ? g_fileCap * 2 : 32;
        g_mdFiles = (wchar_t (*)[MAX_PATH])realloc(
            g_mdFiles, g_fileCap * sizeof(*g_mdFiles));
    }
    if (g_mdFiles)
        lstrcpynW(g_mdFiles[g_fileN++], path, MAX_PATH);
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

/* scan file text for links: [[wiki targets]] and [text](targets);
 * fenced code blocks are skipped */
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
            /* wiki-link [[target]] or [[target|label]] */
            int j = i + 2;
            while (text[j] && !(text[j] == L']' && text[j+1] == L']')) j++;
            if (text[j] == L']' && j > i + 2) {
                AddEdge(path, text + i + 2, j - i - 2);
                i = j + 1;
            }
        } else if (text[i] == L'[' && text[i+1] != L'[') {
            /* standard markdown link [text](target) */
            int j = i + 1;
            while (text[j] && text[j] != L']') j++;
            if (text[j] == L']' && text[j+1] == L'(') {
                int k = j + 2;
                while (text[k] && text[k] != L')') k++;
                if (text[k] == L')' && k > j + 1) {
                    int tlen = k - j - 1;
                    /* strip optional leading ./ or .\ */
                    const wchar_t *tgt = text + j + 2;
                    if (tgt[0] == L'.' && (tgt[1] == L'/' || tgt[1] == L'\\'))
                        tgt += 2;
                    AddEdge(path, tgt, tlen - (tgt - (text + j + 2)));
                }
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
    if (!g_wsDir[0]) return;
    TreeForEachFile(CollectMd, NULL);
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
/*                                                                     */
/* Walks the directory tree on disk with its own enumeration so it     */
/* never depends on which folders the user expanded in the sidebar     */
/* (the sidebar is lazy; the graph / backlink index must see all).     */
/* ------------------------------------------------------------------ */

static void ForEachDiskRec(const wchar_t *dir, int depth,
                           void (*cb)(const wchar_t *path, void *ctx),
                           void *ctx)
{
    if (depth > 6) return;                 /* runaway-nesting guard */
    wchar_t pat[MAX_PATH];
    wsprintfW(pat, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int count = 0;
    do {
        if (fd.cFileName[0] == L'.') continue;   /* hidden + . & .. */
        BOOL isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!isDir && !IsMdName(fd.cFileName)) continue;
        if (++count > TREE_MAX_PER_DIR) break;
        wchar_t full[MAX_PATH];
        wsprintfW(full, L"%s\\%s", dir, fd.cFileName);
        if (isDir)
            ForEachDiskRec(full, depth + 1, cb, ctx);
        else
            cb(full, ctx);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void TreeForEachFile(void (*cb)(const wchar_t *path, void *ctx), void *ctx)
{
    if (!g_wsDir[0]) return;
    ForEachDiskRec(g_wsDir, 0, cb, ctx);
}
