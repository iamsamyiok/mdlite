/* MDLite - a tiny markdown editor & viewer
   build: x86_64-w64-mingw32-gcc -Os ... */
#define _WIN32_WINNT 0x0601
#ifndef EM_GETSELTEXT
#define EM_GETSELTEXT 0x00B2
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include "mdlite.h"
#include "markdown.h"
#include "tree.h"
#include "graph.h"
#include "backlinks.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <wchar.h>

/* theme - Apple-style (macOS system colors) */
static const COLORREF COL_ACCENT   = RGB(0x00,0x7A,0xFF); /* systemBlue */
static const COLORREF COL_HEADERBG = RGB(0xF5,0xF5,0xF7); /* windowBg */
static const COLORREF COL_BORDER   = RGB(0xD2,0xD2,0xD7); /* separator */
static const COLORREF COL_BTNTXT   = RGB(0x1D,0x1D,0x1F); /* label */
static const COLORREF COL_BTNBRD   = RGB(0xD2,0xD2,0xD7);
static const COLORREF COL_BTNHVR   = RGB(0xE8,0xE8,0xED);
static const COLORREF COL_SEG_BG   = RGB(0xE8,0xE8,0xED);
static const COLORREF COL_TITLE    = RGB(0x1D,0x1D,0x1F);
static const COLORREF COL_STATTXT  = RGB(0x86,0x86,0x8B); /* secondaryLabel */

HWND   g_hwnd;
HWND   g_edit;
WNDPROC g_editProc;
int    g_dpi = 96;

static HFONT  g_fontEdit;
static HFONT  g_fontStatus;
HFONT  g_fontHeader;
static HFONT  g_fontHeaderBold;
static MDFonts g_fonts;

static BOOL   g_preview;   /* preview engine active (EDIT off, SPLIT/PREVIEW on) */
static int    g_view;     /* VIEW_EDIT / VIEW_SPLIT / VIEW_PREVIEW */
static MDDoc  g_doc;
static int    g_scrollY;
static BOOL   g_splitDrag;  /* dragging the split preview scrollbar */
static HMODULE g_hRichEd = NULL;   /* msftedit.dll handle (RichEdit50W) */
static BOOL   g_useRichEd = FALSE; /* TRUE if RichEdit loaded successfully */

wchar_t g_path[MAX_PATH] = L"";
wchar_t g_name[MAX_PATH] = L"";
static BOOL   g_dirty;

static int    g_hoverId = -1;   /* header hover: 0 open 1 save 2 history 3 settings 4 more 5 topmost 10 seg-edit 11 seg-split 12 seg-preview */
static DWORD  g_graphLastClickTick = 0;
static int    g_graphLastClickIdx  = -1;
static BOOL   g_graphDragging      = FALSE;
static BOOL   g_graphPanning       = FALSE;

/* zoom levels (percent), cycled with Ctrl+wheel */
static const int ZOOMS[5] = { 85, 100, 115, 130, 150 };
static int g_zoom = 1;

/* autosave interval in seconds (0 = off); global hotkey */
static int  g_autoSec = 60;
int  g_histMax = 20;   /* history entries kept per file */
static UINT g_hotMod = MOD_CONTROL | MOD_SHIFT;
static UINT g_hotVk  = VK_SPACE;

/* history panel state */
static BOOL  g_loading;        /* suppress EN_CHANGE side effects */
char  g_archivedTree[41]; /* tree already recorded (skip re-archiving) */

static BOOL    g_topmost;             /* keep window on top */
BOOL   g_indentRet    = TRUE; /* Enter inherits leading blanks */
/* eol/BOM fidelity: preserve the file's original flavor on save */
static BOOL g_eolLF   = TRUE; /* TRUE: write LF, FALSE: write CRLF */
static BOOL g_bomUtf8 = FALSE;

/* most-recently-used file list */
#define MRU_MAX 10
static wchar_t g_mru[MRU_MAX][MAX_PATH];
static int     g_mruN;




/* tray icon */
static UINT  g_taskbarMsg;            /* TaskbarCreated broadcast */
static BOOL  g_trayOn;
static NOTIFYICONDATAW g_nid;

/* find bar */
static HWND   g_findEdit;
static WNDPROC g_findProc;
static BOOL   g_findShown;
static wchar_t g_findQuery[128] = L"";
static wchar_t g_findInfo[64]   = L"";

/* draft recovery */
static BOOL g_draftSaved = FALSE;
static DWORD g_lastDraftTick = 0;

/* replace bar (shares the find row) */
static HWND   g_replEdit;
static WNDPROC g_replProc;
static wchar_t g_replQuery[136] = L"";

/* outline popup (Ctrl+P) */
static HWND   g_olWnd;          /* popup container */
static HWND   g_olFilter;       /* filter EDIT */
static HWND   g_olList;         /* LISTBOX of headings */
static WNDPROC g_olFilterProc;
static WNDPROC g_olListProc;
static int   *g_olLines;        /* editor line per list item */
static int    g_olLinesCap;

/* status bar cache (drives cheap 500ms-ui refresh) */
static int    g_stLine = -1, g_stCol = -1, g_stWords = -1;
static int    g_stLen = -1;      /* editor length when words last counted */
int    g_stBusySec = -1;  /* busy seconds last drawn */


int HeaderH(void) { return SC(48); }
int StatusH(void) { return SC(26); }

/* tree.c needs the find-bar state to align its top edge */
BOOL FindBarActive(void) { return g_findShown; }

int text_w(HDC hdc, HFONT font, const wchar_t *s, int len);
static void SaveSettings(void);
static void ShowSettings(void);
static void ApplyHotKey(BOOL quiet);
static void ApplyAutoSave(void);
static void MruLoad(void);
static void MruSave(void);
static void MruRemove(int idx);
static void MruPush(const wchar_t *path);
void DoPlainExport(void);
void DoPrint(void);
static void SetTopmost(BOOL on);
static BOOL CfgHaveKey(const wchar_t *name);
static DWORD CfgGetDword(const wchar_t *name, DWORD def);
static BOOL CfgGetStr(const wchar_t *name, wchar_t *out, int cch);
static void CfgSetDword(const wchar_t *name, DWORD v);
static void CfgSetStr(const wchar_t *name, const wchar_t *v);
void PathHash(const wchar_t *path, wchar_t *out);
void StartAi(const wchar_t *question);
void ShowSelAiMenu(void);
static BOOL TryRestoreDraft(void);
static BOOL WriteDraft(void);

static void GetBodyRect(RECT *rc)
{
    GetClientRect(g_hwnd, rc);
    rc->top += HeaderH() + (g_findShown ? SC(40) : 0);
    rc->bottom -= StatusH();
    /* graph view uses the full body area (ignores the tree sidebar),
     * so only inset the tree strip when the tree is actually shown */
    if (TreeShown() && g_view != VIEW_GRAPH) rc->left += TreeWidth();
}

/* ------------------------------------------------------------------ */
/* fonts / dpi                                                         */
/* ------------------------------------------------------------------ */

/* Wine's font fallback fails for pure-CJK strings (renders empty boxes),
   and it maps Segoe UI / Consolas / SimHei to faces without CJK glyphs.
   Windows font-links CJK reliably, so keep the classic faces there and
   pick faces with native CJK glyphs when running under Wine. */
static int WineEnv(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL;
}

/* ---- font availability probing ------------------------------------ */
/* Wine registers only some faces inside .ttc collections and does no
 * GDI font-linking, so a face name alone is not enough: we also verify
 * the face really renders CJK glyphs before using it. */

static int CALLBACK FaceEnumProc(const ENUMLOGFONTEXW *e,
                                 const TEXTMETRICW *tm, DWORD ft, LPARAM lp)
{
    (void)e; (void)tm; (void)ft;
    *(BOOL *)lp = TRUE;
    return 1; /* one hit is enough */
}

static BOOL FontAvailable(const wchar_t *name)
{
    HDC dc = GetDC(g_hwnd);
    LOGFONTW lf;
    BOOL found = FALSE;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, name, LF_FACESIZE);
    EnumFontFamiliesExW(dc, &lf, (FONTENUMPROCW)FaceEnumProc, (LPARAM)&found, 0);
    ReleaseDC(g_hwnd, dc);
    return found;
}

static BOOL FaceHasCJK(const wchar_t *name)
{
    HFONT f = CreateFontW(0, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, name);
    BOOL ok = FALSE;
    if (f) {
        HDC dc = GetDC(g_hwnd);
        HFONT old = (HFONT)SelectObject(dc, f);
        WORD gi[2] = { 0xFFFF, 0xFFFF };
        if (GetGlyphIndicesW(dc, L"\x4e2d\x6c38", 2, gi, 1) == 2)
            ok = (gi[0] != 0xFFFF && gi[1] != 0xFFFF);
        SelectObject(dc, old);
        DeleteObject(f);
        ReleaseDC(g_hwnd, dc);
    }
    return ok;
}

/* pick the first candidate that exists and (optionally) has CJK glyphs */
static const wchar_t *PickFace(const wchar_t *const *cands, int n, int needCJK)
{
    for (int i = 0; i < n; i++)
        if (FontAvailable(cands[i]) && (!needCJK || FaceHasCJK(cands[i])))
            return cands[i];
    return cands[0];
}

static const wchar_t *const k_uiFaces[] = {
    L"Noto Sans CJK SC", L"Noto Sans CJK JP", L"WenQuanYi Micro Hei",
    L"WenQuanYi Zen Hei", L"Microsoft YaHei", L"SimHei"
};
static const wchar_t *const k_monoFaces[] = {
    L"Noto Sans Mono CJK SC", L"Noto Sans Mono CJK JP",
    L"WenQuanYi Micro Hei Mono", L"Noto Sans CJK SC",
    L"WenQuanYi Micro Hei"
};

static wchar_t g_faceUI[LF_FACESIZE]   = L"Segoe UI";
static wchar_t g_faceMono[LF_FACESIZE] = L"Consolas";

const wchar_t *UiFaceName(void)  { return g_faceUI;  }
const wchar_t *MonoFaceName(void) { return g_faceMono; }

static void ResolveFaces(void)
{
    if (WineEnv()) {
        lstrcpynW(g_faceUI,   PickFace(k_uiFaces,  (int)(sizeof(k_uiFaces)/sizeof(*k_uiFaces)),     TRUE), LF_FACESIZE);
        lstrcpynW(g_faceMono, PickFace(k_monoFaces,(int)(sizeof(k_monoFaces)/sizeof(*k_monoFaces)), TRUE), LF_FACESIZE);
    } else {
        lstrcpynW(g_faceUI,   L"Segoe UI", LF_FACESIZE);
        lstrcpynW(g_faceMono, L"Consolas", LF_FACESIZE);
    }
}

static void CreateUiFonts(void)
{
    HDC dc = GetDC(g_hwnd);
    if (g_fontEdit)      DeleteObject(g_fontEdit);
    if (g_fontStatus)    DeleteObject(g_fontStatus);
    if (g_fontHeader)    DeleteObject(g_fontHeader);
    if (g_fontHeaderBold) DeleteObject(g_fontHeaderBold);

    int fdpi = MulDiv(g_dpi, ZOOMS[g_zoom], 100); /* text scales, chrome stays */

    ResolveFaces();
    const wchar_t *uiFace   = g_faceUI;
    const wchar_t *monoFace = g_faceMono;

    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;

    lstrcpynW(lf.lfFaceName, monoFace, LF_FACESIZE);
    lf.lfHeight = -MulDiv(11, fdpi, 72);
    g_fontEdit = CreateFontIndirectW(&lf);

    lstrcpynW(lf.lfFaceName, uiFace, LF_FACESIZE);
    lf.lfHeight = -MulDiv(9, g_dpi, 72);
    g_fontStatus = CreateFontIndirectW(&lf);

    lf.lfHeight = -MulDiv(95, g_dpi, 720);
    g_fontHeader = CreateFontIndirectW(&lf);

    lf.lfHeight = -MulDiv(95, g_dpi, 720);
    lf.lfWeight = FW_SEMIBOLD;
    g_fontHeaderBold = CreateFontIndirectW(&lf);

    md_init_fonts(&g_fonts, dc, fdpi);
    ReleaseDC(g_hwnd, dc);
}

/* Attempt to load RichEdit50W; returns TRUE on success. Falls back to EDIT. */
static BOOL InitRichEdit(void)
{
    if (g_hRichEd) return TRUE;
    g_hRichEd = LoadLibraryW(L"msftedit.dll");
    if (!g_hRichEd) return FALSE;
    /* RegisterClassEx must be called with the RichEdit class name after loading.
     * Win32 does this automatically on first CreateWindow("RichEdit50W"),
     * so we just set the flag. */
    g_useRichEd = TRUE;
    return TRUE;
}

static void ApplyEditPadding(void)
{
    RECT rcE;
    GetClientRect(g_edit, &rcE);
    RECT rc = { SC(16), SC(10), rcE.right - SC(16), rcE.bottom - SC(10) };
    if (rc.right < rc.left) rc.right = rc.left;
    if (rc.bottom < rc.top) rc.bottom = rc.top;
    SendMessageW(g_edit, EM_SETRECT, 0, (LPARAM)&rc);
    InvalidateRect(g_edit, NULL, TRUE);
}

/* preview pane rect inside the body; returns FALSE when preview is off */
static BOOL PreviewRect(RECT *rc)
{
    if (!g_preview) return FALSE;
    GetBodyRect(rc);
    if (g_view == VIEW_SPLIT) {
        int mid = (rc->left + rc->right) / 2;
        rc->right = mid - 1; /* 1px divider gutter */
    }
    return TRUE;
}

static void LayoutChildren(void)
{
    RECT rcBody;
    GetBodyRect(&rcBody);
    if (g_view == VIEW_SPLIT) {
        int mid = (rcBody.left + rcBody.right) / 2;
        MoveWindow(g_edit, mid + 1, rcBody.top,
                   rcBody.right - mid - 1, rcBody.bottom - rcBody.top, TRUE);
    } else {
        MoveWindow(g_edit, rcBody.left, rcBody.top,
                   rcBody.right - rcBody.left, rcBody.bottom - rcBody.top, TRUE);
    }
    ApplyEditPadding();
    if (g_findShown && g_findEdit) {
        int fx = SC(48);
        int fw = (rcBody.right - rcBody.left - SC(28)) * 38 / 100;
        MoveWindow(g_findEdit, fx, HeaderH() + SC(7), fw, SC(26), TRUE);
        if (g_replEdit) {
            int rx = fx + fw + SC(42);
            MoveWindow(g_replEdit, rx, HeaderH() + SC(7), fw, SC(26), TRUE);
        }
    }
    if (g_preview) {
        /* re-layout preview for the new width */
        RECT rcPrev;
        PreviewRect(&rcPrev);
        int len = GetWindowTextLengthW(g_edit);
        wchar_t *buf = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
        if (buf) {
            GetWindowTextW(g_edit, buf, len + 1);
            HDC dc = GetDC(g_hwnd);
            md_build(&g_doc, buf, len, &g_fonts, dc,
                     rcPrev.right - rcPrev.left);
            ReleaseDC(g_hwnd, dc);
            free(buf);
        }
        RECT rcAny;
        PreviewRect(&rcAny);
        SCROLLINFO si;
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = g_doc.height;
        si.nPage = rcAny.bottom - rcAny.top;
        if (g_scrollY > si.nMax - (int)si.nPage && si.nMax > (int)si.nPage)
            g_scrollY = si.nMax - si.nPage;
        if (g_scrollY < 0) g_scrollY = 0;
        si.nPos = g_scrollY;
        if (g_view == VIEW_PREVIEW)
            SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
        else
            ShowScrollBar(g_hwnd, SB_VERT, FALSE);
    }
}

/* ------------------------------------------------------------------ */
/* title & status                                                      */
/* ------------------------------------------------------------------ */

static void UpdateTitle(void)
{
    static wchar_t last[MAX_PATH + 64] = L"";
    wchar_t buf[MAX_PATH + 64];
    if (g_name[0])
        wsprintfW(buf, L"%s%s - " APP_NAME, g_dirty ? L"* " : L"", g_name);
    else
        wsprintfW(buf, L"%s%s - " APP_NAME, g_dirty ? L"* " : L"", UNTITLED);
    if (lstrcmpW(last, buf) == 0) return; /* avoid churn per keystroke */
    lstrcpynW(last, buf, MAX_PATH + 64);
    SetWindowTextW(g_hwnd, buf);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* file io                                                             */
/* ------------------------------------------------------------------ */

static void SetPath(const wchar_t *path)
{
    if (path && path[0]) {
        lstrcpynW(g_path, path, MAX_PATH);
        const wchar_t *base = wcsrchr(path, L'\\');
        const wchar_t *base2 = wcsrchr(path, L'/');
        if (base2 > base) base = base2;
        lstrcpynW(g_name, base ? base + 1 : path, MAX_PATH);
    } else {
        g_path[0] = 0;
        g_name[0] = 0;
    }
    TreeSync(path);   /* workspace follows the current document */
}

/* ---- external modification watch (item 30) ---- */
static FILETIME g_extTime;
static DWORD    g_extSize;
static BOOL     g_extValid;
static BOOL     g_extNoticed;

static void ExtMark(void)
{
    g_extValid = FALSE;
    g_extNoticed = FALSE;
    if (!g_path[0]) return;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(g_path, GetFileExInfoStandard, &fad)) return;
    g_extTime = fad.ftLastWriteTime;
    g_extSize = fad.nFileSizeLow;
    g_extValid = TRUE;
}

BOOL LoadFile(const wchar_t *path)   /* public: tree.c opens files too */
{
    /* normalize to an absolute path so the git snapshot dir resolves */
    wchar_t full[MAX_PATH];
    DWORD fn = path ? GetFullPathNameW(path, MAX_PATH, full, NULL) : 0;
    if (fn > 0 && fn < MAX_PATH) path = full;

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 32u * 1024 * 1024) {
        CloseHandle(h);
        MessageBoxW(g_hwnd, L"文件过大或读取失败。", APP_NAME, MB_ICONERROR);
        return FALSE;
    }
    char *u8 = (char *)malloc(size + 1);
    DWORD rd = 0;
    BOOL ok = ReadFile(h, u8, size, &rd, NULL);
    CloseHandle(h);
    if (!ok) { free(u8); return FALSE; }

    int off = 0;
    g_bomUtf8 = FALSE;
    if (rd >= 3 && (BYTE)u8[0] == 0xEF && (BYTE)u8[1] == 0xBB
        && (BYTE)u8[2] == 0xBF) {
        off = 3; rd -= 3;
        g_bomUtf8 = TRUE;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, u8 + off, rd, NULL, 0);
    wchar_t *wbuf = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
    if (!wbuf) { free(u8); return FALSE; }
    MultiByteToWideChar(CP_UTF8, 0, u8 + off, rd, wbuf, wlen);
    wbuf[wlen] = 0;
    g_eolLF = (wmemchr(wbuf, L'\r', wlen) == NULL);
    free(u8);

    SetWindowTextW(g_edit, wbuf);
    free(wbuf);

    SendMessageW(g_edit, EM_SETSEL, 0, 0);
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
    SetPath(path);
    g_dirty = FALSE;
    UpdateTitle();
    MruPush(path);
    MruSave();
    ExtMark();
    return TRUE;
}



static void LinksFill(void);          /* forward: refresh after save */
static BOOL LinksPanelOpen(void);     /* forward: is the panel visible */
static BOOL SaveFileEx(const wchar_t *path, BOOL isAuto)
{
    int len = GetWindowTextLengthW(g_edit);
    wchar_t *wbuf = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!wbuf) return FALSE;
    GetWindowTextW(g_edit, wbuf, len + 1);

    int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, NULL, 0, NULL, NULL);
    char *u8 = (char *)malloc(u8len + 4);
    if (!u8) { free(wbuf); return FALSE; }
    WideCharToMultiByte(CP_UTF8, 0, wbuf, len, u8, u8len, NULL, NULL);
    free(wbuf);

    /* restore the file's original eol flavor (EDIT always yields CRLF) */
    if (g_eolLF) {
        int w = 0;
        for (int r = 0; r < u8len; r++) {
            if (u8[r] == '\r' && r + 1 < u8len && u8[r + 1] == '\n') continue;
            u8[w++] = u8[r];
        }
        u8len = w;
    }
    if (g_bomUtf8) {
        memmove(u8 + 3, u8, u8len);
        u8[0] = (char)0xEF; u8[1] = (char)0xBB; u8[2] = (char)0xBF;
        u8len += 3;
    }

    if (!WriteAllBytes(path, u8, u8len)) { free(u8); return FALSE; }

    SetPath(path);
    g_dirty = FALSE;
    UpdateTitle();
    GitArchive(u8, u8len, isAuto);
    ExtMark();
    free(u8);
    /* live refresh: keep the graph view and an open backlinks panel
     * in sync with what was just written to disk */
    if (g_view == VIEW_GRAPH) {
        GraphBuild();
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
    if (LinksPanelOpen()) LinksFill();
    /* materialized backlinks: keep the auto block at the end of
     * every linked note in sync with this file's outgoing links */
    if (!wcsstr(path, L"\\.mdlite")) {
        const wchar_t *vault = TreeVaultDir();
        if (vault[0]) {
            BacklinksSyncDir(vault);
        } else {
            wchar_t dir[MAX_PATH];
            lstrcpynW(dir, path, MAX_PATH);
            wchar_t *sl = wcsrchr(dir, L'\\');
            if (sl) { *sl = 0; BacklinksSyncDir(dir); }
        }
    }
    return TRUE;
}

static BOOL PromptSaveAs(wchar_t *buf, int cch)
{
    buf[0] = 0;
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Markdown (*.md;*.markdown;*.mdown;*.txt)\0"
                      "*.md;*.markdown;*.mdown;*.txt\0"
                      L"所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = cch;
    lstrcpynW(buf, g_name[0] ? g_name : L"未命名.md", cch);
    ofn.lpstrDefExt = L"md";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetSaveFileNameW(&ofn);
}


/* ---- window topmost toggle (header button 5) ---- */

static void SetTopmost(BOOL on)
{
    g_topmost = on;
    SetWindowPos(g_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    InvalidateRect(g_hwnd, NULL, FALSE);   /* repaint the pin toggle */
}


static const wchar_t *HELP_TEXT =
    L"MDLite 使用帮助\r\n"
    L"\r\n"
    L"【视图】\r\n"
    L"编辑 / 分屏 / 预览三态：Ctrl+/ 切换，或点右上分段。\r\n"
    L"缩放：Ctrl+滚轮 85%-150% 五档。\r\n"
    L"\r\n"
    L"【文件】\r\n"
    L"Ctrl+N 新建，Ctrl+O 打开，Ctrl+S 保存，Ctrl+Shift+S 另存；拖拽 .md 直接打开。\r\n"
    L"自动保存：设置中可配间隔（秒，0=关闭），默认 60 秒。\r\n"
    L"文件树（Ctrl+B）：标题栏右侧三个工具按钮——新建文件、新建文件夹、\r\n"
    L"  折叠全部；Enter 或单击打开文件，双击目录展开/折叠；右键菜单：\r\n"
    L"  打开、新建文件/文件夹（目录上）、重命名、复制文件地址/文件名、\r\n"
    L"  删除（进回收站）；Enter 确认命名，Esc 取消。\r\n"
    L"\r\n"
    L"【编辑】\r\n"
    L"查找：Ctrl+F，替换：Ctrl+H；Enter 查找下一个，Shift+Enter 上一个；\r\n"
    L"  替换框 Enter 替换当前，Ctrl+Enter 全部替换，Esc 关闭。\r\n"
    L"大纲：Ctrl+P 弹出标题列表，输入过滤，Enter 跳转，Esc 关闭。\r\n"
    L"括号：输入 [ ( { 自动配对并可包裹选区；输入 ) ] } 跳出配对；退格删除整对。\r\n"
    L"\r\n"
    L"【斜杠命令】\r\n"
    L"输入 / 弹出块命令菜单：任务/无序/有序列表、代码块、表格、引用块、\r\n"
    L"  提示框、突出显示、帽头、分隔线、今日日期（11 项）。\r\n"
    L"  输入即过滤，↑↓ 选择，Enter/Tab 应用，Esc 关闭；弹窗高度随条目\r\n"
    L"  数量自适应。标题直接用 # 手打更快，故不进菜单。\r\n"
    L"\r\n"
    L"【双链与图谱】\r\n"
    L"双链补全：输入 [[ 自动弹出工作区笔记列表，继续输入过滤，\r\n"
    L"  ↑↓ 选择、Enter/Tab 补全、Esc 关闭。\r\n"
    L"反向链接：Ctrl+Shift+L 弹出反向链接与孤儿笔记面板；双击条目跳转，\r\n"
    L"  Esc 关闭；保存后自动刷新。\r\n"
    L"知识图谱：Ctrl+G 进入/退出全库链接图；滚轮缩放、拖拽节点、\r\n"
    L"  双击跳转，Esc 返回。节点右上角数字为连接数。\r\n"
    L"\r\n"
    L"【格式与渲染】\r\n"
    L"突出显示：整行用 == 包裹（==重点内容==）显示为黄色高亮条，\r\n"
    L"  斜杠命令选「突出显示」快速插入。\r\n"
    L"提示框：> [!NOTE] / [!TIP] / [!IMPORTANT] / [!WARNING] / [!CAUTION]\r\n"
    L"  渲染为五色卡片；斜杠命令选「提示框」插入骨架。\r\n"
    L"帽头：三个横杠包裹的文档头（title/tags/date），斜杠命令选「帽头」，\r\n"
    L"  日期自动填今天。\r\n"
    L"插入片段：编辑器按 @ 弹出 Markdown 片段菜单（粗体、斜体、链接、\r\n"
    L"  图片、脚注等 17 项），输入过滤、Enter 插入、Esc 关闭。\r\n"
    L"\r\n"
    L"【AI 与 Agent】\r\n"
    L"AI：行首输入 //问题 后回车发送（OpenAI 兼容接口），Esc 中断。\r\n"
    L"Agent：行首输入 ///任务 后回车，调用 opencode 在文档目录执行，\r\n"
    L"  Esc 终止；超时秒数可在设置中调整。\r\n"
    L"\r\n"
    L"【其他】\r\n"
    L"置顶：标题栏「置顶」按钮切换窗口总在最前。\r\n"
    L"分享：更多菜单可导出自包含 HTML（图片内嵌），单文件发任意设备浏览器可看。\r\n"
    L"历史：点「历史」查看版本，点击恢复；行右侧 ✕ 删除单条；上限在设置中调整。\r\n"
    L"热键：设置中可配全局呼出热键（默认 Ctrl+Shift+Space）。\r\n"
    L"托盘：关闭窗口最小化到托盘，右键退出。\r\n"
    L"本帮助：更多菜单「帮助」或 F1 打开。";

static LRESULT CALLBACK HelpProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        DestroyWindow(h);
        return 0;
    }
    if (msg == WM_SIZE) {
        HWND ed = GetWindow(h, GW_CHILD);
        if (ed) MoveWindow(ed, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    }
    if (msg == WM_DESTROY) {
        HWND owner = GetWindow(h, GW_OWNER);
        if (owner) EnableWindow(owner, TRUE);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ShowHelp(void)
{
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = HelpProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"MDLiteHelp";
        if (!RegisterClassW(&wc)) return;
        registered = TRUE;
    }
    /* one window at a time */
    HWND exist = FindWindowW(L"MDLiteHelp", NULL);
    if (exist) { SetForegroundWindow(exist); return; }

    EnableWindow(g_hwnd, FALSE);   /* modal-ish */
    int w = SC(600), h = SC(560);
    RECT rcMain;
    GetWindowRect(g_hwnd, &rcMain);
    int x = rcMain.left + (rcMain.right - rcMain.left - w) / 2;
    int y = rcMain.top + (rcMain.bottom - rcMain.top - h) / 2;
    HWND hw = CreateWindowExW(WS_EX_TOOLWINDOW, L"MDLiteHelp",
        L"MDLite 帮助",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
        x, y, w, h, g_hwnd, NULL, NULL, NULL);
    if (!hw) { EnableWindow(g_hwnd, TRUE); return; }
    HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", HELP_TEXT,
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY
        | WS_VSCROLL | ES_AUTOVSCROLL,
        0, 0, w, h, hw, (HMENU)1, NULL, NULL);
    SendMessageW(ed, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
    ShowWindow(hw, SW_SHOW);
    SetFocus(ed);
}

/* "more" dropdown anchored under header button 4 */
static RECT BtnRect(int id);
static void ShowMoreMenu(void)
{
    HMENU pm = CreatePopupMenu();
    AppendMenuW(pm, MF_STRING, IDM_TREEBAR,   L"文件树\tCtrl+B");
    AppendMenuW(pm, MF_STRING, IDM_HISTORY,   L"历史");
    AppendMenuW(pm, MF_STRING, IDM_SETTINGS,  L"配置\tCtrl+,");
    AppendMenuW(pm, MF_STRING, IDM_HELP,      L"帮助\tF1");
    AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(pm, MF_STRING, IDM_OUTLINE,    L"大纲\tCtrl+P");
    AppendMenuW(pm, MF_STRING, IDM_LINKS,      L"反向链接与孤儿笔记\tCtrl+Shift+L");
    AppendMenuW(pm, MF_STRING, IDM_GRAPH,      L"知识图谱\tCtrl+G");
    AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(pm, MF_STRING, IDM_COPYHTML,   L"复制为 HTML");
    AppendMenuW(pm, MF_STRING, IDM_EXPORTHTML, L"导出 HTML…");
    AppendMenuW(pm, MF_STRING, IDM_SHAREHTML,  L"导出分享 HTML（内嵌图片）…");
    AppendMenuW(pm, MF_STRING, IDM_EXPORTTXT,  L"导出纯文本…");
    AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
    if (g_mruN > 0) {
        HMENU sub = CreatePopupMenu();
        for (int i = 0; i < g_mruN; i++) {
            const wchar_t *p = g_mru[i];
            const wchar_t *name = p, *slash = p;
            for (const wchar_t *q = p; *q; q++)
                if (*q == L'\\' || *q == L'/') slash = q + 1;
            name = slash;
            wchar_t txt[MAX_PATH + 64];
            wchar_t dir[MAX_PATH];
            lstrcpynW(dir, p, (int)(slash - p) + 1);
            wchar_t cdir[96];
            PathCompactPathExW(cdir, dir, 48, 0);
            wsprintfW(txt, L"%s\t%s", name, cdir);
            AppendMenuW(sub, MF_STRING, IDM_MRU_BASE + i, txt);
        }
        AppendMenuW(pm, MF_POPUP, (UINT_PTR)sub, L"最近文件");
        AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(pm, MF_STRING, IDM_PRINT,      L"打印 / 导出 PDF…");
    AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(pm, MF_STRING, IDM_HELP,       L"帮助");
    POINT pt = { BtnRect(2).left, BtnRect(2).bottom + SC(2) };
    ClientToScreen(g_hwnd, &pt);
    int cmd = TrackPopupMenu(pm, TPM_LEFTALIGN | TPM_RIGHTBUTTON
                                | TPM_RETURNCMD, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(pm);
    if (cmd >= IDM_MRU_BASE && cmd < IDM_MRU_BASE + MRU_MAX) {
        int i = cmd - IDM_MRU_BASE;
        if (i < g_mruN) {
            if (ConfirmDiscard()) {
                if (!LoadFile(g_mru[i])) {
                    MessageBoxW(g_hwnd, L"无法打开文件，已从列表移除。",
                                APP_NAME, MB_ICONWARNING);
                    MruRemove(i);
                }
            }
        }
        return;
    }
    if (cmd)
        SendMessageW(g_hwnd, WM_EDITCMD, cmd, 0);
}

static void DoOpen(void)
{
    wchar_t buf[MAX_PATH];
    buf[0] = 0;
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Markdown (*.md;*.markdown;*.mdown;*.txt)\0"
                      "*.md;*.markdown;*.mdown;*.txt\0"
                      L"所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn))
        if (!LoadFile(buf))
            MessageBoxW(g_hwnd, L"无法打开文件。", APP_NAME, MB_ICONERROR);
}

static BOOL DoSaveEx(BOOL isAuto)
{
    if (!g_path[0]) {
        wchar_t buf[MAX_PATH];
        if (!PromptSaveAs(buf, MAX_PATH)) return FALSE;
        lstrcpynW(g_path, buf, MAX_PATH);
    }
    if (!SaveFileEx(g_path, isAuto)) {
        if (!isAuto)
            MessageBoxW(g_hwnd, L"保存失败。", APP_NAME, MB_ICONERROR);
        return FALSE;
    }
    /* once a document is saved, draft is no longer relevant */
    if (isAuto) WriteDraft();
    g_draftSaved = FALSE;
    return TRUE;
}

static BOOL DoSave(void)
{
    BOOL ok = DoSaveEx(FALSE);
    if (ok && g_path[0]) {
        MruPush(g_path);
        MruSave();
    }
    return ok;
}

/* ---- temp-session autosave -------------------------------------- */
/* Documents that were never saved to a user folder get adopted into
 * <exe dir>\mdlite\autosave\ as real files named
 * YYYY-MM-DD_HH-MM-SS_v001.md. From then on the regular per-minute
 * autosave (TIMER_AUTO -> DoSaveEx) keeps them persisted, quitting
 * saves them silently, and the next launch without a file argument
 * reopens the newest one. */

static void AutoSaveDir(wchar_t *out, int cch)
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) { out[0] = 0; return; }
    wchar_t *slash = exe;
    for (wchar_t *p = exe; *p; p++)
        if (*p == L'\\' || *p == L'/') slash = p;
    *++slash = 0;
    lstrcpynW(out, exe, cch);
    lstrcatW(out, L"mdlite");
    CreateDirectoryW(out, NULL);
    lstrcatW(out, L"\\autosave");
    CreateDirectoryW(out, NULL);
}

static BOOL PathInAutoSaveDir(void)
{
    if (!g_path[0]) return FALSE;
    wchar_t dir[MAX_PATH]; AutoSaveDir(dir, MAX_PATH);
    if (!dir[0]) return FALSE;
    return wcsncmp(g_path, dir, lstrlenW(dir)) == 0;
}

/* first autosave of an untitled document: adopt a session file so all
 * regular save paths take over from here */
static BOOL AdoptSessionFile(void)
{
    wchar_t dir[MAX_PATH]; AutoSaveDir(dir, MAX_PATH);
    if (!dir[0]) return FALSE;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%s\\%04d-%02d-%02d_%02d-%02d-%02d_v001.md",
              dir, (int)st.wYear, (int)st.wMonth, (int)st.wDay,
              (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    SetPath(path);
    return DoSaveEx(TRUE);
}

/* reopen the newest temp-session file when starting without a file
 * argument; a restored draft or any loaded document wins */
static void RestoreLatestSession(void)
{
    if (g_path[0] || GetWindowTextLengthW(g_edit) > 0) return;
    wchar_t dir[MAX_PATH]; AutoSaveDir(dir, MAX_PATH);
    if (!dir[0]) return;
    wchar_t pat[MAX_PATH]; wsprintfW(pat, L"%s\\*.md", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    wchar_t best[MAX_PATH]; best[0] = 0;
    ULONGLONG bestv = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULONGLONG v = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32)
                    | fd.ftLastWriteTime.dwLowDateTime;
        if (v > bestv) {
            bestv = v;
            wsprintfW(best, L"%s\\%s", dir, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (best[0]) LoadFile(best);
}

BOOL ConfirmDiscard(void)   /* public: tree.c checks before opening */
{
    if (!g_dirty) return TRUE;
    /* temp-session files under the autosave dir are saved silently:
     * the user never picked a folder, so quitting keeps the content */
    if (PathInAutoSaveDir()) return DoSaveEx(TRUE);
    int r = MessageBoxW(g_hwnd, L"文档已修改，是否保存更改？", APP_NAME,
                        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return FALSE;
    if (r == IDYES) return DoSave();
    return TRUE;
}

void SetView(int v);

static void DoNew(void)
{
    if (!ConfirmDiscard()) return;
    if (g_view != VIEW_EDIT) SetView(VIEW_EDIT);
    SetWindowTextW(g_edit, L"");
    SetPath(NULL);
    g_dirty = FALSE;
    g_eolLF = TRUE;
    g_bomUtf8 = FALSE;
    g_draftSaved = FALSE;
    UpdateTitle();
}

static void UpdateScroll(void)
{
    RECT rcBody;
    GetBodyRect(&rcBody);
    int viewH = rcBody.bottom - rcBody.top;
    int maxScroll = g_doc.height - viewH;
    if (maxScroll < 0) maxScroll = 0;
    if (g_scrollY > maxScroll) g_scrollY = maxScroll;
    if (g_scrollY < 0) g_scrollY = 0;
    if (g_view == VIEW_PREVIEW) {
        SCROLLINFO si;
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
        si.nMin = 0;
        si.nMax = g_doc.height;
        si.nPage = viewH;
        si.nPos = g_scrollY;
        SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
    }
}

void SetView(int v)
{
    if (v == g_view) return;
    if (v == VIEW_GRAPH) {
        g_preview = FALSE;
        g_view = VIEW_GRAPH;
        md_free(&g_doc);
        ShowWindow(g_edit, SW_HIDE);
        ShowScrollBar(g_hwnd, SB_VERT, FALSE);
        GraphBuild();
        GraphResetView();
        LayoutChildren();
        SetFocus(g_hwnd);   /* keyboard (Esc etc.) must reach the main window */
        InvalidateRect(g_hwnd, NULL, TRUE);
        return;
    }
    if (v == VIEW_EDIT) {
        g_preview = FALSE;
        g_view = VIEW_EDIT;
        md_free(&g_doc);
        ShowScrollBar(g_hwnd, SB_VERT, FALSE);
        ShowWindow(g_edit, SW_SHOW);
        LayoutChildren();
        SetFocus(g_edit);
    } else {
        int len = GetWindowTextLengthW(g_edit);
        wchar_t *buf = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
        if (!buf) return;
        GetWindowTextW(g_edit, buf, len + 1);
        g_view = v;
        g_preview = TRUE;
        RECT rcPrev;
        PreviewRect(&rcPrev);
        HDC dc = GetDC(g_hwnd);
        md_build(&g_doc, buf, len, &g_fonts, dc,
                 rcPrev.right - rcPrev.left);
        ReleaseDC(g_hwnd, dc);
        free(buf);
        g_scrollY = 0;
        if (v == VIEW_PREVIEW) {
            ShowWindow(g_edit, SW_HIDE);
            UpdateScroll();
            SetFocus(g_hwnd);   /* Esc-to-exit must reach the main window */
        } else {
            ShowWindow(g_edit, SW_SHOW);
            ShowScrollBar(g_hwnd, SB_VERT, FALSE);
            SetFocus(g_edit);
        }
        LayoutChildren();
    }
    InvalidateRect(g_hwnd, NULL, TRUE);
}

/* debounced preview refresh (kicked on EN_CHANGE while preview is live) */
static void KickPreview(void)
{
    if (!g_preview) return;
    KillTimer(g_hwnd, TIMER_KICK);
    SetTimer(g_hwnd, TIMER_KICK, 300, NULL);
}

static void RefreshPreviewNow(void)
{
    if (!g_preview) return;
    int len = GetWindowTextLengthW(g_edit);
    wchar_t *buf = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!buf) return;
    GetWindowTextW(g_edit, buf, len + 1);
    RECT rcPrev;
    PreviewRect(&rcPrev);
    /* keep the viewport anchored while the document reflows: hold the
     * scroll fraction so typing doesn't make the preview jump */
    int oldH = g_doc.height;
    int oldY = g_scrollY;
    HDC dc = GetDC(g_hwnd);
    md_build(&g_doc, buf, len, &g_fonts, dc, rcPrev.right - rcPrev.left);
    ReleaseDC(g_hwnd, dc);
    free(buf);
    RECT rcAny;
    PreviewRect(&rcAny);
    int viewH = rcAny.bottom - rcAny.top;
    if (oldH > viewH && viewH > 0) {
        int maxNew = g_doc.height - viewH;
        if (maxNew < 0) maxNew = 0;
        g_scrollY = (int)((long long)oldY * maxNew / (oldH - viewH));
    } else {
        g_scrollY = 0;
    }
    if (g_scrollY > g_doc.height - viewH) g_scrollY = g_doc.height - viewH;
    if (g_scrollY < 0) g_scrollY = 0;
    if (g_view == VIEW_PREVIEW) UpdateScroll();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void ApplyZoom(int dir)
{
    int nz = ((g_zoom + dir) % 5 + 5) % 5;
    if (nz == g_zoom) return;
    g_zoom = nz;
    CreateUiFonts();
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fontEdit, TRUE);
    if (g_findEdit)
        SendMessageW(g_findEdit, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
    if (g_preview)
        RefreshPreviewNow();
    else
        ApplyEditPadding();
    InvalidateRect(g_hwnd, NULL, TRUE);
}

/* ------------------------------------------------------------------ */
/* header widgets                                                      */
/* ------------------------------------------------------------------ */

static RECT BtnRect(int id) /* 0 open 1 save 2 more 3 topmost */
{
    RECT rc;
    if (id == 3) {                 /* narrow pin toggle right of more */
        rc.left = SC(14) + 3 * (SC(64) + SC(8));
        rc.top = SC(10);
        rc.right = rc.left + SC(48);
        rc.bottom = rc.top + SC(28);
        return rc;
    }
    rc.left = SC(14) + id * (SC(64) + SC(8));
    rc.top = SC(10);
    rc.right = rc.left + SC(64);
    rc.bottom = rc.top + SC(28);
    return rc;
}

static RECT SegRect(void)
{
    RECT rc;
    rc.right = 0;
    RECT rcClient;
    GetClientRect(g_hwnd, &rcClient);
    rc.left = rcClient.right - SC(14) - SC(180);
    rc.top = SC(10);
    rc.right = rcClient.right - SC(14);
    rc.bottom = rc.top + SC(28);
    return rc;
}

static int HitTestHeader(POINT pt)
{
    POINT p = pt;
    if (p.y < 0 || p.y > HeaderH()) return -1;
    for (int i = 0; i < 4; i++) {
        RECT r = BtnRect(i);
        if (PtInRect(&r, p)) return i;
    }
    RECT s = SegRect();
    if (PtInRect(&s, p)) {
        int third = (s.right - s.left) / 3;
        int idx = (p.x - s.left) / third;
        if (idx < 0) idx = 0;
        if (idx > 2) idx = 2;
        return 10 + idx;
    }
    return -1;
}


/* ------------------------------------------------------------------ */
/* history dropdown panel (custom-drawn, newest first, 10 rows + wheel) */
/* ------------------------------------------------------------------ */

#define HIST_VIS  10

typedef struct {
    char    sha[41];
    wchar_t time[20];
    BOOL    isAuto;
    BOOL    pinned;   /* entry is in the pinned group */
    BOOL    isHdr;    /* group header row ("已钉住") */
} HistEntry;

/* pinned snapshots for the current file (item 22) */
#define PIN_MAX 50
static char    g_pins[PIN_MAX][41];
static int     g_pinN;
static wchar_t g_pinKey[16];       /* "P" + path hash */
static HistEntry *g_histWalk;      /* walked chain, panel-lifetime */
static int     g_histWalkN;

static HistEntry *g_histList;
static int  g_histCount;
static HWND g_histWnd;
static int  g_histScroll, g_histHover = -1, g_histPick = -1;
static int  g_histDelPick = -1;      /* row pending delete confirm */
static int  g_histDelHover = -1;     /* row whose X is hovered */
static int  g_histPinHover = -1;     /* row whose pin is hovered */
static wchar_t g_histRepo[MAX_PATH]; /* repo path valid while panel open */
static BOOL g_histDragBar;
static int  g_histDragY0, g_histScroll0;

static int HistRowH(void) { return SC(32); }

/* ---- pinned snapshot bookkeeping ---- */

static void ShaToWide(const char *sha, wchar_t *out, int cch)
{
    int i;
    for (i = 0; i < 40 && i < cch - 1 && sha[i]; i++)
        out[i] = (wchar_t)(unsigned char)sha[i];
    out[i] = 0;
}

static void WideToSha(const wchar_t *in, char *sha, int cch)
{
    int i;
    for (i = 0; i < 40 && i < cch - 1 && in[i]; i++)
        sha[i] = (char)in[i];
    sha[i] = 0;
}

void PinLoad(const wchar_t *path)
{
    g_pinN = 0;
    wchar_t hash[9];
    PathHash(path, hash);
    wsprintfW(g_pinKey, L"P%s", hash);
    for (int i = 0; i < PIN_MAX; i++) {
        wchar_t key[24], buf[48];
        wsprintfW(key, L"%s%d", g_pinKey, i);
        buf[0] = 0;
        if (CfgGetStr(key, buf, 48) && buf[0]) {
            WideToSha(buf, g_pins[g_pinN], 41);
            if (g_pins[g_pinN][0]) g_pinN++;
        }
    }
}

static void PinSave(void)
{
    for (int i = 0; i < PIN_MAX; i++) {
        wchar_t key[24];
        wsprintfW(key, L"%s%d", g_pinKey, i);
        if (i < g_pinN) {
            wchar_t w[48];
            ShaToWide(g_pins[i], w, 48);
            CfgSetStr(key, w);
        } else if (CfgHaveKey(key))
            CfgSetStr(key, L"");
    }
}

BOOL PinHas(const char *sha)
{
    for (int i = 0; i < g_pinN; i++)
        if (!strcmp(g_pins[i], sha)) return TRUE;
    return FALSE;
}

static void PinToggle(const char *sha)
{
    for (int i = 0; i < g_pinN; i++)
        if (!strcmp(g_pins[i], sha)) {
            for (int j = i; j < g_pinN - 1; j++)
                memcpy(g_pins[j], g_pins[j + 1], 41);
            g_pinN--;
            PinSave();
            return;
        }
    if (g_pinN >= PIN_MAX) return;   /* full: ignore */
    lstrcpynA(g_pins[g_pinN], sha, 41);
    g_pinN++;
    PinSave();
}

/* pin toggle button, left of the delete X */
static void HistDrawPinBtn(HDC dc, int rowTop, int rowH, int rightX,
                           BOOL pinned, BOOL hover)
{
    int cx = rightX - SC(44);
    int cy = rowTop + rowH / 2;
    int r = SC(5);
    if (hover) {
        HBRUSH hb = CreateSolidBrush(RGB(232, 240, 255));
        RECT br2 = { cx - r - 4, cy - r - 4, cx + r + 4, cy + r + 4 };
        FillRect(dc, &br2, hb);
        DeleteObject(hb);
    }
    /* pushpin: round head + slanted needle */
    HPEN pen = CreatePen(PS_SOLID, 1,
        pinned ? RGB(0x1A, 0x73, 0xE8)
               : (hover ? RGB(60, 90, 160) : RGB(160, 160, 160)));
    HPEN op = (HPEN)SelectObject(dc, pen);
    HBRUSH br = CreateSolidBrush(
        pinned ? RGB(0x1A, 0x73, 0xE8) : RGB(255, 255, 255));
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    Ellipse(dc, cx - r, cy - r - 1, cx + r, cy + r - 1);
    SelectObject(dc, ob);
    DeleteObject(br);
    MoveToEx(dc, cx, cy + r - 2, NULL);
    LineTo(dc, cx, cy + r + 3);
    SelectObject(dc, op);
    DeleteObject(pen);
}

static BOOL HistPinHit(POINT pt, int rowTop, int rowH, int rightX)
{
    int cx = rightX - SC(44);
    int cy = rowTop + rowH / 2;
    int r = SC(10);
    return pt.x >= cx - r && pt.x <= cx + r
        && pt.y >= cy - r && pt.y <= cy + r;
}

/* commit committer line -> viewer-local "YYYY-MM-DD HH:MM" */
static BOOL CommitTimeLocal(const char *body, int blen, wchar_t *out, int cch)
{
    int tl = 0;
    const char *t = CommitLine(body, blen, "committer ", 10, &tl);
    if (!t || tl <= 0) return FALSE;
    LONGLONG unixSec = 0;
    int sp1 = -1, sp2 = -1;
    for (int k = tl - 1; k >= 0; k--)
        if (t[k] == ' ') {
            if (sp2 < 0) sp2 = k;
            else { sp1 = k; break; }
        }
    if (sp1 < 0 || sp2 <= sp1) return FALSE;
    for (int k = sp1 + 1; k < sp2; k++)
        if (t[k] >= '0' && t[k] <= '9')
            unixSec = unixSec * 10 + (t[k] - '0');
        else
            return FALSE;
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)(unixSec + 11644473600LL) * 10000000ULL;
    FILETIME ft, loc;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    if (!FileTimeToLocalFileTime(&ft, &loc)) return FALSE;
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&loc, &st)) return FALSE;
    wsprintfW(out, L"%04u-%02u-%02u %02u:%02u",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    (void)cch;
    return TRUE;
}

static void HistClose(void)
{
    if (g_histWnd) {
        ReleaseCapture();
        DestroyWindow(g_histWnd);
        g_histWnd = NULL;
        /* wake the modal GetMessageW loop so it notices IsWindow==FALSE
           even when no further input events arrive */
        PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
    }
}

/* rebuild the display list from the walked chain + current pins:
 * [pin header][pinned ascending time][normal descending time] */
static void HistBuildDisplay(HistEntry *disp, int dispCap, int *pCount)
{
    int n = 0;
    if (g_pinN > 0) {
        ZeroMemory(&disp[n], sizeof(HistEntry));
        disp[n].isHdr = TRUE;
        n++;
    }
    /* pinned entries, oldest first */
    for (int i = g_histWalkN - 1; i >= 0 && n < dispCap; i--) {
        if (!g_histWalk[i].pinned) continue;
        disp[n] = g_histWalk[i];
        disp[n].pinned = TRUE;
        disp[n].isHdr = FALSE;
        n++;
    }
    /* normal entries, newest first */
    for (int i = 0; i < g_histWalkN && n < dispCap; i++) {
        if (g_histWalk[i].pinned) continue;
        disp[n] = g_histWalk[i];
        disp[n].pinned = FALSE;
        disp[n].isHdr = FALSE;
        n++;
    }
    *pCount = n;
}

/* close (X) glyph geometry: centered at (rightX - SC(22), rowTop + rowH/2) */
static void HistDrawX(HDC dc, int rowTop, int rowH, int rightX, BOOL hover)
{
    int cx = rightX - SC(22);
    int cy = rowTop + rowH / 2;
    int r = SC(6);
    if (hover) {
        HBRUSH hb = CreateSolidBrush(RGB(252, 230, 230));
        RECT br2 = { cx - r - 3, cy - r - 3, cx + r + 3, cy + r + 3 };
        FillRect(dc, &br2, hb);
        DeleteObject(hb);
    }
    HPEN pen = CreatePen(PS_SOLID, 1,
                         hover ? RGB(200, 40, 40) : RGB(160, 160, 160));
    HPEN op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, cx - r, cy - r, NULL);
    LineTo(dc, cx + r, cy + r);
    MoveToEx(dc, cx - r, cy + r, NULL);
    LineTo(dc, cx + r, cy - r);
    SelectObject(dc, op);
    DeleteObject(pen);
}

static BOOL HistXHit(POINT pt, int rowTop, int rowH, int rightX)
{
    int cx = rightX - SC(22);
    int cy = rowTop + rowH / 2;
    int r = SC(9);
    return pt.x >= cx - r && pt.x <= cx + r
        && pt.y >= cy - r && pt.y <= cy + r;
}

static LRESULT CALLBACK HistWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &rc, br);
        DeleteObject(br);
        HBRUSH bdr = CreateSolidBrush(COL_BORDER);
        FrameRect(dc, &rc, bdr);
        DeleteObject(bdr);
        rc.right--;
        rc.bottom--;

        int rowH = HistRowH();
        int vis = (rc.bottom + 1) / rowH;
        HFONT old = (HFONT)SelectObject(dc, g_fontHeader);
        SetBkMode(dc, TRANSPARENT);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        for (int i = 0; i < vis; i++) {
            int idx = g_histScroll + i;
            if (idx >= g_histCount) break;
            RECT rr = { 1, i * rowH, rc.right + 1, (i + 1) * rowH };
            const HistEntry *he = &g_histList[idx];
            if (he->isHdr) {
                static const wchar_t ptitle[] = L"— 已钉住 —";
                HFONT os = (HFONT)SelectObject(dc, g_fontStatus);
                SetTextColor(dc, RGB(0x8A, 0x8A, 0x8E));
                int wl = lstrlenW(ptitle);
                TextOutW(dc, (rc.right - text_w(dc, g_fontStatus, ptitle,
                         wl)) / 2, rr.top + (rowH - 14) / 2, ptitle, wl);
                SelectObject(dc, os);
                continue;
            }
            if (idx == g_histHover) {
                HBRUSH hb = CreateSolidBrush(RGB(232, 240, 255));
                FillRect(dc, &rr, hb);
                DeleteObject(hb);
            }
            int ty = rr.top + (rowH - tm.tmHeight) / 2;
            SetTextColor(dc, COL_BTNTXT);
            int tx = he->pinned ? SC(18) : SC(14);
            TextOutW(dc, tx, ty, he->time, lstrlenW(he->time));
            if (he->isAuto) {
                static const wchar_t tag[] = L"自动";
                int wl = lstrlenW(tag);
                SelectObject(dc, g_fontStatus);
                SetTextColor(dc, COL_STATTXT);
                TextOutW(dc, rc.right - SC(40) - text_w(dc, g_fontStatus,
                         tag, wl), ty + 1, tag, wl);
                SelectObject(dc, g_fontHeader);
            }
            HistDrawPinBtn(dc, rr.top, rowH, rc.right, he->pinned,
                           idx == g_histPinHover);
            HistDrawX(dc, rr.top, rowH, rc.right, idx == g_histDelHover);
        }
        /* slim scrollbar when needed */
        if (g_histCount > vis) {
            int railX = rc.right - SC(6);
            HBRUSH rail = CreateSolidBrush(RGB(238, 238, 242));
            RECT rrc = { railX, 0, rc.right + 1, rc.bottom + 1 };
            FillRect(dc, &rrc, rail);
            DeleteObject(rail);
            int trackH = rc.bottom + 1;
            int thumbH = trackH * vis / g_histCount;
            if (thumbH < SC(18)) thumbH = SC(18);
            int maxScroll = g_histCount - vis;
            int thumbY = (trackH - thumbH) * g_histScroll / maxScroll;
            HBRUSH tb = CreateSolidBrush(RGB(201, 201, 206));
            RECT trc = { railX, thumbY, rc.right + 1, thumbY + thumbH };
            FillRect(dc, &trc, tb);
            DeleteObject(tb);
        }
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT rc;
        GetClientRect(h, &rc);
        if (g_histDragBar) {
            int trackH = rc.bottom - SC(0);
            int vis = rc.bottom / HistRowH();
            int thumbH = trackH * vis / g_histCount;
            if (thumbH < SC(18)) thumbH = SC(18);
            int maxScroll = g_histCount - vis;
            int dy = pt.y - g_histDragY0;
            int ns = g_histScroll0 + dy * maxScroll
                     / (trackH - thumbH > 1 ? trackH - thumbH : 1);
            if (ns < 0) ns = 0;
            if (ns > maxScroll) ns = maxScroll;
            if (ns != g_histScroll) {
                g_histScroll = ns;
                InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        }
        int rowH = HistRowH();
        int hover = (pt.x >= 0 && pt.x < rc.right && pt.y >= 0)
                    ? pt.y / rowH + g_histScroll : -1;
        if (hover >= g_histCount) hover = -1;
        if (hover != g_histHover) {
            g_histHover = hover;
            InvalidateRect(h, NULL, FALSE);
        }
        int delHover = -1, pinHover = -1;
        if (hover >= 0 && !g_histList[hover].isHdr) {
            int rowTop = (hover - g_histScroll) * rowH;
            if (HistXHit(pt, rowTop, rowH, rc.right)) delHover = hover;
            else if (HistPinHit(pt, rowTop, rowH, rc.right))
                pinHover = hover;
        }
        if (delHover != g_histDelHover) {
            g_histDelHover = delHover;
            InvalidateRect(h, NULL, FALSE);
        }
        if (pinHover != g_histPinHover) {
            g_histPinHover = pinHover;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int vis = 10;
        RECT rc;
        GetClientRect(h, &rc);
        vis = rc.bottom / HistRowH();
        int maxScroll = g_histCount - vis;
        if (maxScroll <= 0) return 0;
        int d = -(short)HIWORD(wp) / WHEEL_DELTA;
        int ns = g_histScroll + d;
        if (d != 0 && ns == g_histScroll && d < 0) ns = g_histScroll - 1;
        if (d != 0 && ns == g_histScroll && d > 0) ns = g_histScroll + 1;
        if (ns < 0) ns = 0;
        if (ns > maxScroll) ns = maxScroll;
        if (ns != g_histScroll) {
            g_histScroll = ns;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT rc;
        GetClientRect(h, &rc);
        if (!PtInRect(&rc, pt)) { HistClose(); return 0; }
        int vis = rc.bottom / HistRowH();
        if (g_histCount > vis && pt.x >= rc.right - SC(10)) {
            /* scrollbar zone: page or thumb drag */
            int trackH = rc.bottom;
            int thumbH = trackH * vis / g_histCount;
            if (thumbH < SC(18)) thumbH = SC(18);
            int maxScroll = g_histCount - vis;
            int thumbY = (trackH - thumbH) * g_histScroll / maxScroll;
            if (pt.y >= thumbY && pt.y < thumbY + thumbH) {
                g_histDragBar = TRUE;
                g_histDragY0 = pt.y;
                g_histScroll0 = g_histScroll;
                SetCapture(h);
            } else {
                int page = (pt.y < thumbY ? -1 : 1) * vis;
                int ns = g_histScroll + page;
                if (ns < 0) ns = 0;
                if (ns > maxScroll) ns = maxScroll;
                if (ns != g_histScroll) {
                    g_histScroll = ns;
                    InvalidateRect(h, NULL, FALSE);
                }
            }
            return 0;
        }
        int row = pt.y / HistRowH();
        int idx = row + g_histScroll;
        if (idx >= 0 && idx < g_histCount) {
            int rowTop = row * HistRowH();
            if (g_histList[idx].isHdr) return 0;
            if (HistXHit(pt, rowTop, HistRowH(), rc.right)) {
                g_histDelPick = idx;
                HistClose();
                return 0;
            }
            if (HistPinHit(pt, rowTop, HistRowH(), rc.right)) {
                /* toggle pin and rebuild the display in place */
                PinToggle(g_histList[idx].sha);
                for (int k = 0; k < g_histWalkN; k++)
                    g_histWalk[k].pinned = PinHas(g_histWalk[k].sha);
                HistBuildDisplay(g_histList, 1 + PIN_MAX
                                 + (g_histMax < 1 ? 1 : g_histMax),
                                 &g_histCount);
                g_histScroll = 0;
                g_histHover = -1;
                g_histPick = -1;
                g_histDelHover = -1;
                g_histPinHover = -1;
                RECT rcw;
                GetClientRect(h, &rcw);
                InvalidateRect(h, NULL, FALSE);
                (void)rcw;
                return 0;
            }
            g_histPick = idx;
            HistClose();
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_histDragBar) {
            g_histDragBar = FALSE;
            ReleaseCapture();
            SetCapture(h); /* keep the outside-click close behavior */
        }
        return 0;
    case WM_RBUTTONDOWN:
        HistClose();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { HistClose(); return 0; }
        return 0;
    case WM_CAPTURECHANGED:
        HistClose();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* editor content byte-equal with the file on disk? */
static BOOL EditorEqualsFile(const wchar_t *path)
{
    HANDLE fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return FALSE;
    DWORD size = GetFileSize(fh, NULL);
    BOOL eq = FALSE;
    if (size != INVALID_FILE_SIZE && size < 32u * 1024 * 1024) {
        char *u8 = (char *)malloc(size + 1);
        DWORD rd = 0;
        if (u8 && ReadFile(fh, u8, size, &rd, NULL)) {
            int bom = (rd >= 3 && (BYTE)u8[0] == 0xEF
                       && (BYTE)u8[1] == 0xBB && (BYTE)u8[2] == 0xBF);
            int wl = MultiByteToWideChar(CP_UTF8, 0, u8 + (bom ? 3 : 0),
                                         rd - (bom ? 3 : 0), NULL, 0);
            wchar_t *w = (wchar_t *)malloc((wl + 1) * sizeof(wchar_t));
            if (w) {
                MultiByteToWideChar(CP_UTF8, 0, u8 + (bom ? 3 : 0),
                                    rd - (bom ? 3 : 0), w, wl);
                int el = GetWindowTextLengthW(g_edit);
                eq = (el == wl);
                if (eq && el > 0) {
                    wchar_t *eb = (wchar_t *)malloc((el + 1) * sizeof(wchar_t));
                    if (eb) {
                        GetWindowTextW(g_edit, eb, el + 1);
                        eq = (memcmp(eb, w, el * sizeof(wchar_t)) == 0);
                        free(eb);
                    }
                }
                free(w);
            }
        }
        free(u8);
    }
    CloseHandle(fh);
    return eq;
}

static void RestoreSnapshot(const wchar_t *repo, const HistEntry *e,
                            const wchar_t *mainPath)
{
    char *cb = NULL;
    int cbl = 0;
    if (!ReadLoose(repo, e->sha, &cb, &cbl)) {
        MessageBoxW(g_hwnd, L"无法打开历史版本。", APP_NAME, MB_ICONERROR);
        return;
    }
    int tl = 0;
    const char *t = CommitLine(cb, cbl, "tree ", 5, &tl);
    BOOL loaded = FALSE;
    char treeHex[41] = "";
    if (t && tl == 40) {
        memcpy(treeHex, t, 40);
        treeHex[40] = 0;
        char *tb = NULL;
        int tbl = 0;
        if (ReadLoose(repo, treeHex, &tb, &tbl)) {
            for (int i = 0; i + 20 <= tbl; i++)
                if (tb[i] == 0) {
                    static const char HC[] = "0123456789abcdef";
                    char blobHex[41];
                    for (int k = 0; k < 20; k++) {
                        blobHex[k*2]   = HC[(BYTE)tb[i+1+k] >> 4];
                        blobHex[k*2+1] = HC[(BYTE)tb[i+1+k] & 15];
                    }
                    blobHex[40] = 0;
                    char *bb = NULL;
                    int bbl = 0;
                    if (ReadLoose(repo, blobHex, &bb, &bbl)) {
                        int wl = MultiByteToWideChar(CP_UTF8, 0, bb, bbl,
                                                     NULL, 0);
                        wchar_t *w = (wchar_t *)malloc(
                            (wl + 1) * sizeof(wchar_t));
                        if (w) {
                            MultiByteToWideChar(CP_UTF8, 0, bb, bbl, w, wl);
                            w[wl] = 0;
                            if (g_view != VIEW_EDIT) SetView(VIEW_EDIT);
                            g_loading = TRUE;
                            SetWindowTextW(g_edit, w);
                            g_loading = FALSE;
                            SendMessageW(g_edit, EM_SETSEL, 0, 0);
                            SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
                            free(w);
                            loaded = TRUE;
                        }
                        free(bb);
                    }
                    break;
                }
            free(tb);
        }
    }
    free(cb);
    if (loaded) {
        SetPath(mainPath); /* keep the main file path */
        /* restored content is already archived at that commit; a later
         * autosave only records when the user edits something */
        lstrcpynA(g_archivedTree, treeHex, 41);
        g_dirty = !EditorEqualsFile(mainPath);
        UpdateTitle();
        InvalidateRect(g_hwnd, NULL, FALSE);
    } else {
        MessageBoxW(g_hwnd, L"无法打开历史版本。", APP_NAME, MB_ICONERROR);
    }
}

static BOOL HistDeleteAt(const wchar_t *repo, HistEntry *list,
                          int *pCount, int i)
{
    int count = *pCount;
    if (i < 0 || i >= count) return FALSE;
    char newTip[41] = "";
    if (count == 1) {
        /* deleting the only commit: drop the master ref */
        wchar_t path[MAX_PATH + 64];
        RepoRefPath(repo, path, MAX_PATH + 64);
        DeleteFileW(path);
        *pCount = 0;
        return TRUE;
    }
    if (i == 0) {
        lstrcpynA(newTip, list[1].sha, 41);
    } else {
        char parentHex[41] = "";
        if (i + 1 < count) lstrcpynA(parentHex, list[i + 1].sha, 41);
        for (int k = i - 1; k >= 0; k--) {
            char newHex[41];
            if (!CloneCommit(repo, list[k].sha,
                             parentHex[0] ? parentHex : NULL, newHex))
                return FALSE;
            lstrcpynA(parentHex, newHex, 41);
            if (k == 0) lstrcpynA(newTip, newHex, 41);
        }
    }
    if (!newTip[0] || !WriteMaster(repo, newTip)) return FALSE;
    /* rebuild the in-memory list by walking the new chain */
    int nc = 0;
    char cur[41];
    lstrcpynA(cur, newTip, 41);
    while (nc < count) {
        char *b = NULL; int bl = 0;
        if (!ReadLoose(repo, cur, &b, &bl)) break;
        lstrcpynA(list[nc].sha, cur, 41);
        lstrcpynW(list[nc].time, L"----", 20);
        CommitTimeLocal(b, bl, list[nc].time, 20);
        list[nc].isAuto = CommitLine(b, bl, "自动保存 ", 13, NULL) != NULL;
        nc++;
        int pl = 0;
        const char *p = CommitLine(b, bl, "parent ", 7, &pl);
        BOOL has = (p && pl == 40);
        if (has) { memcpy(cur, p, 40); cur[40] = 0; }
        free(b);
        if (!has) break;
    }
    *pCount = nc;
    return TRUE;
}

static void ShowHistoryMenu(void)
{
    if (!g_path[0]) {
        MessageBoxW(g_hwnd, L"请先保存文件，历史版本会自动存档。", APP_NAME,
                    MB_ICONINFORMATION);
        return;
    }
    wchar_t repo[MAX_PATH];
    if (!BuildRepoDir(repo, MAX_PATH)) return;

    char tipHex[41];
    if (!ReadMaster(repo, tipHex)) {
        MessageBoxW(g_hwnd, L"暂无历史版本。", APP_NAME, MB_ICONINFORMATION);
        return;
    }

    /* walk the chain: newest first; collect the normal cap plus any
     * pinned entries found along the way (pins survive the cap) */
    PinLoad(g_path);
    int cap = g_histMax < 1 ? 1 : g_histMax;
    int hard = cap + PIN_MAX * 2 + 50;
    HistEntry *walk = (HistEntry *)malloc(hard * sizeof(HistEntry));
    if (!walk) return;
    int wcount = 0, normalN = 0, pinSeen = 0;
    char cur[41];
    lstrcpynA(cur, tipHex, 41);
    while (wcount < hard) {
        char *body = NULL;
        int blen = 0;
        if (!ReadLoose(repo, cur, &body, &blen)) break;
        HistEntry *e = &walk[wcount];
        lstrcpynA(e->sha, cur, 41);
        lstrcpynW(e->time, L"----", 20);
        CommitTimeLocal(body, blen, e->time, 20);
        e->isAuto = CommitLine(body, blen, "自动保存 ", 13, NULL) != NULL;
        e->pinned = PinHas(e->sha);
        e->isHdr = FALSE;
        if (e->pinned) pinSeen++; else normalN++;
        wcount++;
        int pl = 0;
        const char *p = CommitLine(body, blen, "parent ", 7, &pl);
        BOOL has = (p && pl == 40);
        if (has) {
            memcpy(cur, p, 40);
            cur[40] = 0;
        }
        free(body);
        if (!has) break;
        if (normalN >= cap && pinSeen >= g_pinN) break;
    }
    if (wcount == 0) {
        MessageBoxW(g_hwnd, L"暂无历史版本。", APP_NAME, MB_ICONINFORMATION);
        free(walk);
        return;
    }
    /* prune stale pins that no longer resolve to any commit */
    if (pinSeen < g_pinN) {
        for (int i = g_pinN - 1; i >= 0; i--) {
            BOOL found = FALSE;
            for (int k = 0; k < wcount; k++)
                if (!strcmp(walk[k].sha, g_pins[i])) { found = TRUE; break; }
            if (!found) {
                for (int j = i; j < g_pinN - 1; j++)
                    memcpy(g_pins[j], g_pins[j + 1], 41);
                g_pinN--;
            }
        }
        PinSave();
    }

    int dispCap = 1 + PIN_MAX + cap;
    HistEntry *list = (HistEntry *)malloc(dispCap * sizeof(HistEntry));
    if (!list) { free(walk); return; }
    g_histWalk = walk;
    g_histWalkN = wcount;
    int count = 0;
    HistBuildDisplay(list, dispCap, &count);

    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = HistWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"MDLiteHist";
        if (!RegisterClassW(&wc)) { free(list); return; }
        registered = TRUE;
    }

    wchar_t mainPath[MAX_PATH];
    lstrcpynW(mainPath, g_path, MAX_PATH);

    int rows = count < HIST_VIS ? count : HIST_VIS;
    int rowH = HistRowH();
    RECT br = BtnRect(2);
    POINT pt = { br.left, br.bottom + SC(4) };
    ClientToScreen(g_hwnd, &pt);

    g_histList = list;
    g_histCount = count;
    g_histScroll = 0;
    g_histHover = -1;
    g_histPick = -1;
    g_histDelPick = -1;
    g_histDelHover = -1;
    g_histPinHover = -1;
    g_histDragBar = FALSE;
    lstrcpynW(g_histRepo, repo, MAX_PATH);

    g_histWnd = CreateWindowExW(0, L"MDLiteHist", NULL, WS_POPUP | WS_VISIBLE,
                                pt.x, pt.y, SC(264), rows * rowH + 2,
                                g_hwnd, NULL, NULL, NULL);
    if (g_histWnd) {
        SetFocus(g_histWnd);
        SetCapture(g_histWnd);
        MSG msg;
        while (IsWindow(g_histWnd) && GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetCapture() == g_histWnd) ReleaseCapture();
        SetFocus(g_edit);
    }
    g_histWnd = NULL;
    g_histRepo[0] = 0;

    /* map display picks back onto the chain-ordered walk array */
    int walkDel = -1, walkPick = -1;
    for (int i = 0; i < count; i++) {
        if (i == g_histDelPick || i == g_histPick)
            for (int k = 0; k < g_histWalkN; k++)
                if (!strcmp(list[i].sha, g_histWalk[k].sha)) {
                    if (i == g_histDelPick) walkDel = k;
                    if (i == g_histPick) walkPick = k;
                }
    }

    BOOL reopen = FALSE;
    if (walkDel >= 0 && walkDel < g_histWalkN) {
        wchar_t msg2[128];
        wsprintfW(msg2, L"删除该历史版本？\n%s",
                  g_histWalk[walkDel].time);
        if (MessageBoxW(g_hwnd, msg2, APP_NAME,
                        MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
            if (g_histWalk[walkDel].pinned)
                PinToggle(g_histWalk[walkDel].sha);
            if (HistDeleteAt(repo, g_histWalk, &g_histWalkN, walkDel))
                reopen = (g_histWalkN > 0);
        }
        g_histDelPick = -1;
    } else if (walkPick >= 0 && walkPick < g_histWalkN) {
        RestoreSnapshot(repo, &g_histWalk[walkPick], mainPath);
    }
    free(list);
    free(g_histWalk);
    g_histWalk = NULL;
    g_histWalkN = 0;
    g_histList = NULL;
    if (reopen) ShowHistoryMenu();
}



static void DrawRoundRect(HDC dc, RECT *rc, int rad, COLORREF fill,
                          COLORREF border, int borderW)
{
    HPEN pen = CreatePen(PS_SOLID | PS_INSIDEFRAME, borderW, border);
    HBRUSH br = CreateSolidBrush(fill);
    HPEN op = (HPEN)SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    RoundRect(dc, rc->left, rc->top, rc->right, rc->bottom, rad, rad);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(br);
}

static void DrawHeader(HDC dc, RECT *rcClient)
{
    RECT rc = { rcClient->left, rcClient->top, rcClient->right,
                rcClient->top + HeaderH() };
    HBRUSH br = CreateSolidBrush(COL_HEADERBG);
    FillRect(dc, &rc, br);
    DeleteObject(br);

    /* buttons - pill shape */
    static const wchar_t *labels[3] =
        { L"\x25B6 打开", L"\x25A0 保存", L"\x22EF 更多" };
    for (int i = 0; i < 3; i++) {
        RECT r = BtnRect(i);
        int hot = (g_hoverId == i);
        DrawRoundRect(dc, &r, SC(14), hot ? COL_BTNHVR : RGB(255,255,255),
                      COL_BTNBRD, 1);
        HFONT old = (HFONT)SelectObject(dc, g_fontHeader);
        SetTextColor(dc, COL_BTNTXT);
        SetBkMode(dc, TRANSPARENT);
        int tx = r.left + (r.right - r.left) / 2
                 - text_w(dc, g_fontHeader, labels[i], lstrlenW(labels[i])) / 2;
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        int ty = r.top + (r.bottom - r.top - tm.tmHeight) / 2;
        TextOutW(dc, tx, ty, labels[i], lstrlenW(labels[i]));
        SelectObject(dc, old);
    }

    /* topmost toggle - accent-styled while active */
    {
        static const wchar_t *pin = L"置顶";
        RECT r = BtnRect(3);
        int hot = (g_hoverId == 3);
        int on = g_topmost;
        DrawRoundRect(dc, &r, SC(14),
                      on ? RGB(0xE3, 0xEE, 0xFF)
                         : (hot ? COL_BTNHVR : RGB(255, 255, 255)),
                      on ? COL_ACCENT : COL_BTNBRD, 1);
        HFONT old = (HFONT)SelectObject(dc, g_fontHeader);
        SetTextColor(dc, on ? COL_ACCENT : COL_BTNTXT);
        SetBkMode(dc, TRANSPARENT);
        int tx = r.left + (r.right - r.left) / 2
                 - text_w(dc, g_fontHeader, pin, lstrlenW(pin)) / 2;
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        int ty = r.top + (r.bottom - r.top - tm.tmHeight) / 2;
        TextOutW(dc, tx, ty, pin, lstrlenW(pin));
        SelectObject(dc, old);
    }

    /* segment switch - capsule, three segments */
    {
        RECT s = SegRect();
        DrawRoundRect(dc, &s, SC(14), COL_SEG_BG, COL_SEG_BG, 1);
        int third = (s.right - s.left) / 3;
        static const wchar_t *segLabels[3] = { L"编辑", L"分栏", L"预览" };
        HFONT old = (HFONT)SelectObject(dc, g_fontHeader);
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i < 3; i++) {
            RECT r = { s.left + i * third, s.top + SC(2),
                       s.left + (i + 1) * third, s.bottom - SC(2) };
            if (i == g_view) {
                RECT pad = { r.left + SC(2), r.top,
                             r.right - SC(2), r.bottom };
                DrawRoundRect(dc, &pad, SC(11), RGB(255,255,255),
                              COL_BORDER, 1);
            }
            int wl = lstrlenW(segLabels[i]);
            int w = text_w(dc, g_fontHeader, segLabels[i], wl);
            SetTextColor(dc, i == g_view ? COL_ACCENT : RGB(0x6E,0x6E,0x73));
            TEXTMETRICW tm;
            GetTextMetricsW(dc, &tm);
            int tx = r.left + (r.right - r.left) / 2 - w / 2;
            int ty = r.top + (r.bottom - r.top - tm.tmHeight) / 2;
            TextOutW(dc, tx, ty, segLabels[i], wl);
        }
        SelectObject(dc, old);
    }

    /* bottom border */
    HPEN pen = CreatePen(PS_SOLID, 1, COL_BORDER);
    HPEN op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, rc.left, rc.bottom - 1, NULL);
    LineTo(dc, rc.right, rc.bottom - 1);
    SelectObject(dc, op);
    DeleteObject(pen);
}

/* CJK chars count as one word each; runs of latin/digits count as one */
static int CountWords(const wchar_t *s, int len)
{
    int words = 0, inWord = 0;
    for (int i = 0; i < len; i++) {
        wchar_t c = s[i];
        /* merge UTF-16 surrogate pairs into a single character */
        if ((c & 0xFC00) == 0xD800 && i + 1 < len
            && (s[i + 1] & 0xFC00) == 0xDC00) {
            i++;
            if (!inWord) { words++; inWord = 1; }
            continue;
        }
        BOOL cjk = (c >= 0x2E80 && c <= 0x9FFF)
                   || (c >= 0xF900 && c <= 0xFAFF)
                   || (c >= 0xFF00 && c <= 0xFFEF)
                   || (c >= 0x3000 && c <= 0x303F);
        if (cjk) { words++; inWord = 0; continue; }
        if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r') {
            inWord = 0; continue;
        }
        if (!inWord) { words++; inWord = 1; }
    }
    return words;
}

/* refresh cached line/col/words; invalidate status bar on change.
 * called from a 500ms UI timer (cheap: word count only when length
 * changed) */
/* Poll the file once a second; reload silently when clean, hint when dirty. */
static void ExtCheck(void)
{
    if (!g_extValid || !g_path[0]) return;
    if (g_aiBusy || g_agBusy) return;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(g_path, GetFileExInfoStandard, &fad)) return;
    if (fad.nFileSizeLow == g_extSize
        && fad.ftLastWriteTime.dwLowDateTime == g_extTime.dwLowDateTime
        && fad.ftLastWriteTime.dwHighDateTime == g_extTime.dwHighDateTime)
        return;

    if (!g_dirty) {
        int first = (int)SendMessageW(g_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        HCURSOR oc = SetCursor(LoadCursorW(NULL, IDC_WAIT));
        LoadFile(g_path);               /* resets the baseline via ExtMark */
        SetCursor(oc);
        SendMessageW(g_edit, EM_LINESCROLL, 0, first);
        RefreshPreviewNow();
    } else {
        g_extNoticed = TRUE;            /* keep saving wins; just hint */
        g_extTime = fad.ftLastWriteTime;
        g_extSize = fad.nFileSizeLow;
        RECT rc, rcC;
        GetClientRect(g_hwnd, &rcC);
        rc = rcC;
        rc.top = rcC.bottom - StatusH();
        InvalidateRect(g_hwnd, &rc, FALSE);
    }
}

static void StatusTick(void)
{
    static int s_half;
    if (!(++s_half & 1)) ExtCheck();    /* every other 500 ms tick */

    DWORD selStart = 0, selEnd = 0;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
    int line = (int)SendMessageW(g_edit, EM_LINEFROMCHAR,
                                 (WPARAM)selEnd, 0);
    int lineStart = (int)SendMessageW(g_edit, EM_LINEINDEX,
                                      (WPARAM)line, 0);
    int col = (int)selEnd - lineStart + 1;
    if (col < 1) col = 1;

    int len = GetWindowTextLengthW(g_edit);
    if (len != g_stLen) {
        g_stLen = len;
        wchar_t *buf = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
        if (buf) {
            GetWindowTextW(g_edit, buf, len + 1);
            g_stWords = CountWords(buf, len);
            free(buf);
        }
    }

    BOOL busy = g_aiBusy || g_agBusy;
    int busySec = -1;
    if (busy) {
        DWORD t = GetTickCount() - (g_aiBusy ? g_aiStartTick : g_agStartTick);
        busySec = (int)(t / 1000);
    }

    if (line != g_stLine || col != g_stCol || busySec != g_stBusySec) {
        g_stLine = line;
        g_stCol = col;
        g_stBusySec = busySec;
        RECT rc, rcC;
        GetClientRect(g_hwnd, &rcC);
        rc = rcC;
        rc.top = rcC.bottom - StatusH();
        InvalidateRect(g_hwnd, &rc, FALSE);
    }
}

static void DrawStatus(HDC dc, RECT *rcClient)
{
    RECT rc = { rcClient->left, rcClient->bottom - StatusH(),
                rcClient->right, rcClient->bottom };
    HBRUSH br = CreateSolidBrush(COL_HEADERBG);
    FillRect(dc, &rc, br);
    DeleteObject(br);

    HPEN pen = CreatePen(PS_SOLID, 1, COL_BORDER);
    HPEN op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, rc.left, rc.top, NULL);
    LineTo(dc, rc.right, rc.top);
    SelectObject(dc, op);
    DeleteObject(pen);

    HFONT old = (HFONT)SelectObject(dc, g_fontStatus);
    SetBkMode(dc, TRANSPARENT);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    int ty = rc.top + (StatusH() - tm.tmHeight) / 2;

    /* left: line/col/words/read-time + zoom */
    int line = g_stLine >= 0 ? g_stLine + 1 : 1;
    int col = g_stCol >= 0 ? g_stCol : 1;
    int words = g_stWords >= 0 ? g_stWords : 0;
    int minutes = (words + 299) / 300;   /* ~300 words/min */
    wchar_t left[96];
    wchar_t read[24];
    if (words > 0) {
        if (minutes < 1) lstrcpynW(read, L"<1 分钟", 24);
        else wsprintfW(read, L"%d 分钟", minutes);
        if (ZOOMS[g_zoom] != 100)
            wsprintfW(left, L"行 %d:%d   字数 %d   阅读 %s   %d%%",
                      line, col, words, read, ZOOMS[g_zoom]);
        else
            wsprintfW(left, L"行 %d:%d   字数 %d   阅读 %s",
                      line, col, words, read);
    } else {
        if (ZOOMS[g_zoom] != 100)
            wsprintfW(left, L"行 %d:%d   %d%%", line, col, ZOOMS[g_zoom]);
        else
            wsprintfW(left, L"行 %d:%d", line, col);
    }
    SetTextColor(dc, COL_STATTXT);
    TextOutW(dc, SC(14), ty, left, lstrlenW(left));

    /* right: busy timer first, else encoding + save state */
    wchar_t right[80];
    if (g_aiBusy || g_agBusy) {
        DWORD t = GetTickCount() - (g_aiBusy ? g_aiStartTick : g_agStartTick);
        int sec = (int)(t / 1000);
        wsprintfW(right, L"%s 执行中 %d:%02d · Esc 取消",
                  g_aiBusy ? L"AI" : L"Agent", sec / 60, sec % 60);
    } else if (g_extNoticed) {
        wsprintfW(right, L"文件已被外部修改%s   UTF-8",
                  g_dirty ? L" · 未保存" : L"");
    } else {
        wsprintfW(right, L"%s   UTF-8", g_dirty ? L"未保存" : L"已保存");
    }
    int rw = text_w(dc, g_fontStatus, right, lstrlenW(right));
    SetTextColor(dc, (g_aiBusy || g_agBusy || g_dirty || g_extNoticed)
                        ? COL_ACCENT : COL_STATTXT);
    TextOutW(dc, rc.right - SC(14) - rw, ty, right, lstrlenW(right));

    SelectObject(dc, old);
}

/* text_w is defined in markdown.c */

/* ------------------------------------------------------------------ */
/* find bar                                                             */
/* ------------------------------------------------------------------ */

void HideFindBar(void);
static LRESULT CALLBACK FindProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

static wchar_t lowerW(wchar_t c)
{
    if (c >= L'A' && c <= L'Z') return c + (L'a' - L'A');
    return c;
}

static BOOL match_at(const wchar_t *b, const wchar_t *q, int ql)
{
    for (int i = 0; i < ql; i++)
        if (lowerW(b[i]) != lowerW(q[i])) return FALSE;
    return TRUE;
}

static void DoFind(int dir)
{
    int ql = lstrlenW(g_findQuery);
    if (ql == 0) {
        lstrcpynW(g_findInfo, L"输入查找内容", 64);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    int len = GetWindowTextLengthW(g_edit);
    wchar_t *buf = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
    if (!buf) return;
    GetWindowTextW(g_edit, buf, len + 1);

    DWORD selStart = 0, selEnd = 0;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);

    int pos = -1;
    if (dir > 0) {
        for (int i = (int)selEnd; i + ql <= len; i++)
            if (match_at(buf + i, g_findQuery, ql)) { pos = i; break; }
        if (pos < 0) /* wrap */
            for (int i = 0; i + ql <= len; i++)
                if (match_at(buf + i, g_findQuery, ql)) { pos = i; break; }
    } else {
        for (int i = (int)selStart - ql; i >= 0; i--)
            if (match_at(buf + i, g_findQuery, ql)) { pos = i; break; }
        if (pos < 0) /* wrap */
            for (int i = len - ql; i >= 0; i--)
                if (match_at(buf + i, g_findQuery, ql)) { pos = i; break; }
    }

    int total = 0, idx = 0;
    for (int i = 0; i + ql <= len; i++)
        if (match_at(buf + i, g_findQuery, ql)) {
            total++;
            if (pos >= 0 && i < pos) idx++;
        }
    free(buf);

    if (pos < 0 || total == 0) {
        lstrcpynW(g_findInfo, L"无匹配", 64);
    } else {
        SendMessageW(g_edit, EM_SETSEL, pos, pos + ql);
        SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
        wsprintfW(g_findInfo, L"%d/%d", idx + 1, total);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* replace the current match, then jump to the next one */
static void DoReplaceOne(void)
{
    int ql = lstrlenW(g_findQuery);
    if (ql == 0) { DoFind(1); return; }

    DWORD s0 = 0, e0 = 0;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
    wchar_t sel[144];
    BOOL isMatch = FALSE;
    if ((int)(e0 - s0) == ql && ql < 140) {
        ((LPWORD)sel)[0] = (WORD)(ql + 1);
        SendMessageW(g_edit, EM_GETSELTEXT, 0, (LPARAM)sel);
        isMatch = match_at(sel, g_findQuery, ql);
    }
    if (!isMatch) { DoFind(1); return; }

    wchar_t rep[136];
    GetWindowTextW(g_replEdit, rep, 136);
    SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)rep);
    DoFind(1);
}

/* replace every occurrence (case-insensitive, non-overlapping) */
static void DoReplaceAll(void)
{
    int ql = lstrlenW(g_findQuery);
    if (ql == 0) { DoFind(1); return; }
    wchar_t rep[136];
    GetWindowTextW(g_replEdit, rep, 136);
    int rl = lstrlenW(rep);

    int len = GetWindowTextLengthW(g_edit);
    wchar_t *buf = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
    if (!buf) return;
    GetWindowTextW(g_edit, buf, len + 1);

    int count = 0;
    for (int i = 0; i + ql <= len; i++)
        if (match_at(buf + i, g_findQuery, ql)) { count++; i += ql - 1; }
    if (count == 0) {
        lstrcpynW(g_findInfo, L"无匹配", 64);
        free(buf);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }

    long long nll = (long long)len + (long long)count * (rl - ql);
    if (nll < 0) nll = 0;
    wchar_t *out = (wchar_t *)malloc(((size_t)nll + 1) * sizeof(wchar_t));
    if (!out) { free(buf); return; }
    int o = 0;
    for (int i = 0; i < len; ) {
        if (i + ql <= len && match_at(buf + i, g_findQuery, ql)) {
            memcpy(out + o, rep, (size_t)rl * sizeof(wchar_t));
            o += rl; i += ql;
        } else {
            out[o++] = buf[i++];
        }
    }
    out[o] = 0;
    SendMessageW(g_edit, EM_SETSEL, 0, len);
    SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)out);
    SendMessageW(g_edit, EM_SETSEL, 0, 0);
    wsprintfW(g_findInfo, L"已替换 %d 处", count);
    free(buf);
    free(out);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void ShowFindBar(int toReplace)
{
    if (g_view == VIEW_PREVIEW) SetView(VIEW_SPLIT); /* keep editing */
    if (!g_findEdit) {
        g_findEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_findQuery,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, g_hwnd, (HMENU)2, NULL, NULL);
        g_findProc = (WNDPROC)SetWindowLongPtrW(g_findEdit, GWLP_WNDPROC,
                                                (LONG_PTR)FindProc);
        SendMessageW(g_findEdit, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
        g_replEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_replQuery,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, g_hwnd, (HMENU)3, NULL, NULL);
        g_replProc = (WNDPROC)SetWindowLongPtrW(g_replEdit, GWLP_WNDPROC,
                                                (LONG_PTR)FindProc);
        SendMessageW(g_replEdit, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
    }
    g_findShown = TRUE;
    ShowWindow(g_findEdit, SW_SHOW);
    ShowWindow(g_replEdit, SW_SHOW);
    LayoutChildren();
    InvalidateRect(g_hwnd, NULL, TRUE);
    SetFocus(toReplace ? g_replEdit : g_findEdit);
    SendMessageW(toReplace ? g_replEdit : g_findEdit, EM_SETSEL, 0, -1);
}

void HideFindBar(void)
{
    if (!g_findShown) return;
    g_findShown = FALSE;
    if (g_findEdit) ShowWindow(g_findEdit, SW_HIDE);
    LayoutChildren();
    InvalidateRect(g_hwnd, NULL, TRUE);
    SetFocus(g_edit);
}

/* ------------------------------------------------------------------ */
/* outline popup (Ctrl+P): filter + jump to headings                    */
/* ------------------------------------------------------------------ */

static void HideOutline(void);

/* collect heading lines matching the filter and (re)fill the listbox */
static void OutlineFill(void)
{
    wchar_t filter[128];
    GetWindowTextW(g_olFilter, filter, 128);
    int fl = lstrlenW(filter);
    for (int i = 0; i < fl; i++) filter[i] = lowerW(filter[i]);

    SendMessageW(g_olList, LB_RESETCONTENT, 0, 0);
    int nItems = 0;

    int total = GetWindowTextLengthW(g_edit);
    int lineCount = (int)SendMessageW(g_edit, EM_GETLINECOUNT, 0, 0);
    int pos = 0;
    wchar_t buf[1024];
    for (int li = 0; li < lineCount && pos <= total; li++) {
        int ll = (int)SendMessageW(g_edit, EM_LINELENGTH, pos, 0);
        if (ll > 0 && ll < 1024) {
            buf[0] = 1024;
            int n = (int)SendMessageW(g_edit, EM_GETLINE, li, (LPARAM)buf);
            if (n > 0) {
                buf[n] = 0;
                int h = 0;
                while (h < 6 && buf[h] == L'#') h++;
                if (h >= 1 && h <= 6 && buf[h] == L' ') {
                    const wchar_t *txt = buf + h + 1;
                    int tl = n - h - 1;
                    while (tl > 0 && txt[0] == L' ') { txt++; tl--; }
                    if (tl > 0) {
                        /* substring match, case-insensitive */
                        BOOL ok = (fl == 0);
                        if (!ok) {
                            for (int s = 0; s <= tl - fl && !ok; s++) {
                                int k = 0;
                                while (k < fl
                                       && lowerW(txt[s + k]) == filter[k])
                                    k++;
                                if (k == fl) ok = TRUE;
                            }
                        }
                        if (ok) {
                            wchar_t item[1100];
                            /* indent by level for a quick hierarchy view */
                            int pad = (h - 1) * 2;
                            int e = 0;
                            for (int k = 0; k < pad && e < 8; k++)
                                item[e++] = L' ';
                            int copy = tl > 1000 ? 1000 : tl;
                            for (int k = 0; k < copy; k++)
                                item[e++] = txt[k];
                            item[e] = 0;
                            SendMessageW(g_olList, LB_ADDSTRING, 0,
                                         (LPARAM)item);
                            if (nItems >= g_olLinesCap) {
                                int cap = g_olLinesCap
                                        ? g_olLinesCap * 2 : 256;
                                int *nb = (int *)realloc(g_olLines,
                                                         cap * sizeof(int));
                                if (nb) { g_olLines = nb; g_olLinesCap = cap; }
                            }
                            if (nItems < g_olLinesCap)
                                g_olLines[nItems] = li;
                            nItems++;
                        }
                    }
                }
            }
        }
        pos += ll + 2;   /* skip line + CRLF */
    }
    if (nItems > 0) SendMessageW(g_olList, LB_SETCURSEL, 0, 0);
}

/* jump to the heading selected in the list */
static void OutlineJump(void)
{
    int sel = (int)SendMessageW(g_olList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= g_olLinesCap || !g_olLines) return;
    int li = g_olLines[sel];
    int ls = (int)SendMessageW(g_edit, EM_LINEINDEX, li, 0);
    int ll = (int)SendMessageW(g_edit, EM_LINELENGTH, ls, 0);
    HideOutline();
    if (g_view == VIEW_PREVIEW) SetView(VIEW_SPLIT);
    SendMessageW(g_edit, EM_SETSEL, ls, ls + ll);
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
    SetFocus(g_edit);
}

static LRESULT CALLBACK OlFilterProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) { HideOutline(); return 0; }
        if (wp == VK_DOWN && (GetKeyState(VK_CONTROL) & 0x8000) == 0) {
            SetFocus(g_olList);
            return 0;
        }
        if (wp == VK_RETURN) { OutlineJump(); return 0; }
    }
    if (msg == WM_KILLFOCUS) {
        HWND nf = (HWND)wp;
        if (nf != g_olWnd && nf != g_olFilter && nf != g_olList)
            PostMessageW(g_hwnd, WM_EDITCMD, IDM_OUTLINE, 0);
        return 0;
    }
    return CallWindowProcW(g_olFilterProc, h, msg, wp, lp);
}

static LRESULT CALLBACK OlListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) { OutlineJump(); return 0; }
        if (wp == VK_ESCAPE) { HideOutline(); return 0; }
    }
    if (msg == WM_KILLFOCUS) {
        HWND nf = (HWND)wp;
        if (nf != g_olWnd && nf != g_olFilter && nf != g_olList)
            PostMessageW(g_hwnd, WM_EDITCMD, IDM_OUTLINE, 0);
        return 0;
    }
    return CallWindowProcW(g_olListProc, h, msg, wp, lp);
}

static LRESULT CALLBACK OutlineProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == g_olFilter) {
            OutlineFill();
            return 0;
        }
        if (HIWORD(wp) == LBN_DBLCLK && (HWND)lp == g_olList) {
            OutlineJump();
            return 0;
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { HideOutline(); return 0; }
        break;
    case WM_KILLFOCUS:
        /* clicking anywhere else closes the outline */
        if (GetFocus() != g_olFilter && GetFocus() != g_olList)
            HideOutline();
        return 0;
    case WM_NCDESTROY:
        g_olWnd = NULL;
        g_olFilter = NULL;
        g_olList = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ShowOutline(void)
{
    if (g_olWnd) { SetFocus(g_olFilter); return; }
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = OutlineProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"MDLiteOutline";
        if (!RegisterClassW(&wc)) return;
        registered = TRUE;
    }
    RECT rcMain;
    GetWindowRect(g_hwnd, &rcMain);
    int w = SC(360), h = SC(300);
    int x = rcMain.left + (rcMain.right - rcMain.left - w) / 2;
    int y = rcMain.top + HeaderH() + SC(56);
    g_olWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"MDLiteOutline", NULL, WS_POPUP | WS_BORDER,
        x, y, w, h, g_hwnd, NULL, NULL, NULL);
    if (!g_olWnd) return;

    g_olFilter = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        SC(10), SC(10), w - SC(20), SC(26), g_olWnd, (HMENU)1, NULL, NULL);
    g_olFilterProc = (WNDPROC)SetWindowLongPtrW(g_olFilter, GWLP_WNDPROC,
                                                (LONG_PTR)OlFilterProc);
    SendMessageW(g_olFilter, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);

    g_olList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
        SC(10), SC(44), w - SC(20), h - SC(54), g_olWnd, (HMENU)2, NULL, NULL);
    g_olListProc = (WNDPROC)SetWindowLongPtrW(g_olList, GWLP_WNDPROC,
                                              (LONG_PTR)OlListProc);
    SendMessageW(g_olList, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);

    OutlineFill();
    ShowWindow(g_olWnd, SW_SHOW);
    SetFocus(g_olFilter);
}

static void HideOutline(void)
{
    if (g_olWnd) DestroyWindow(g_olWnd);
    SetFocus(g_edit);
}

/* ------------------------------------------------------------------ */
/* links panel: backlinks of the current note + orphan notes           */
/* ------------------------------------------------------------------ */

static void HideLinks(void);

static HWND    g_lkWnd, g_lkList;
static WNDPROC g_lkListProc;
static wchar_t (*g_lkPaths)[MAX_PATH];
static int     g_lkN;      /* file entries stored (headers not counted) */
static int     g_lkBackN;  /* backlink entries before the orphan ones */

static void LinksJump(void)
{
    int sel = (int)SendMessageW(g_lkList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || !g_lkPaths) return;
    /* header rows: index 0 and the one right after the backlinks */
    if (sel == 0 || sel == g_lkBackN + 1) return;
    int pi = sel <= g_lkBackN ? sel - 1 : sel - 2;
    if (pi < 0 || pi >= g_lkN) return;
    wchar_t path[MAX_PATH];
    lstrcpynW(path, g_lkPaths[pi], MAX_PATH);
    HideLinks();
    LoadFile(path);
}

static LRESULT CALLBACK LkListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) { LinksJump(); return 0; }
        if (wp == VK_ESCAPE) { HideLinks(); return 0; }
    }
    if (msg == WM_KILLFOCUS) {
        if ((HWND)wp != g_lkWnd && (HWND)wp != g_lkList)
            PostMessageW(g_hwnd, WM_EDITCMD, IDM_LINKS, 0);
        return 0;
    }
    return CallWindowProcW(g_lkListProc, h, msg, wp, lp);
}

static LRESULT CALLBACK LinksProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wp) == LBN_DBLCLK && (HWND)lp == g_lkList) {
            LinksJump();
            return 0;
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { HideLinks(); return 0; }
        break;
    case WM_KILLFOCUS:
        if (GetFocus() != g_lkList) HideLinks();
        return 0;
    case WM_NCDESTROY:
        g_lkWnd = NULL;
        g_lkList = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* base name of a path, for list display */
static const wchar_t *BaseNameOf(const wchar_t *path)
{
    const wchar_t *b = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') b = p + 1;
    return b;
}

static BOOL LinksPanelOpen(void)
{
    return g_lkWnd != NULL;
}

static void LinksFill(void)
{
    free(g_lkPaths);
    g_lkPaths = NULL;
    g_lkN = 0;
    g_lkBackN = 0;
    SendMessageW(g_lkList, LB_RESETCONTENT, 0, 0);

    TreeScanLinks();

    wchar_t (*bl)[MAX_PATH] = NULL;
    int nb = TreeBacklinks(g_path, &bl);
    wchar_t (*orph)[MAX_PATH] = NULL;
    int no = TreeOrphans(&orph);

    if (nb + no > 0)
        g_lkPaths = (wchar_t (*)[MAX_PATH])malloc(
            (nb + no) * sizeof(*g_lkPaths));

    wchar_t hdr[96];
    wsprintfW(hdr, L"── 反向链接（%d）──", nb);
    SendMessageW(g_lkList, LB_ADDSTRING, 0, (LPARAM)hdr);
    for (int i = 0; i < nb; i++) {
        if (!g_lkPaths) break;
        lstrcpynW(g_lkPaths[g_lkN++], bl[i], MAX_PATH);
        SendMessageW(g_lkList, LB_ADDSTRING, 0,
                     (LPARAM)BaseNameOf(bl[i]));
    }
    g_lkBackN = g_lkN;

    wsprintfW(hdr, L"── 孤儿笔记（%d）──", no);
    SendMessageW(g_lkList, LB_ADDSTRING, 0, (LPARAM)hdr);
    for (int i = 0; i < no; i++) {
        if (!g_lkPaths) break;
        lstrcpynW(g_lkPaths[g_lkN++], orph[i], MAX_PATH);
        SendMessageW(g_lkList, LB_ADDSTRING, 0,
                     (LPARAM)BaseNameOf(orph[i]));
    }
    free(bl);
    free(orph);
}

static void ShowLinks(void)
{
    if (g_lkWnd) { SetFocus(g_lkList); return; }
    if (!g_path[0]) {
        MessageBoxW(g_hwnd,
            L"当前文档尚未保存到磁盘，暂无反向链接。",
            APP_NAME, MB_ICONINFORMATION);
        return;
    }
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = LinksProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"MDLiteLinks";
        if (!RegisterClassW(&wc)) return;
        registered = TRUE;
    }
    RECT rcMain;
    GetWindowRect(g_hwnd, &rcMain);
    int w = SC(400), h = SC(440);
    int x = rcMain.left + (rcMain.right - rcMain.left - w) / 2;
    int y = rcMain.top + HeaderH() + SC(56);
    g_lkWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"MDLiteLinks", NULL, WS_POPUP | WS_BORDER,
        x, y, w, h, g_hwnd, NULL, NULL, NULL);
    if (!g_lkWnd) return;

    g_lkList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
        SC(10), SC(10), w - SC(20), h - SC(20), g_lkWnd, (HMENU)1,
        NULL, NULL);
    g_lkListProc = (WNDPROC)SetWindowLongPtrW(g_lkList, GWLP_WNDPROC,
                                              (LONG_PTR)LkListProc);
    SendMessageW(g_lkList, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);

    LinksFill();
    ShowWindow(g_lkWnd, SW_SHOW);
    SetFocus(g_lkList);
}

static void HideLinks(void)
{
    if (g_lkWnd) DestroyWindow(g_lkWnd);
    free(g_lkPaths);
    g_lkPaths = NULL;
    SetFocus(g_edit);
}

/* ------------------------------------------------------------------ */
/* insert menu (@): filter + insert markdown snippets                  */
/* ------------------------------------------------------------------ */

static void HideInsertMenu(void);

typedef struct {
    const wchar_t *label;
    const wchar_t *before;   /* prefix inserted before the caret */
    const wchar_t *sel;      /* inserted selected, ready to overtype */
    const wchar_t *after;    /* suffix inserted after the selection */
} InsItem;

static const InsItem INS_ITEMS[] = {
    { L"二级标题", L"\r\n## ", L"标题", L"" },
    { L"三级标题", L"\r\n### ", L"标题", L"" },
    { L"无序列表", L"\r\n", L"- 列表项", L"" },
    { L"有序列表", L"\r\n", L"1. 列表项", L"" },
    { L"任务列表", L"\r\n- [ ] ", L"任务", L"" },
    { L"引用块",   L"\r\n", L"> 引用内容", L"\r\n" },
    { L"表格",     L"\r\n",
      L"| 列一 | 列二 |\r\n| --- | --- |\r\n| 内容 | 内容 |", L"\r\n" },
    { L"代码块",   L"\r\n```\r\n", L"代码", L"\r\n```\r\n" },
    { L"行内代码", L"`", L"代码", L"`" },
    { L"粗体",     L"**", L"粗体文本", L"**" },
    { L"斜体",     L"*", L"斜体文本", L"*" },
    { L"删除线",   L"~~", L"删除文本", L"~~" },
    { L"高亮",     L"==", L"高亮文本", L"==" },
    { L"链接",     L"[", L"链接文字", L"](https://example.com)" },
    { L"图片",     L"![", L"图片说明", L"](image.png)" },
    { L"脚注",     L"[^", L"1", L"]" },
    { L"分隔线",   L"\r\n---\r\n", L"", L"" },
};
#define INS_N (int)(sizeof(INS_ITEMS) / sizeof(INS_ITEMS[0]))

static HWND   g_insWnd, g_insFilter, g_insList;
static WNDPROC g_insFilterProc, g_insListProc;
static int    g_insVis[INS_N < 32 ? 32 : INS_N];
static int    g_insVisN;

/* refill the listbox from the filter text */
static void InsFill(void)
{
    wchar_t filter[64];
    GetWindowTextW(g_insFilter, filter, 64);
    int fl = lstrlenW(filter);
    for (int i = 0; i < fl; i++) filter[i] = lowerW(filter[i]);

    SendMessageW(g_insList, LB_RESETCONTENT, 0, 0);
    g_insVisN = 0;
    for (int i = 0; i < INS_N; i++) {
        BOOL ok = (fl == 0);
        if (!ok) {
            const wchar_t *lbl = INS_ITEMS[i].label;
            int ll = lstrlenW(lbl);
            for (int s = 0; s <= ll - fl && !ok; s++) {
                int k = 0;
                while (k < fl && lowerW(lbl[s + k]) == filter[k]) k++;
                if (k == fl) ok = TRUE;
            }
        }
        if (ok) {
            SendMessageW(g_insList, LB_ADDSTRING, 0,
                         (LPARAM)INS_ITEMS[i].label);
            g_insVis[g_insVisN++] = i;
        }
    }
    if (g_insVisN > 0) SendMessageW(g_insList, LB_SETCURSEL, 0, 0);
}

/* insert the selected snippet at the edit caret (replacing any selection) */
static void InsApply(void)
{
    int sel = (int)SendMessageW(g_insList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= g_insVisN) return;
    const InsItem *it = &INS_ITEMS[g_insVis[sel]];

    DWORD s0 = 0, e0 = 0;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
    /* block snippets start with CRLF so they land on their own line;
     * drop it when the caret already sits at line start */
    int lead = 0;
    if (it->before[0] == L'\r') {
        int li = (int)SendMessageW(g_edit, EM_LINEFROMCHAR, s0, 0);
        if ((int)SendMessageW(g_edit, EM_LINEINDEX, li, 0) == (int)s0)
            lead = 2;
    }

    wchar_t buf[512];
    int n = 0;
    const wchar_t *parts[3] = { it->before, it->sel, it->after };
    for (int p = 0; p < 3; p++)
        for (const wchar_t *q = parts[p]; *q && n < 510; q++)
            buf[n++] = *q;
    buf[n] = 0;

    HideInsertMenu();
    SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)(buf + lead));
    int start = (int)s0 + lstrlenW(it->before) - lead;
    int selLen = lstrlenW(it->sel);
    if (start < 0) start = 0;
    SendMessageW(g_edit, EM_SETSEL, start, start + selLen);
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
    SetFocus(g_edit);
}

static LRESULT CALLBACK InsFilterProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_DOWN && g_insVisN > 0) {
            int cur = (int)SendMessageW(g_insList, LB_GETCURSEL, 0, 0);
            if (cur < 0) cur = 0;
            if (cur + 1 < g_insVisN)
                SendMessageW(g_insList, LB_SETCURSEL, cur + 1, 0);
            return 0;
        }
        if (wp == VK_RETURN) { InsApply(); return 0; }
        if (wp == VK_ESCAPE) { HideInsertMenu(); return 0; }
    }
    if (msg == WM_KILLFOCUS) {
        HWND nf = (HWND)wp;
        if (nf != g_insWnd && nf != g_insFilter && nf != g_insList)
            HideInsertMenu();
        return 0;
    }
    return CallWindowProcW(g_insFilterProc, h, msg, wp, lp);
}

static LRESULT CALLBACK InsListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) { InsApply(); return 0; }
        if (wp == VK_ESCAPE) { HideInsertMenu(); return 0; }
    }
    if (msg == WM_KILLFOCUS) {
        HWND nf = (HWND)wp;
        if (nf != g_insWnd && nf != g_insFilter && nf != g_insList)
            HideInsertMenu();
        return 0;
    }
    return CallWindowProcW(g_insListProc, h, msg, wp, lp);
}

static LRESULT CALLBACK InsertProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == g_insFilter) {
            InsFill();
            return 0;
        }
        if (HIWORD(wp) == LBN_DBLCLK && (HWND)lp == g_insList) {
            InsApply();
            return 0;
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { HideInsertMenu(); return 0; }
        break;
    case WM_KILLFOCUS:
        if (GetFocus() != g_insFilter && GetFocus() != g_insList)
            HideInsertMenu();
        return 0;
    case WM_NCDESTROY:
        g_insWnd = NULL;
        g_insFilter = NULL;
        g_insList = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* [[ note-name autocomplete - Obsidian-style: the caret stays in the  */
/* editor, the filter word is the text between "[[" and the caret,    */
/* arrows / Enter / Tab / Esc are intercepted by EditProc.            */
/* ------------------------------------------------------------------ */

#define WK_MAX_NAMES 500
#define WK_MAX_VIS   256
static void WkResize(void);
static void SlResize(void);
static HWND  g_wkWnd, g_wkList;
static wchar_t (*g_wkNames)[96];
static int   g_wkNameN;
static int   g_wkVis[WK_MAX_VIS];
static int   g_wkVisN;
static int   g_wkFilterLen;      /* filter length at last check */
static int   g_wkAnchor;         /* abs pos just after "[[" */
static int   g_wkHH;             /* current popup height */

void HideWikiMenu(void)
{
    if (g_wkWnd) DestroyWindow(g_wkWnd);
    g_wkWnd = NULL;
    g_wkList = NULL;
}

BOOL WikiMenuActive(void)
{
    return g_wkWnd != NULL;
}

static void WkCollect(const wchar_t *path, void *ctx)
{
    (void)ctx;
    if (g_wkNameN >= WK_MAX_NAMES) return;
    /* base name without extension */
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    int n = 0;
    while (base[n] && base[n] != L'.' && n < 95) n++;
    if (n <= 0) return;
    /* dedupe (case-insensitive against stored names) */
    for (int i = 0; i < g_wkNameN; i++) {
        int same = 1;
        for (int k = 0; k < n; k++) {
            if (lowerW(g_wkNames[i][k]) != lowerW(base[k])
                || g_wkNames[i][k] == 0) { same = 0; break; }
        }
        if (same && g_wkNames[i][n] == 0) return;
    }
    for (int i = 0; i < n; i++) g_wkNames[g_wkNameN][i] = base[i];
    g_wkNames[g_wkNameN][n] = 0;
    g_wkNameN++;
}

static void WkFill(const wchar_t *filter, int flen)
{
    wchar_t low[96];
    if (flen > 95) flen = 95;
    for (int i = 0; i < flen; i++) low[i] = lowerW(filter[i]);
    low[flen] = 0;

    SendMessageW(g_wkList, LB_RESETCONTENT, 0, 0);
    g_wkVisN = 0;
    for (int i = 0; i < g_wkNameN && g_wkVisN < WK_MAX_VIS; i++) {
        BOOL ok = (flen == 0);
        if (!ok) {
            int nl0 = lstrlenW(g_wkNames[i]);
            for (int k = 0; k <= nl0 - flen && !ok; k++) {
                int m = 0;
                while (m < flen && lowerW(g_wkNames[i][k + m]) == low[m]) m++;
                if (m == flen) ok = TRUE;
            }
        }
        if (ok) {
            SendMessageW(g_wkList, LB_ADDSTRING, 0,
                         (LPARAM)g_wkNames[i]);
            g_wkVis[g_wkVisN++] = i;
        }
    }
    if (g_wkVisN > 0) SendMessageW(g_wkList, LB_SETCURSEL, 0, 0);
    WkResize();
}

/* size the popup to the visible row count (max 10 rows) */
static void WkResize(void)
{
    if (!g_wkWnd) return;
    int rows = g_wkVisN;
    if (rows < 1) rows = 1;
    if (rows > 10) rows = 10;
    g_wkHH = SC(8) + rows * SC(26);
    SetWindowPos(g_wkWnd, NULL, 0, 0, SC(260), g_wkHH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    MoveWindow(g_wkList, 0, 0, SC(260), g_wkHH, TRUE);
}

static void WkApply(void)
{
    int sel = (int)SendMessageW(g_wkList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= g_wkVisN) return;
    const wchar_t *name = g_wkNames[g_wkVis[sel]];
    DWORD s0 = 0, e0 = 0;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
    int start = g_wkAnchor;
    if (start < 0 || (DWORD)start > s0) { HideWikiMenu(); return; }
    wchar_t rep[128];
    wsprintfW(rep, L"[[%s]]", name);
    HideWikiMenu();
    SendMessageW(g_edit, EM_SETSEL, start, s0);
    SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)rep);
    int end = start + lstrlenW(rep);
    SendMessageW(g_edit, EM_SETSEL, end, end);
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
}

static void ShowWikiMenu(void)
{
    if (g_wkWnd) return;
    if (!g_wkNames)
        g_wkNames = (wchar_t (*)[96])malloc(
            WK_MAX_NAMES * sizeof(*g_wkNames));
    if (!g_wkNames) return;
    g_wkNameN = 0;
    TreeForEachFile(WkCollect, NULL);
    if (g_wkNameN == 0) return;

    g_wkWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"STATIC", NULL, WS_POPUP,
        0, 0, SC(260), SC(180), g_hwnd, NULL, NULL, NULL);
    g_wkList = CreateWindowExW(0, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY
        | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
        0, 0, SC(260), SC(180), g_wkWnd, (HMENU)3, NULL, NULL);
    SendMessageW(g_wkList, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
}

static void WkPosition(void)
{
    if (!g_wkWnd) return;
    DWORD s0 = 0, e0 = 0;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
    /* EM_POSFROMCHAR is reliable regardless of caret visibility
     * (GetCaretPos returns (0,0) whenever the caret is not realized,
     * which parked the popup at the top-left of the screen) */
    LPARAM pp = SendMessageW(g_edit, EM_POSFROMCHAR, (WPARAM)s0, 0);
    POINT pt = { (int)(short)LOWORD(pp), (int)(short)HIWORD(pp) };
    ClientToScreen(g_edit, &pt);   /* now screen coordinates */
    RECT rcBody;
    GetBodyRect(&rcBody);
    POINT tl = { rcBody.left, rcBody.top }, br = { rcBody.right, rcBody.bottom };
    ClientToScreen(g_hwnd, &tl);
    ClientToScreen(g_hwnd, &br);
    int x = pt.x, y = pt.y + SC(22);   /* caret lower-right */
    int w = SC(260), hh = g_wkHH ? g_wkHH : SC(180);
    if (x + w > br.x) x = br.x - w;
    if (x < tl.x) x = tl.x;
    if (y + hh > br.y) y = pt.y - SC(6) - hh;   /* flip above when tight */
    if (y < tl.y) y = tl.y;
    SetWindowPos(g_wkWnd, HWND_TOPMOST, x, y, w, hh,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void WikiCompleteCheck(HWND edit)
{
    (void)edit;
    if (g_view != VIEW_EDIT && g_view != VIEW_SPLIT) {
        HideWikiMenu();
        return;
    }
    DWORD s0 = 0, e0 = 0;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
    if (s0 != e0) { HideWikiMenu(); return; }
    int li = (int)SendMessageW(g_edit, EM_LINEFROMCHAR, s0, 0);
    int ls = (int)SendMessageW(g_edit, EM_LINEINDEX, li, 0);
    int off = (int)s0 - ls;
    wchar_t buf[1024];
    *(LPWORD)buf = (WORD)(sizeof(buf) / sizeof(wchar_t) - 1);
    int got = (int)SendMessageW(g_edit, EM_GETLINE, li, (LPARAM)buf);
    if (got > 0) buf[got] = 0; else buf[0] = 0;
    /* find "[[" ending right before the filter word:
     * either the caret sits just after "[[" (empty filter), or the
     * scan-back over the filter stops at a '[' that forms "[[" */
    int e = -1;
    if (off >= 2 && buf[off-1] == L'[' && buf[off-2] == L'[') {
        e = off;                     /* just typed "[[" */
    } else {
        int b = off;
        while (b > 0 && buf[b-1] != L'[' && buf[b-1] != L']'
               && (off - b) < 64)
            b--;
        if (b >= 2 && buf[b-1] == L'[' && buf[b-2] == L'[') e = b;
    }
    if (e < 0) { HideWikiMenu(); return; }
    int flen = off - e;
    if (flen > 64) { HideWikiMenu(); return; }

    if (!g_wkWnd) ShowWikiMenu();
    if (!g_wkWnd) return;
    g_wkFilterLen = flen;
    g_wkAnchor = ls + e;
    WkFill(buf + e, flen);
    if (g_wkVisN == 0) { HideWikiMenu(); return; }
    WkPosition();
}

void WikiCompleteKey(HWND edit, UINT vk, BOOL *eaten)
{
    (void)edit;
    *eaten = 0;
    if (!g_wkWnd) return;
    if (vk == VK_DOWN || vk == VK_UP) {
        int cur = (int)SendMessageW(g_wkList, LB_GETCURSEL, 0, 0);
        if (cur < 0) cur = 0;
        if (vk == VK_DOWN && cur + 1 < g_wkVisN) cur++;
        if (vk == VK_UP && cur > 0) cur--;
        SendMessageW(g_wkList, LB_SETCURSEL, cur, 0);
        *eaten = 1;
    } else if (vk == VK_RETURN || vk == VK_TAB) {
        WkApply();
        *eaten = 1;
    } else if (vk == VK_ESCAPE) {
        HideWikiMenu();
        *eaten = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Slash command menu - Notion-style block commands. The '/' stays in */
/* the editor and acts as the filter word; arrows / Enter / Tab / Esc */
/* work like the wiki menu. An unmatched filter auto-hides the popup  */
/* so line-leading "/question" + Enter keeps working as the AI prompt */
/* and "//cmd" keeps working as the agent prompt.                      */
/* ------------------------------------------------------------------ */

enum { SL_TEXT = 0, SL_FRONTMATTER = 11, SL_DATE = 13 };

typedef struct {
    const wchar_t *label;
    const wchar_t *before, *sel, *after;  /* used when id == SL_TEXT */
    int id;
} SlItem;

static const SlItem SLASH_ITEMS[] = {
    { L"任务列表", L"\r\n- [ ] ", L"任务", L"", 0 },
    { L"无序列表", L"\r\n", L"- 列表项", L"", 0 },
    { L"有序列表", L"\r\n", L"1. 列表项", L"", 0 },
    { L"代码块",   L"\r\n```\r\n", L"代码", L"\r\n```\r\n", 0 },
    { L"表格",     L"\r\n| 列一 | 列二 |\r\n| --- | --- |\r\n"
                   L"| 内容 | 内容 |", L"", L"\r\n", 0 },
    { L"引用块",   L"\r\n", L"> 引用内容", L"\r\n", 0 },
    { L"提示框",   L"\r\n> [!NOTE] ", L"标题", L"\r\n> 正文\r\n", 0 },
    { L"突出显示", L"\r\n==", L"重点内容", L"==\r\n", 0 },
    { L"帽头",     NULL, NULL, NULL, SL_FRONTMATTER },
    { L"分隔线",   L"\r\n---\r\n", L"", L"", 0 },
    { L"今日日期", NULL, NULL, NULL, SL_DATE },
};
#define SL_N (int)(sizeof(SLASH_ITEMS) / sizeof(SLASH_ITEMS[0]))

static HWND g_slWnd, g_slList;
static int  g_slVis[SL_N];
static int  g_slVisN;
static int  g_slAnchor;        /* abs pos just after the '/' */
static int  g_slHH;            /* current popup height */

void HideSlashMenu(void)
{
    if (g_slWnd) DestroyWindow(g_slWnd);
    g_slWnd = NULL;
    g_slList = NULL;
}

BOOL SlashMenuActive(void)
{
    return g_slWnd != NULL;
}

static void SlFill(const wchar_t *filter, int flen)
{
    wchar_t low[40];
    if (flen > 38) flen = 38;
    for (int i = 0; i < flen; i++) low[i] = lowerW(filter[i]);
    low[flen] = 0;

    SendMessageW(g_slList, LB_RESETCONTENT, 0, 0);
    g_slVisN = 0;
    for (int i = 0; i < SL_N; i++) {
        BOOL ok = (flen == 0);
        if (!ok) {
            const wchar_t *lbl = SLASH_ITEMS[i].label;
            int ll = lstrlenW(lbl);
            for (int s = 0; s <= ll - flen && !ok; s++) {
                int k = 0;
                while (k < flen && lowerW(lbl[s + k]) == low[k]) k++;
                if (k == flen) ok = TRUE;
            }
        }
        if (ok) {
            SendMessageW(g_slList, LB_ADDSTRING, 0,
                         (LPARAM)SLASH_ITEMS[i].label);
            g_slVis[g_slVisN++] = i;
        }
    }
    if (g_slVisN > 0) SendMessageW(g_slList, LB_SETCURSEL, 0, 0);
    SlResize();
}

/* size the popup to the visible row count (max 10 rows) */
static void SlResize(void)
{
    if (!g_slWnd) return;
    int rows = g_slVisN;
    if (rows < 1) rows = 1;
    if (rows > 10) rows = 10;
    g_slHH = SC(8) + rows * SC(26);
    SetWindowPos(g_slWnd, NULL, 0, 0, SC(260), g_slHH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    MoveWindow(g_slList, 0, 0, SC(260), g_slHH, TRUE);
}

/* replace "/filter" at the caret with the chosen command */
static void SlApply(void)
{
    int sel = (int)SendMessageW(g_slList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= g_slVisN) return;
    const SlItem *it = &SLASH_ITEMS[g_slVis[sel]];

    DWORD s0 = 0, e0 = 0;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
    int start = g_slAnchor;
    if (start < 0 || (DWORD)start > s0) { HideSlashMenu(); return; }

    wchar_t buf[600];
    int  n = 0, lead = 0;
    int  selAt = -1, selLen = 0;   /* range to select after insert */
    SYSTEMTIME st;
    GetLocalTime(&st);

    if (it->id == SL_DATE) {
        n = wsprintfW(buf, L"%04d-%02d-%02d",
                      st.wYear, st.wMonth, st.wDay);
    } else if (it->id == SL_FRONTMATTER) {
        n = wsprintfW(buf,
            L"---\r\ntitle: 标题\r\ntags: []\r\ndate: "
            L"%04d-%02d-%02d\r\n---\r\n",
            st.wYear, st.wMonth, st.wDay);
        selAt = 12; selLen = 2;    /* select the 标题 placeholder */
    } else {
        /* plain text snippet - same semantics as the @ insert menu */
        const wchar_t *parts[3] = { it->before, it->sel, it->after };
        for (int p = 0; p < 3; p++)
            for (const wchar_t *q = parts[p]; *q && n < 590; q++)
                buf[n++] = *q;
        buf[n] = 0;
        /* block snippets start with CRLF; drop it at line start */
        if (buf[0] == L'\r') {
            int li = (int)SendMessageW(g_edit,
                                       EM_LINEFROMCHAR, s0, 0);
            if ((int)SendMessageW(g_edit, EM_LINEINDEX, li, 0)
                    == (int)s0) {
                memmove(buf, buf + 2, (n - 1) * sizeof(wchar_t));
                n -= 2;
                lead = 2;
            }
        }
        selAt = lstrlenW(it->before) - lead;
        selLen = lstrlenW(it->sel);
    }

    HideSlashMenu();
    /* replace from the '/' through the caret with the snippet */
    SendMessageW(g_edit, EM_SETSEL, start - 1, s0);
    SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)buf);
    int base = start - 1;
    if (selAt >= 0)
        SendMessageW(g_edit, EM_SETSEL, base + selAt,
                     base + selAt + selLen);
    else
        SendMessageW(g_edit, EM_SETSEL, base + n, base + n);
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
    SetFocus(g_edit);
}

static void ShowSlashMenu(void)
{
    if (g_slWnd) return;
    g_slWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"STATIC", NULL, WS_POPUP,
        0, 0, SC(260), SC(200), g_hwnd, NULL, NULL, NULL);
    if (!g_slWnd) return;
    g_slList = CreateWindowExW(0, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY
        | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
        0, 0, SC(260), SC(200), g_slWnd, (HMENU)4, NULL, NULL);
    SendMessageW(g_slList, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
}

static void SlPosition(void)
{
    if (!g_slWnd) return;
    DWORD s0 = 0, e0 = 0;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
    /* EM_POSFROMCHAR instead of GetCaretPos: the caret may not be
     * realized when this runs, and its (0,0) fallback parked the menu
     * at the top-left of the screen. */
    LPARAM pp = SendMessageW(g_edit, EM_POSFROMCHAR, (WPARAM)s0, 0);
    POINT pt = { (int)(short)LOWORD(pp), (int)(short)HIWORD(pp) };
    ClientToScreen(g_edit, &pt);   /* now screen coordinates */
    RECT rcBody;
    GetBodyRect(&rcBody);
    POINT tl = { rcBody.left, rcBody.top }, br = { rcBody.right, rcBody.bottom };
    ClientToScreen(g_hwnd, &tl);
    ClientToScreen(g_hwnd, &br);
    int x = pt.x, y = pt.y + SC(22);   /* caret lower-right */
    int w = SC(260), hh = g_slHH ? g_slHH : SC(200);
    if (x + w > br.x) x = br.x - w;
    if (x < tl.x) x = tl.x;
    if (y + hh > br.y) y = pt.y - SC(6) - hh;   /* flip above when tight */
    if (y < tl.y) y = tl.y;
    SetWindowPos(g_slWnd, HWND_TOPMOST, x, y, w, hh,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void SlashCompleteCheck(HWND edit)
{
    (void)edit;
    if (g_view != VIEW_EDIT && g_view != VIEW_SPLIT) {
        HideSlashMenu();
        return;
    }
    DWORD s0 = 0, e0 = 0;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
    if (s0 != e0) { HideSlashMenu(); return; }
    int li = (int)SendMessageW(g_edit, EM_LINEFROMCHAR, s0, 0);
    int ls = (int)SendMessageW(g_edit, EM_LINEINDEX, li, 0);
    int off = (int)s0 - ls;
    wchar_t buf[1024];
    *(LPWORD)buf = (WORD)(sizeof(buf) / sizeof(wchar_t) - 1);
    int got = (int)SendMessageW(g_edit, EM_GETLINE, li, (LPARAM)buf);
    if (got > 0) buf[got] = 0; else buf[0] = 0;
    /* find the nearest '/' before the caret on this line */
    int i = off;
    while (i > 0 && buf[i-1] != L'/' && buf[i-1] != L' '
           && buf[i-1] != L'\t')
        i--;
    if (i == 0 || buf[i-1] != L'/') { HideSlashMenu(); return; }
    int slash = i - 1;
    /* "//cmd" belongs to the agent and "http:/" to URLs */
    if (slash > 0 && (buf[slash-1] == L'/' || buf[slash-1] == L':')) {
        HideSlashMenu();
        return;
    }
    int flen = off - slash - 1;
    if (flen > 38) { HideSlashMenu(); return; }

    if (!g_slWnd) ShowSlashMenu();
    if (!g_slWnd) return;
    g_slAnchor = ls + slash + 1;
    SlFill(buf + slash + 1, flen);
    if (g_slVisN == 0) { HideSlashMenu(); return; }
    SlPosition();
}

void SlashCompleteKey(HWND edit, UINT vk, BOOL *eaten)
{
    (void)edit;
    *eaten = 0;
    if (!g_slWnd) return;
    if (vk == VK_DOWN || vk == VK_UP) {
        int cur = (int)SendMessageW(g_slList, LB_GETCURSEL, 0, 0);
        if (cur < 0) cur = 0;
        if (vk == VK_DOWN && cur + 1 < g_slVisN) cur++;
        if (vk == VK_UP && cur > 0) cur--;
        SendMessageW(g_slList, LB_SETCURSEL, cur, 0);
        *eaten = 1;
    } else if (vk == VK_RETURN || vk == VK_TAB) {
        SlApply();
        *eaten = 1;
    } else if (vk == VK_ESCAPE) {
        HideSlashMenu();
        *eaten = 1;
    }
}

void ShowInsertMenu(void)
{
    if (g_insWnd) { SetFocus(g_insFilter); return; }
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = InsertProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"MDLiteInsert";
        if (!RegisterClassW(&wc)) return;
        registered = TRUE;
    }
    RECT rcMain;
    GetWindowRect(g_hwnd, &rcMain);
    int w = SC(300), h = SC(320);
    int x = rcMain.left + (rcMain.right - rcMain.left - w) / 2;
    int y = rcMain.top + HeaderH() + SC(56);
    g_insWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"MDLiteInsert", NULL, WS_POPUP | WS_BORDER,
        x, y, w, h, g_hwnd, NULL, NULL, NULL);
    if (!g_insWnd) return;

    g_insFilter = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        SC(10), SC(10), w - SC(20), SC(26), g_insWnd, (HMENU)1, NULL, NULL);
    g_insFilterProc = (WNDPROC)SetWindowLongPtrW(g_insFilter, GWLP_WNDPROC,
                                                 (LONG_PTR)InsFilterProc);
    SendMessageW(g_insFilter, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);

    g_insList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
        SC(10), SC(44), w - SC(20), h - SC(54), g_insWnd, (HMENU)2, NULL, NULL);
    g_insListProc = (WNDPROC)SetWindowLongPtrW(g_insList, GWLP_WNDPROC,
                                               (LONG_PTR)InsListProc);
    SendMessageW(g_insList, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);

    InsFill();
    ShowWindow(g_insWnd, SW_SHOW);
    SetFocus(g_insFilter);
}

static void HideInsertMenu(void)
{
    if (g_insWnd) DestroyWindow(g_insWnd);
    SetFocus(g_edit);
}

static LRESULT CALLBACK FindProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            if (h == g_replEdit) {
                if (GetKeyState(VK_CONTROL) & 0x8000)
                    DoReplaceAll();
                else
                    DoReplaceOne();
                return 0;
            }
            SendMessageW(g_hwnd, WM_EDITCMD,
                         (GetKeyState(VK_SHIFT) & 0x8000) ? IDM_FINDPREV
                                                          : IDM_FINDNEXT, 0);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            HideFindBar();
            return 0;
        }
    }
    if (msg == WM_CHAR && wp == 10) return 0; /* Ctrl+Enter linefeed */
    if (msg == WM_CHAR || msg == WM_KEYUP) {
        GetWindowTextW(g_findEdit, g_findQuery, 128);
        GetWindowTextW(g_replEdit, g_replQuery, 136);
    }
    return CallWindowProcW(g_findProc, h, msg, wp, lp);
}

static void DrawFindBar(HDC dc, RECT *rcClient)
{
    RECT rc = { rcClient->left, rcClient->top + HeaderH(),
                rcClient->right, rcClient->top + HeaderH() + SC(40) };
    HBRUSH br = CreateSolidBrush(COL_HEADERBG);
    FillRect(dc, &rc, br);
    DeleteObject(br);
    HPEN pen = CreatePen(PS_SOLID, 1, COL_BORDER);
    HPEN op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, rc.left, rc.bottom - 1, NULL);
    LineTo(dc, rc.right, rc.bottom - 1);
    SelectObject(dc, op);
    DeleteObject(pen);

    static const wchar_t *lblQ = L"查找";
    static const wchar_t *lblR = L"替换";
    HFONT old = (HFONT)SelectObject(dc, g_fontHeader);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, COL_TITLE);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    int ty = rc.top + (SC(40) - tm.tmHeight) / 2;
    TextOutW(dc, SC(14), ty, lblQ, lstrlenW(lblQ));
    int qx = SC(14) + SC(34);
    int qw = (rc.right - rc.left - SC(28)) * 38 / 100;
    TextOutW(dc, qx + qw + SC(8), ty, lblR, lstrlenW(lblR));

    const wchar_t *info = g_findInfo[0] ? g_findInfo
                        : L"Enter 查找 · 替换框 Enter 逐个 · Ctrl+Enter 全部";
    int w = text_w(dc, g_fontStatus, info, lstrlenW(info));
    SelectObject(dc, g_fontStatus);
    SetTextColor(dc, COL_STATTXT);
    TextOutW(dc, rc.right - SC(14) - w,
             rc.top + (SC(40) - tm.tmHeight) / 2, info, lstrlenW(info));
    SelectObject(dc, old);
}

/* ------------------------------------------------------------------ */
/* main window proc                                                    */
/* ------------------------------------------------------------------ */

/* clickable links in preview (item 49) */
#define LINK_MAX 256
static struct { RECT rc; wchar_t url[520]; int wiki; } g_links[LINK_MAX];
static int g_linkN;

static void LinkSink(void *ctx, RECT rc, const wchar_t *url, int urlLen,
                     int wiki)
{
    (void)ctx;
    if (g_linkN >= LINK_MAX) return;
    int n = urlLen < 519 ? urlLen : 519;
    for (int i = 0; i < n; i++) g_links[g_linkN].url[i] = url[i];
    g_links[g_linkN].url[n] = 0;
    g_links[g_linkN].rc = rc;
    g_links[g_linkN].wiki = wiki;
    g_linkN++;
}

static int HitLink(int px, int py)
{
    for (int i = 0; i < g_linkN; i++)
        if (px >= g_links[i].rc.left && px < g_links[i].rc.right
            && py >= g_links[i].rc.top && py < g_links[i].rc.bottom)
            return i;
    return -1;
}

/* open a [[wiki-link]]: resolve to a workspace file; offer to create
 * the note when nothing matches */
static void OpenWikiLink(const wchar_t *target)
{
    wchar_t path[MAX_PATH];
    if (TreeJumpResolve(g_path, target, path, MAX_PATH)) {
        LoadFile(path);
        return;
    }
    wchar_t msg[600];
    wsprintfW(msg, L"笔记「%s」不存在。\n\n现在创建它吗？", target);
    if (MessageBoxW(g_hwnd, msg, APP_NAME,
                    MB_ICONQUESTION | MB_YESNO) != IDYES)
        return;
    /* create next to the current document: save current, then start a
     * new document pre-set to the target path */
    wchar_t dir[MAX_PATH];
    dir[0] = 0;
    if (g_path[0]) {
        lstrcpynW(dir, g_path, MAX_PATH);
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0;
    }
    wchar_t newp[MAX_PATH];
    if (dir[0]) {
        wsprintfW(newp, L"%s\\%s.md", dir, target);
        SetPath(newp);
    } else {
        SetPath(L"");
    }
    SetWindowTextW(g_edit, L"");
    SendMessageW(g_edit, EM_SETMODIFY, FALSE, 0);
    g_dirty = FALSE;
    if (dir[0])
        MessageBoxW(g_hwnd,
            L"已就绪：写入内容后按 Ctrl+S 保存到该文件。",
            APP_NAME, MB_ICONINFORMATION);
}

static void OpenLink(int i)
{
    if (i < 0 || i >= g_linkN) return;
    if (g_links[i].wiki) {
        OpenWikiLink(g_links[i].url);
        return;
    }
    HINSTANCE r = ShellExecuteW(g_hwnd, L"open", g_links[i].url,
                                NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        wchar_t msg[600];
        wsprintfW(msg, L"无法打开链接：%s", g_links[i].url);
        MessageBoxW(g_hwnd, msg, APP_NAME, MB_ICONWARNING);
    }
}

static void ScrollBy(int delta)
{
    RECT rcBody;
    GetBodyRect(&rcBody);
    int viewH = rcBody.bottom - rcBody.top;
    int maxScroll = g_doc.height - viewH;
    if (maxScroll < 0) maxScroll = 0;
    int newy = g_scrollY + delta;
    if (newy < 0) newy = 0;
    if (newy > maxScroll) newy = maxScroll;
    if (newy != g_scrollY) {
        g_scrollY = newy;
        SetScrollPos(g_hwnd, SB_VERT, g_scrollY, TRUE);
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
         g_edit = CreateWindowExW(0,
             g_useRichEd ? L"RichEdit50W" : L"EDIT", L"",
             WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE
             | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL
             | (g_useRichEd ? 0 : 0),
             0, 0, 0, 0, hwnd, (HMENU)1, NULL, NULL);
        /* multiline edit caps at 64K chars by default; long AI sessions
         * would silently drop inserts past the limit */
        SendMessageW(g_edit, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
        g_editProc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC,
                                                (LONG_PTR)EditProc);
         HDC dc = GetDC(hwnd);
         g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
         ReleaseDC(hwnd, dc);
         InitRichEdit();
         CreateUiFonts();
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fontEdit, TRUE);
        SendMessageW(g_edit, EM_SETLIMITTEXT, 0, 0);
        DragAcceptFiles(hwnd, TRUE);
        LayoutChildren();
        UpdateTitle();
        SetFocus(g_edit);

        /* tray icon: register once, re-add on explorer restart */
        g_taskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");
        ZeroMemory(&g_nid, sizeof(g_nid));
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = LoadIconW((HINSTANCE)GetWindowLongPtrW(hwnd,
                                    GWLP_HINSTANCE), MAKEINTRESOURCEW(1));
        if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
        lstrcpynW(g_nid.szTip, APP_NAME, 128);
        g_trayOn = Shell_NotifyIconW(NIM_ADD, &g_nid);
        md_set_link_sink(LinkSink, NULL);
        TryRestoreDraft();
        if (g_topmost)
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE);
        return 0;
    }

    default:
        if (msg == g_taskbarMsg && g_taskbarMsg && g_trayOn) {
            /* explorer restarted: re-add the tray icon */
            Shell_NotifyIconW(NIM_ADD, &g_nid);
            return 0;
        }
        break;

    case WM_SIZE:
        LayoutChildren();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        int w = rcClient.right - rcClient.left;
        int h = rcClient.bottom - rcClient.top;
        if (w <= 0 || h <= 0) { EndPaint(hwnd, &ps); return 0; }

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);

        RECT rcBody;
        GetBodyRect(&rcBody);
        g_linkN = 0;   /* repopulated by md_paint via the link sink */
        if (g_view == VIEW_PREVIEW) {
            RECT rcView = { 0, 0, w, h };
            FillRect(mem, &rcView, (HBRUSH)GetStockObject(WHITE_BRUSH));
            md_paint(&g_doc, mem, &rcBody, g_scrollY, &g_fonts, g_path);
        } else if (g_view == VIEW_SPLIT) {
            RECT rcPrev;
            PreviewRect(&rcPrev);
            RECT rcWhite = { rcPrev.left, rcPrev.top, rcPrev.right + 1,
                             rcPrev.bottom };
            FillRect(mem, &rcWhite, (HBRUSH)GetStockObject(WHITE_BRUSH));
            md_paint(&g_doc, mem, &rcPrev, g_scrollY, &g_fonts, g_path);
            /* divider gutter */
            RECT rcDiv = { rcPrev.right + 1, rcPrev.top,
                           rcPrev.right + 2, rcPrev.bottom };
            HBRUSH db = CreateSolidBrush(COL_BORDER);
            FillRect(mem, &rcDiv, db);
            DeleteObject(db);
            /* slim preview scrollbar on the divider's left edge */
            int docH = g_doc.height;
            int viewH = rcPrev.bottom - rcPrev.top;
            if (docH > viewH && viewH > 0) {
                int trackTop = rcPrev.top;
                int trackH = viewH;
                int thumbH = viewH * viewH / docH;
                if (thumbH < SC(18)) thumbH = SC(18);
                int thumbY = trackTop + (trackH - thumbH) * g_scrollY
                             / (docH - viewH);
                RECT rcTrack = { rcPrev.right - SC(6), trackTop,
                                 rcPrev.right, trackTop + trackH };
                HBRUSH tb = CreateSolidBrush(RGB(0xE8,0xE8,0xED));
                FillRect(mem, &rcTrack, tb);
                DeleteObject(tb);
                RECT rcThumb = { rcPrev.right - SC(6), thumbY,
                                 rcPrev.right, thumbY + thumbH };
                HBRUSH hb = CreateSolidBrush(g_splitDrag
                            ? COL_ACCENT : RGB(0xC7,0xC7,0xCC));
                FillRect(mem, &rcThumb, hb);
                DeleteObject(hb);
            }
        } else {
            FillRect(mem, &rcBody, (HBRUSH)GetStockObject(WHITE_BRUSH));
        }
        DrawHeader(mem, &rcClient);
        if (g_view == VIEW_GRAPH) {
            /* graph view uses the full body area */
            RECT rcG = rcBody;
            GraphDraw(mem, &rcG);
        } else {
            TreeDraw(mem, &rcClient);
            if (g_findShown) DrawFindBar(mem, &rcClient);
        }
        DrawStatus(mem, &rcClient);

        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == g_edit) {
            if (!g_loading) {
                g_dirty = TRUE;
                g_archivedTree[0] = 0; /* content changed -> archive next save */
                UpdateTitle();
            }
            KickPreview();
        }
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == g_findEdit
            && g_findShown) {
            GetWindowTextW(g_findEdit, g_findQuery, 128);
            SendMessageW(g_edit, EM_SETSEL, 0, 0);
            DoFind(1);
        }
        return 0;

    case WM_EDITCMD:
        switch (wp) {
        case IDM_OPEN:   if (ConfirmDiscard()) DoOpen();   return 0;
        case IDM_SAVE:   DoSave();                          return 0;
        case IDM_SAVEAS: {
            wchar_t buf[MAX_PATH];
            if (PromptSaveAs(buf, MAX_PATH))
                SaveFileEx(buf, FALSE);
            return 0;
        }
        case IDM_NEW:      DoNew();                 return 0;
        case IDM_TOGGLE:   SetView(g_view == VIEW_EDIT ? VIEW_SPLIT
                            : (g_view == VIEW_SPLIT ? VIEW_PREVIEW
                                                    : VIEW_EDIT)); return 0;
        case IDM_GRAPH:
            SetView(g_view == VIEW_GRAPH ? VIEW_EDIT : VIEW_GRAPH);
            return 0;
        case IDM_FIND:     ShowFindBar(FALSE);      return 0;
        case IDM_REPLACE:  ShowFindBar(TRUE);       return 0;
        case IDM_FINDNEXT: DoFind(1);               return 0;
        case IDM_FINDPREV: DoFind(-1);              return 0;
        case IDM_ZOOM:     ApplyZoom(wp == (WPARAM)-1 ? -1 : 1); return 0;
        case IDM_SETTINGS: ShowSettings();          return 0;
        case IDM_OUTLINE:
            if (g_olWnd) HideOutline();
            else ShowOutline();
            return 0;
        case IDM_COPYHTML:   CopyHtml();            return 0;
        case IDM_EXPORTHTML: ExportHtml();          return 0;
        case IDM_SHAREHTML:  ExportShareHtml();     return 0;
        case IDM_EXPORTTXT:  DoPlainExport();       return 0;
        case IDM_PRINT:      DoPrint();             return 0;
        case IDM_TOPMOST:
            SetTopmost(!g_topmost);
            SaveSettings();
            return 0;
        case IDM_TREEBAR:
            TreeToggle();
            LayoutChildren();   /* reflow the edit control or the tree
                                 * stays hidden underneath it */
            SaveSettings();
            return 0;
        case IDM_LINKS:
            if (g_lkWnd) HideLinks();
            else ShowLinks();
            return 0;
        case IDM_HELP:       ShowHelp();            return 0;
        case IDM_HISTORY:    ShowHistoryMenu();     return 0;
        }
        return 0;

    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            if (wp == 'S') { SendMessageW(hwnd, WM_EDITCMD, IDM_SAVE, 0); return 0; }
            if (wp == 'O') { SendMessageW(hwnd, WM_EDITCMD, IDM_OPEN, 0); return 0; }
            if (wp == 'N') { SendMessageW(hwnd, WM_EDITCMD, IDM_NEW, 0);  return 0; }
            if (wp == 'B') { SendMessageW(hwnd, WM_EDITCMD, IDM_TREEBAR, 0); return 0; }
            if (wp == 'G') { SendMessageW(hwnd, WM_EDITCMD, IDM_GRAPH, 0); return 0; }
            if (wp == 'L' && (GetKeyState(VK_SHIFT) & 0x8000)) {
                SendMessageW(hwnd, WM_EDITCMD, IDM_LINKS, 0); return 0;
            }
            if (wp == 'F') { SendMessageW(hwnd, WM_EDITCMD, IDM_FIND, 0); return 0; }
            if (wp == 'H') { SendMessageW(hwnd, WM_EDITCMD, IDM_REPLACE, 0); return 0; }
            if (wp == VK_OEM_COMMA) { SendMessageW(hwnd, WM_EDITCMD, IDM_SETTINGS, 0); return 0; }
            if (wp == VK_OEM_2) { SendMessageW(hwnd, WM_EDITCMD, IDM_TOGGLE, 0); return 0; }
        }
        if (wp == VK_F1) { ShowHelp(); return 0; }
        if (g_view == VIEW_PREVIEW) {
            int step = g_fonts.bodyH + (g_fonts.bodyH >> 1);
            RECT rcBody;
            GetBodyRect(&rcBody);
            int viewH = rcBody.bottom - rcBody.top;
            if (wp == VK_UP)          { ScrollBy(-step);  return 0; }
            if (wp == VK_DOWN)        { ScrollBy(step);   return 0; }
            if (wp == VK_PRIOR)       { ScrollBy(-viewH); return 0; }
            if (wp == VK_NEXT)        { ScrollBy(viewH);  return 0; }
            if (wp == VK_HOME)        { ScrollBy(-g_doc.height); return 0; }
            if (wp == VK_END)         { ScrollBy(g_doc.height);  return 0; }
            if (wp == VK_ESCAPE) {
                SetView(VIEW_EDIT);
                return 0;
            }
        }
        if (g_view == VIEW_GRAPH && wp == VK_ESCAPE) {
            if (GraphSelected() >= 0) {   /* 1st Esc clears focus */
                GraphSelect(-1);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            SetView(VIEW_EDIT);
            return 0;
        }
        break;

    case WM_LBUTTONDOWN: {
        int px = (short)LOWORD(lp);
        int py = (short)HIWORD(lp);
        /* the file tree owns clicks inside its strip */
        if (TreeClick((POINT){ px, py })) return 0;
        if (g_view == VIEW_GRAPH) {
            RECT rcBody;
            GetBodyRect(&rcBody);
            int hi = GraphHitTest(px, py, &rcBody);
            if (hi >= 0) {
                /* double-click on node: jump + exit graph (checked
                 * before drag start, which always succeeds on a hit) */
                DWORD now = GetTickCount();
                if (hi == g_graphLastClickIdx
                    && now - g_graphLastClickTick < GetDoubleClickTime()) {
                    wchar_t path[MAX_PATH];
                    if (GraphNodePath(hi, path, MAX_PATH)) {
                        g_graphLastClickTick = 0;
                        g_graphLastClickIdx = -1;
                        ReleaseCapture();
                        GraphDragEnd();
                        GraphSelect(-1);
                        SetView(VIEW_EDIT);
                        LoadFile(path);
                        return 0;
                    }
                }
                g_graphLastClickTick = now;
                g_graphLastClickIdx  = hi;
                GraphSelect(hi);   /* kg-style click-to-focus */
                InvalidateRect(hwnd, &rcBody, FALSE);
                if (GraphDragStart(px, py, &rcBody)) {
                    g_graphDragging = TRUE;
                    SetCapture(hwnd);
                }
            } else {
                g_graphLastClickTick = 0;
                g_graphLastClickIdx  = -1;
                GraphSelect(-1);   /* click empty space: clear focus */
                GraphPanStart(px, py);
                g_graphPanning = TRUE;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &rcBody, FALSE);
            }
            return 0;
        }
        if (g_view == VIEW_PREVIEW || g_view == VIEW_SPLIT) {
            BOOL inPrev = (g_view == VIEW_PREVIEW);
            if (!inPrev) {
                RECT rcPrev;
                PreviewRect(&rcPrev);
                inPrev = (px < rcPrev.right - SC(6));
            }
            if (inPrev) {
                int li = HitLink(px, py);
                if (li >= 0) {
                    OpenLink(li);
                    return 0;
                }
            }
        }
        if (g_view == VIEW_SPLIT) {
            RECT rcPrev;
            ZeroMemory(&rcPrev, sizeof(rcPrev));
            PreviewRect(&rcPrev);
            if (px >= rcPrev.right - SC(8) && px < rcPrev.right + 2
                && py >= rcPrev.top && py < rcPrev.bottom
                && g_doc.height > rcPrev.bottom - rcPrev.top) {
                /* hit the slim preview scrollbar: jump or start drag */
                int viewH = rcPrev.bottom - rcPrev.top;
                int thumbH = viewH * viewH / g_doc.height;
                if (thumbH < SC(18)) thumbH = SC(18);
                int thumbY = rcPrev.top + (viewH - thumbH) * g_scrollY
                             / (g_doc.height - viewH);
                if (py >= thumbY && py < thumbY + thumbH) {
                    g_splitDrag = TRUE;
                    SetCapture(hwnd);
                } else {
                    RECT rcBody;
                    GetBodyRect(&rcBody);
                    int mid = (rcBody.left + rcBody.right) / 2;
                    (void)mid;
                    int page = (py < rcPrev.top + viewH / 2) ? -viewH : viewH;
                    ScrollBy(page);
                }
                return 0;
            }
        }
        int id = HitTestHeader((POINT){ px, py });
        if (id == 0) { if (ConfirmDiscard()) DoOpen(); }
        else if (id == 1) DoSave();
        else if (id == 2) ShowMoreMenu();
        else if (id == 3) { SetTopmost(!g_topmost); SaveSettings(); }
        else if (id >= 10 && id <= 12) SetView(id - 10);
        return 0;
    }

    case WM_SETCURSOR:
        if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (g_view == VIEW_GRAPH) {
                RECT rcBody;
                GetBodyRect(&rcBody);
                if (GraphHitTest(pt.x, pt.y, &rcBody) >= 0) {
                    SetCursor(LoadCursorW(NULL, IDC_HAND));
                    return TRUE;
                }
                return FALSE;
            }
            if ((g_view == VIEW_PREVIEW || g_view == VIEW_SPLIT)
                && HitLink(pt.x, pt.y) >= 0) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_MOUSEMOVE: {
        if (g_view == VIEW_GRAPH) {
            int px = (short)LOWORD(lp);
            int py = (short)HIWORD(lp);
            RECT rcBody;
            GetBodyRect(&rcBody);
            if (g_graphDragging) {
                GraphDragMove(px, py);
                InvalidateRect(hwnd, &rcBody, FALSE);
                return 0;
            }
            if (g_graphPanning) {
                GraphPanMove(px, py);
                InvalidateRect(hwnd, &rcBody, FALSE);
                return 0;
            }
            int hi = GraphHitTest(px, py, &rcBody);
            if (hi != g_hoverId) {
                g_hoverId = hi;
                GraphSetHover(hi);
                InvalidateRect(hwnd, &rcBody, FALSE);
            }
            return 0;
        }
        if (g_splitDrag) {
            int py = (short)HIWORD(lp);
            RECT rcPrev;
            PreviewRect(&rcPrev);
            int viewH = rcPrev.bottom - rcPrev.top;
            int thumbH = viewH * viewH / g_doc.height;
            if (thumbH < SC(18)) thumbH = SC(18);
            int rel = py - rcPrev.top - thumbH / 2;
            int maxScroll = g_doc.height - viewH;
            int trackH = viewH - thumbH;
            int newy = trackH > 0
                       ? (long long)rel * maxScroll / trackH : 0;
            if (newy < 0) newy = 0;
            if (newy > maxScroll) newy = maxScroll;
            if (newy != g_scrollY) {
                g_scrollY = newy;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        POINT ptMv = { (short)LOWORD(lp), (short)HIWORD(lp) };
        TreeMouseMove(ptMv);
        int id = HitTestHeader(ptMv);
        if (id != g_hoverId) {
            g_hoverId = id;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        static BOOL tracking = FALSE;
        if (!tracking) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            tracking = TRUE;
        }
        return 0;
    }

    case WM_CONTEXTMENU:
        /* workspace tree context menu eats right-clicks inside the tree */
        if (TreeContextMenu(lp)) return 0;
        break;

    case WM_LBUTTONUP:
        if (g_splitDrag) {
            g_splitDrag = FALSE;
            ReleaseCapture();
            SetFocus(g_hwnd);   /* keep Esc alive (wine drops focus) */
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (g_graphDragging) {
            g_graphDragging = FALSE;
            GraphDragEnd();
            ReleaseCapture();
            SetFocus(g_hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (g_graphPanning) {
            g_graphPanning = FALSE;
            GraphPanEnd();
            ReleaseCapture();
            SetFocus(g_hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSELEAVE:
        if (g_hoverId != -1) {
            g_hoverId = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        TreeMouseMove((POINT){ -1, -1 });   /* clear tree row hover */
        return 0;

    case WM_VSCROLL:
        if (g_view == VIEW_PREVIEW) {
            RECT rcBody;
            GetBodyRect(&rcBody);
            int viewH = rcBody.bottom - rcBody.top;
            int step = g_fonts.bodyH + (g_fonts.bodyH >> 1);
            int newy = g_scrollY;
            switch (LOWORD(wp)) {
            case SB_LINEUP:    newy -= step;  break;
            case SB_LINEDOWN:  newy += step;  break;
            case SB_PAGEUP:    newy -= viewH; break;
            case SB_PAGEDOWN:  newy += viewH; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO si;
                si.cbSize = sizeof(si);
                si.fMask = SIF_TRACKPOS;
                GetScrollInfo(hwnd, SB_VERT, &si);
                newy = si.nTrackPos;
                break;
            }
            }
            ScrollBy(newy - g_scrollY);
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (g_view == VIEW_GRAPH) {
            RECT rcBody;
            GetBodyRect(&rcBody);
            POINT ptWh = { (short)LOWORD(lp), (short)HIWORD(lp) };
            ScreenToClient(hwnd, &ptWh);
            GraphZoomByAt(-((short)HIWORD(wp)) / WHEEL_DELTA,
                          ptWh.x, ptWh.y, &rcBody);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            SendMessageW(hwnd, WM_EDITCMD, IDM_ZOOM,
                         ((short)HIWORD(wp) > 0) ? 1 : -1);
            return 0;
        }
        {
            POINT ptWh = { (short)LOWORD(lp), (short)HIWORD(lp) };
            ScreenToClient(hwnd, &ptWh);
            if (TreePtIn(ptWh)) {
                TreeWheel(-((short)HIWORD(wp)) / WHEEL_DELTA * 3);
                return 0;
            }
        }
        if (g_view == VIEW_PREVIEW) {
            int step = g_fonts.bodyH + (g_fonts.bodyH >> 1);
            ScrollBy(-(short)HIWORD(wp) / WHEEL_DELTA * step * 2);
            return 0;
        }
        if (g_view == VIEW_SPLIT) {
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            ScreenToClient(hwnd, &pt);
            RECT rcPrev;
            PreviewRect(&rcPrev);
            if (pt.x < rcPrev.right) {
                int step = g_fonts.bodyH + (g_fonts.bodyH >> 1);
                ScrollBy(-(short)HIWORD(wp) / WHEEL_DELTA * step * 2);
                return 0;
            }
        }
        break;

    case WM_DROPFILES: {
        wchar_t buf[MAX_PATH];
        if (DragQueryFileW((HDROP)wp, 0, buf, MAX_PATH)) {
            if (ConfirmDiscard())
                if (!LoadFile(buf))
                    MessageBoxW(hwnd, L"无法打开文件。", APP_NAME,
                                MB_ICONERROR);
        }
        DragFinish((HDROP)wp);
        return 0;
    }

    case WM_SETFOCUS:
        if (g_view != VIEW_PREVIEW) SetFocus(g_edit);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_KICK) {
            KillTimer(hwnd, TIMER_KICK);
            RefreshPreviewNow();
            return 0;
        }
        if (wp == TIMER_UITICK) {
            StatusTick();
            return 0;
        }
        if (wp == TIMER_AICHUNK) {
            AiFlushPending();
            AgFlushPending();
            return 0;
        }
        if (wp == TIMER_AUTO) {
            /* silent autosave; retry on the next tick when it fails */
            if (g_dirty && g_path[0] && !g_aiBusy && !g_agBusy) {
                if (DoSaveEx(TRUE))
                    InvalidateRect(hwnd, NULL, FALSE);
            } else if (g_dirty && !g_path[0]
                       && !g_aiBusy && !g_agBusy
                       && GetTickCount() - g_lastDraftTick > 5000) {
                /* give the untitled document a real session file so the
                 * regular autosave takes over; draft is the fallback */
                if (!AdoptSessionFile())
                    WriteDraft();
            }
            return 0;
        }
        return 0;

    case WM_AI_CHUNK:
        AiOnChunk((const wchar_t *)lp, (int)wp);
        return 0;

    case WM_AI_DONE:
        AiOnDone((const wchar_t *)lp);
        return 0;

    case WM_AG_CHUNK:
        AgOnChunk((const wchar_t *)lp, (int)wp);
        return 0;

    case WM_AG_DONE:
        AgOnDone((const wchar_t *)lp);
        return 0;
    case WM_HOTKEY:
        if (wp == HOTKEY_SHOW) {
            if (IsWindowVisible(hwnd) && IsIconic(hwnd) == FALSE) {
                /* remember maximized state, then hide */
                WINDOWPLACEMENT wpl;
                wpl.length = sizeof(wpl);
                GetWindowPlacement(hwnd, &wpl);
                ShowWindow(hwnd, SW_HIDE);
            } else {
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
                if (g_view != VIEW_PREVIEW) SetFocus(g_edit);
            }
        }
        return 0;

    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        CreateUiFonts();
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fontEdit, TRUE);
        RECT *rc = (RECT *)lp;
        SetWindowPos(hwnd, NULL, rc->left, rc->top,
                     rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutChildren();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_CLOSE:
        /* tray-resident: closing the window just hides it; the real exit
         * path is the tray menu's quit item */
        if (g_trayOn) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        if (!ConfirmDiscard()) return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_TRAYICON:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                ShowWindow(hwnd, SW_HIDE);
            } else {
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
                if (g_view != VIEW_PREVIEW) SetFocus(g_edit);
            }
        } else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            /* tray context menu: show / quit */
            POINT pt;
            GetCursorPos(&pt);
            HMENU pm = CreatePopupMenu();
            AppendMenuW(pm, MF_STRING, IDM_TRAY_SHOW, L"打开 MDLite");
            AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
            AppendMenuW(pm, MF_STRING, IDM_TRAY_QUIT, L"退出");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(pm, TPM_RIGHTBUTTON | TPM_RETURNCMD
                                          | TPM_NONOTIFY,
                                     pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(pm);
            if (cmd == IDM_TRAY_SHOW) {
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
                if (g_view != VIEW_PREVIEW) SetFocus(g_edit);
            } else if (cmd == IDM_TRAY_QUIT) {
                if (ConfirmDiscard()) DestroyWindow(hwnd);
            }
        }
        return 0;

    case WM_DESTROY:
        if (g_trayOn) Shell_NotifyIconW(NIM_DELETE, &g_nid);
        UnregisterHotKey(hwnd, HOTKEY_SHOW);
        SaveSettings();
        md_free(&g_doc);
        if (g_hRichEd) FreeLibrary(g_hRichEd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* settings dialog (hotkey + autosave)                                  */
/* ------------------------------------------------------------------ */

static HWND   g_setDlg;
static BOOL   g_ocInstalling;   /* opencode install thread running */

/* run a hidden console command, capture stdout+stderr into out.
 * returns the exit code, or 0xFFFFFFFF when the process cannot start */
static DWORD RunCmdHide(const wchar_t *cmdline, wchar_t *out, int outCch)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (out && outCch > 0) {
        if (!CreatePipe(&rd, &wr, &sa, 0)) return 0xFFFFFFFF;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    wchar_t cmd[1024];
    lstrcpynW(cmd, cmdline, 1024);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        if (rd) { CloseHandle(rd); CloseHandle(wr); }
        return 0xFFFFFFFF;
    }
    if (wr) CloseHandle(wr);
    if (out && rd) {
        DWORD n = 0, total = 0;
        char buf[1024];
        while (total < (DWORD)outCch - 1
               && ReadFile(rd, buf, sizeof(buf), &n, NULL) && n > 0) {
            if (total + n > (DWORD)outCch - 1) n = (DWORD)outCch - 1 - total;
            for (DWORD i = 0; i < n; i++) out[total + i] = (wchar_t)buf[i];
            total += n;
        }
        out[total] = 0;
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, 300000);   /* 5 min cap */
    DWORD code = 0xFFFFFFFF;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

/* locate opencode on PATH (opencode.exe from the official installer,
 * opencode.cmd from npm global installs) */
static BOOL DetectOpenCode(wchar_t *path, int cch)
{
    wchar_t found[MAX_PATH];
    DWORD n = SearchPathW(NULL, L"opencode.exe", NULL, MAX_PATH, found, NULL);
    if (n == 0 || n >= MAX_PATH)
        n = SearchPathW(NULL, L"opencode.cmd", NULL, MAX_PATH, found, NULL);
    if (n == 0 || n >= MAX_PATH) return FALSE;
    lstrcpynW(path, found, cch);
    return TRUE;
}

/* background installer: npm first, official PowerShell script as
 * fallback; posts WM_APP+77 back to the settings dialog when done */
static DWORD WINAPI InstallOpenCodeThread(LPVOID param)
{
    (void)param;
    HWND dlg = g_setDlg;
    wchar_t found[MAX_PATH];
    int how = 0;
    if (SearchPathW(NULL, L"npm.exe", NULL, MAX_PATH, found, NULL)
        || SearchPathW(NULL, L"npm.cmd", NULL, MAX_PATH, found, NULL))
        how = 1;
    else
        how = 2;    /* PowerShell ships with every Windows install */
    BOOL ok = FALSE;
    if (how == 1)
        ok = RunCmdHide(L"cmd.exe /c npm install -g opencode-ai", NULL, 0) == 0;
    else if (how == 2)
        ok = RunCmdHide(L"powershell.exe -NoProfile -Command "
                        L"\"irm https://opencode.ai/install.ps1 | iex\"",
                        NULL, 0) == 0;
    if (dlg) PostMessageW(dlg, WM_APP + 77, ok ? 1 : 0, how);
    return 0;
}
static HFONT  g_setFont;            /* dialog text font (CJK-safe face) */
static HWND   g_hkEdit;
static WNDPROC g_hkProc;
static UINT   g_dlgMod, g_dlgVk;   /* dialog-local hotkey state */
/* ---- vault folder: fixed scope for tree / graph / orphans ---- */
static wchar_t g_vaultDir[MAX_PATH];

static void ApplyVault(void)
{
    TreeSetVault(g_vaultDir);
    TreeSync(g_path[0] ? g_path : NULL);
    GraphBuild();
    GraphResetView();
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static BOOL PickVaultFolder(HWND parent, wchar_t *out, int cch)
{
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = parent;
    bi.lpszTitle = L"选择笔记库文件夹（文件树、知识图谱与孤儿笔记的扫描范围）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return FALSE;
    BOOL ok = SHGetPathFromIDListW(pidl, out);
    CoTaskMemFree(pidl);
    return ok;
}

static int    g_dlgAuto;          /* autosave seconds (dialog-local) */
static int    g_dlgHist;          /* history cap (dialog-local) */
static int    g_dlgDone;           /* 1=ok 2=cancel */

static void HkText(wchar_t *buf, int cch, UINT mod, UINT vk)
{
    if (vk == 0) { lstrcpynW(buf, L"未设置（按 Backspace 清除）", cch); return; }
    buf[0] = 0;
    if (mod & MOD_CONTROL) lstrcpynW(buf + lstrlenW(buf), L"Ctrl+",
                                     cch - lstrlenW(buf));
    if (mod & MOD_ALT) lstrcpynW(buf + lstrlenW(buf), L"Alt+",
                                 cch - lstrlenW(buf));
    if (mod & MOD_SHIFT) lstrcpynW(buf + lstrlenW(buf), L"Shift+",
                                   cch - lstrlenW(buf));
    wchar_t key[8] = L"";
    if (vk == VK_SPACE) {
        lstrcpynW(key, L"Space", 8);
    } else if (vk >= 'A' && vk <= 'Z') {
        key[0] = (wchar_t)vk; key[1] = 0;
    } else if (vk >= '0' && vk <= '9') {
        key[0] = (wchar_t)vk; key[1] = 0;
    } else if (vk >= VK_F1 && vk <= VK_F12) {
        wsprintfW(key, L"F%d", vk - VK_F1 + 1);
    } else {
        wsprintfW(key, L"0x%02X", vk);
    }
    lstrcpynW(buf + lstrlenW(buf), key, cch - lstrlenW(buf));
}

static LRESULT CALLBACK HkEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (wp == VK_RETURN || wp == VK_TAB)
            return CallWindowProcW(g_hkProc, h, msg, wp, lp);
        UINT mod = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) & 0x8000)    mod |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) & 0x8000)   mod |= MOD_SHIFT;
        if (wp == VK_BACK) {
            g_dlgVk = 0;
            g_dlgMod = 0;
        } else if (wp == VK_CONTROL || wp == VK_MENU || wp == VK_SHIFT
                   || wp == VK_LWIN) {
            return 0; /* bare modifiers wait for the real key */
        } else if (mod) {
            g_dlgVk = (UINT)wp;
            g_dlgMod = mod;
        } else {
            return 0;
        }
        wchar_t txt[64];
        HkText(txt, 64, g_dlgMod, g_dlgVk);
        SetWindowTextW(h, txt);
        return 0;
    }
    if (msg == WM_CHAR) return 0; /* swallow typed text */
    return CallWindowProcW(g_hkProc, h, msg, wp, lp);
}

static LRESULT CALLBACK SetDlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        /* CJK-safe face for dialog text: SimHei ships with Windows and
         * carries CJK natively; wine maps SimHei to a face without CJK
         * glyphs, so use WenQuanYi Micro Hei there instead. */
        LOGFONTW lf;
        ZeroMemory(&lf, sizeof(lf));
        lf.lfHeight = -MulDiv(13, g_dpi, 96);
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lstrcpynW(lf.lfFaceName,
                  WineEnv() ? UiFaceName() : L"SimHei",
                  LF_FACESIZE);
        HFONT f = CreateFontIndirectW(&lf);
        g_setFont = f;
        struct { const wchar_t *cls, *txt; DWORD sty; int id, x, y, w, hh; }
        items[] = {
            /* -- card 1: general -- */
            { L"STATIC",  L"全局热键（呼出 / 隐藏窗口）", SS_LEFT,
              0, 24, 32, 300, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_READONLY | WS_TABSTOP,
              101, 24, 54, 200, 26 },
            { L"BUTTON",  L"清除", BS_OWNERDRAW | WS_TABSTOP,
              102, 232, 52, 64, 28 },
            { L"STATIC",  L"自动保存间隔(秒)", SS_LEFT,
              0, 24, 98, 200, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_NUMBER | WS_TABSTOP,
              104, 24, 120, 70, 26 },
            { L"STATIC",  L"0 = 关闭", SS_LEFT,
              0, 102, 126, 70, 18 },
            { L"STATIC",  L"历史保留条数", SS_LEFT,
              0, 230, 98, 130, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_NUMBER | WS_TABSTOP,
              105, 230, 120, 70, 26 },
            { L"STATIC",  L"1-1000", SS_LEFT,
              0, 308, 126, 70, 18 },
            { L"BUTTON",  L"回车继承缩进", BS_AUTOCHECKBOX | WS_TABSTOP,
              111, 24, 142, 140, 22 },
            /* -- card 2: AI -- */
            { L"STATIC",  L"Base URL", SS_LEFT, 0, 24, 194, 70, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
              200, 24, 216, 330, 26 },
            { L"STATIC",  L"模型", SS_LEFT, 0, 24, 252, 60, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
              201, 24, 274, 180, 26 },
            { L"STATIC",  L"超时(秒)", SS_LEFT, 0, 250, 252, 70, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_NUMBER | WS_TABSTOP,
              204, 250, 274, 60, 26 },
            { L"STATIC",  L"API Key", SS_LEFT, 0, 24, 310, 70, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD
                              | WS_TABSTOP,
              202, 24, 332, 330, 26 },
            { L"STATIC",  L"System Prompt", SS_LEFT, 0, 24, 368, 120, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_MULTILINE | ES_WANTRETURN
                              | WS_VSCROLL | WS_TABSTOP,
              203, 24, 390, 398, 44 },
            { L"STATIC",  L"上下文轮数", SS_LEFT, 0, 24, 444, 90, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_NUMBER | WS_TABSTOP,
              208, 24, 466, 60, 26 },
            { L"STATIC",  L"携带最近 N 轮问答，0 = 单轮", SS_LEFT,
              0, 96, 472, 210, 18 },
            /* -- card 3: Agent -- */
            { L"STATIC",  L"Open Code 路径", SS_LEFT,
              0, 24, 538, 100, 18 },
            { L"BUTTON",  L"检测 / 一键安装", BS_PUSHBUTTON | WS_TABSTOP,
              209, 130, 536, 134, 22 },
            { L"EDIT",    L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
              205, 24, 560, 264, 26 },
            { L"STATIC",  L"超时(秒)", SS_LEFT, 0, 300, 538, 70, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_NUMBER | WS_TABSTOP,
              207, 300, 560, 60, 26 },
            { L"STATIC",  L"Agent 提示词", SS_LEFT, 0, 24, 596, 130, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_MULTILINE | ES_WANTRETURN
                              | WS_VSCROLL | WS_TABSTOP,
              206, 24, 618, 398, 44 },
            /* -- card 4: vault -- */
            { L"STATIC",  L"笔记库文件夹（文件树 / 知识图谱 / 孤儿笔记的固定范围）",
              SS_LEFT, 0, 24, 706, 390, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_AUTOHSCROLL | ES_READONLY
                              | WS_TABSTOP,
              210, 24, 728, 300, 26 },
            { L"BUTTON",  L"更改…", BS_OWNERDRAW | WS_TABSTOP,
              212, 332, 728, 100, 26 },
            { L"BUTTON",  L"跟随文档", BS_OWNERDRAW | WS_TABSTOP,
              213, 24, 762, 100, 26 },
            /* -- footer buttons -- */
            { L"BUTTON",  L"确定", BS_OWNERDRAW | BS_DEFPUSHBUTTON
                              | WS_TABSTOP,
              108, 238, 812, 88, 32 },
            { L"BUTTON",  L"取消", BS_OWNERDRAW | WS_TABSTOP,
              109, 334, 812, 88, 32 },
        };
        const int NITEMS = (int)(sizeof(items) / sizeof(items[0]));
        for (int i = 0; i < NITEMS; i++) {
            HWND c = CreateWindowExW(0, items[i].cls, items[i].txt,
                WS_CHILD | WS_VISIBLE | items[i].sty,
                SC(items[i].x), SC(items[i].y),
                SC(items[i].w), SC(items[i].hh),
                h, (HMENU)(INT_PTR)items[i].id, NULL, NULL);
            SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        }
        g_hkEdit = GetDlgItem(h, 101);
        g_hkProc = (WNDPROC)SetWindowLongPtrW(g_hkEdit, GWLP_WNDPROC,
                                              (LONG_PTR)HkEditProc);
        wchar_t anum[12];
        wsprintfW(anum, L"%d", g_dlgAuto);
        SetDlgItemTextW(h, 104, anum);
        wchar_t hnum[12];
        wsprintfW(hnum, L"%d", g_dlgHist);
        SetDlgItemTextW(h, 105, hnum);
        SetDlgItemTextW(h, 200, g_aiBase);
        SetDlgItemTextW(h, 201, g_aiModel);
        SetDlgItemTextW(h, 202, g_aiKey);
        SetDlgItemTextW(h, 203, g_aiSys);
        wchar_t num[12];
        wsprintfW(num, L"%d", g_aiTimeout ? g_aiTimeout : 10);
        SetDlgItemTextW(h, 204, num);
        SetDlgItemTextW(h, 205, g_agPath);
        SetDlgItemTextW(h, 206, g_agSys);
        wchar_t gnum[12];
        wsprintfW(gnum, L"%d", g_agTimeout ? g_agTimeout : 60);
        SetDlgItemTextW(h, 207, gnum);
        wchar_t rnum[12];
        wsprintfW(rnum, L"%d", g_aiRounds);
        SetDlgItemTextW(h, 208, rnum);
        CheckDlgButton(h, 111, g_indentRet ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(h, 210, g_vaultDir);
        wchar_t txt[64];
        HkText(txt, 64, g_dlgMod, g_dlgVk);
        SetWindowTextW(g_hkEdit, txt);
        SetFocus(g_hkEdit);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        static const struct { const wchar_t *title; RECT rc; } cards[] = {
            { L"常规",                        { 12,  12, 434, 156 } },
            { L"AI 助手 · 行首 / 调用",        { 12, 170, 434, 500 } },
            { L"Agent · 行首 // 调用",         { 12, 514, 434, 674 } },
            { L"笔记库",                        { 12, 688, 434, 796 } },
        };
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i < 3; i++) {
            RECT cr = cards[i].rc;
            cr.left = SC(cr.left); cr.top = SC(cr.top);
            cr.right = SC(cr.right); cr.bottom = SC(cr.bottom);
            DrawRoundRect(dc, &cr, SC(10), RGB(0xF7, 0xF8, 0xFA),
                          RGB(0xE5, 0xE7, 0xEB), 1);
            HFONT ob = (HFONT)SelectObject(dc, g_fontHeaderBold);
            SetTextColor(dc, RGB(0x1D, 0x1D, 0x1F));
            TextOutW(dc, SC(24), SC(cards[i].rc.top + 2),
                     cards[i].title, lstrlenW(cards[i].title));
            SelectObject(dc, ob);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x55, 0x5F, 0x6E));
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lp;
        if (!d || d->CtlType != ODT_BUTTON) break;
        BOOL sel = (d->itemState & ODS_SELECTED) != 0;
        const wchar_t *label = (d->CtlID == 102) ? L"清除"
                               : (d->CtlID == 212) ? L"更改…"
                               : (d->CtlID == 213) ? L"跟随文档"
                               : (d->CtlID == 108) ? L"确定"
                               : L"取消";
        COLORREF fill, txtc, brd;
        if (d->CtlID == 108) {           /* primary */
            fill = sel ? RGB(0x00, 0x62, 0xCC) : COL_ACCENT;
            txtc = RGB(255, 255, 255);
            brd  = COL_ACCENT;
        } else if (d->CtlID == 102) {    /* small accent */
            fill = sel ? RGB(0xE8, 0xF0, 0xFE) : RGB(255, 255, 255);
            txtc = COL_ACCENT;
            brd  = RGB(0xB3, 0xD1, 0xFF);
        } else {                         /* secondary */
            fill = sel ? RGB(0xF0, 0xF0, 0xF4) : RGB(255, 255, 255);
            txtc = RGB(0x55, 0x5F, 0x6E);
            brd  = RGB(0xD2, 0xD2, 0xD7);
        }
        DrawRoundRect(d->hDC, &d->rcItem, SC(8), fill, brd, 1);
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, txtc);
        HFONT ob = (HFONT)SelectObject(d->hDC, g_fontHeader);
        RECT tr = d->rcItem;
        DrawTextW(d->hDC, label, lstrlenW(label), &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(d->hDC, ob);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 209) {    /* detect / one-click install opencode */
            wchar_t path[512];
            if (DetectOpenCode(path, 512)) {
                MessageBoxW(h, path, L"已检测到 opencode",
                            MB_OK | MB_ICONINFORMATION);
            } else if (!g_ocInstalling) {
                if (MessageBoxW(h,
                    L"未检测到 opencode。是否现在自动安装？\r\n\r\n"
                    L"优先使用 npm 全局安装，否则使用官方 PowerShell 脚本。\r\n"
                    L"安装过程约 1-3 分钟，完成后自动填入路径。",
                    APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    g_ocInstalling = TRUE;
                    HWND btn = GetDlgItem(h, 209);
                    SetWindowTextW(btn, L"安装中…");
                    EnableWindow(btn, FALSE);
                    HANDLE t = CreateThread(NULL, 0, InstallOpenCodeThread,
                                            NULL, 0, NULL);
                    if (t) CloseHandle(t);
                    else {
                        g_ocInstalling = FALSE;
                        SetWindowTextW(btn, L"检测 / 一键安装");
                        EnableWindow(btn, TRUE);
                    }
                }
            }
            return 0;
        }
        if (id == 212) {    /* pick vault folder */
            wchar_t pick[MAX_PATH];
            if (PickVaultFolder(h, pick, MAX_PATH))
                SetDlgItemTextW(h, 210, pick);
            return 0;
        }
        if (id == 213) {    /* clear vault: follow the current document */
            SetDlgItemTextW(h, 210, L"");
            return 0;
        }
        if (id == 102) { /* clear hotkey */
            g_dlgVk = 0;
            g_dlgMod = 0;
            wchar_t txt[64];
            HkText(txt, 64, 0, 0);
            SetWindowTextW(g_hkEdit, txt);
            SetFocus(g_hkEdit);
            return 0;
        }
        if (id == 108) {
            /* commit AI + agent fields while the dialog is still alive */
            GetDlgItemTextW(h, 200, g_aiBase, 256);
            GetDlgItemTextW(h, 201, g_aiModel, 128);
            GetDlgItemTextW(h, 202, g_aiKey, 256);
            GetDlgItemTextW(h, 203, g_aiSys, 1024);
            wchar_t num[12] = L"";
            GetDlgItemTextW(h, 204, num, 12);
            int t = num[0] ? _wtoi(num) : 10;
            if (t < 1) t = 1;
            if (t > 300) t = 300;
            g_aiTimeout = t;
            GetDlgItemTextW(h, 205, g_agPath, 512);
            GetDlgItemTextW(h, 206, g_agSys, 1024);
            wchar_t gnum[12] = L"";
            GetDlgItemTextW(h, 207, gnum, 12);
            int gt = gnum[0] ? _wtoi(gnum) : 60;
            if (gt < 1) gt = 1;
            if (gt > 3600) gt = 3600;
            g_agTimeout = gt;
            wchar_t rnum[12] = L"";
            GetDlgItemTextW(h, 208, rnum, 12);
            int rt = rnum[0] ? _wtoi(rnum) : 2;
            if (rt < 0) rt = 0;
            if (rt > 10) rt = 10;
            g_aiRounds = rt;
            wchar_t anum[12] = L"";
            GetDlgItemTextW(h, 104, anum, 12);
            int a = anum[0] ? _wtoi(anum) : 0;
            if (a < 0) a = 0;
            if (a > 86400) a = 86400;
            g_dlgAuto = a;
            wchar_t hnum[12] = L"";
            GetDlgItemTextW(h, 105, hnum, 12);
            int hm = hnum[0] ? _wtoi(hnum) : 20;
            if (hm < 1) hm = 1;
            if (hm > 1000) hm = 1000;
            g_dlgHist = hm;
            g_indentRet = IsDlgButtonChecked(h, 111) == BST_CHECKED;
            wchar_t vault[MAX_PATH] = L"";
            GetDlgItemTextW(h, 210, vault, MAX_PATH);
            CfgSetStr(L"Vault", vault);
            lstrcpynW(g_vaultDir, vault, MAX_PATH);
            g_dlgDone = 1;
            DestroyWindow(h);
            PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
            return 0;
        }
        if (id == 109 || id == IDCANCEL) {
            g_dlgDone = 2;
            DestroyWindow(h);
            PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
            return 0;
        }
        return 0;
    }
    case WM_APP + 77: {   /* opencode install thread finished */
        g_ocInstalling = FALSE;
        HWND btn = GetDlgItem(h, 209);
        if (btn) {
            SetWindowTextW(btn, L"检测 / 一键安装");
            EnableWindow(btn, TRUE);
        }
        wchar_t path[512];
        if (DetectOpenCode(path, 512)) {
            SetDlgItemTextW(h, 205, path);
            MessageBoxW(h, path, L"opencode 安装完成",
                        MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(h,
                wp ? L"安装程序已执行，但未检测到 opencode。\r\n"
                     L"新装的 PATH 可能需要重启 MDLite 后生效。"
                   : L"安装失败：系统中未找到 npm 或 PowerShell。",
                APP_NAME, MB_OK | MB_ICONWARNING);
        }
        return 0;
    }
    case DM_GETDEFID:
        return MAKELRESULT(108, DC_HASDEFID); /* OK button */
    case WM_CLOSE:
        g_dlgDone = 2;
        DestroyWindow(h);
        PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
        return 0;
    case WM_DESTROY:
        if (g_setFont) { DeleteObject(g_setFont); g_setFont = NULL; }
        g_setDlg = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}


static void ShowSettings(void)
{
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = SetDlgProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"MDLiteSet";
        if (!RegisterClassW(&wc)) return;
        registered = TRUE;
    }
    if (g_setDlg) { SetFocus(g_setDlg); return; }

    g_dlgMod = g_hotMod;
    g_dlgVk = g_hotVk;
    g_dlgAuto = g_autoSec;
    g_dlgHist = g_histMax;
    g_dlgDone = 0;

    RECT rcMain;
    GetWindowRect(g_hwnd, &rcMain);
    int dw = SC(446), dh = SC(860)
             + GetSystemMetrics(SM_CYCAPTION)
             + GetSystemMetrics(SM_CYFIXEDFRAME) * 2;
    int x = rcMain.left + (rcMain.right - rcMain.left - dw) / 2;
    int y = rcMain.top + (rcMain.bottom - rcMain.top - dh) / 2;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (y < wa.top) y = wa.top;
    if (y + dh > wa.bottom) y = wa.bottom - dh;
    if (y < wa.top) y = wa.top;   /* taller than workarea: top-align */
    if (x < wa.left) x = wa.left;
    g_setDlg = CreateWindowExW(WS_EX_TOOLWINDOW, L"MDLiteSet",
        L"设置", WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
        x, y, dw, dh, g_hwnd, NULL, NULL, NULL);
    if (!g_setDlg) return;

    EnableWindow(g_hwnd, FALSE);
    SetActiveWindow(g_setDlg);
    MSG msg;
    while (IsWindow(g_setDlg) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(g_setDlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);
    g_setDlg = NULL;

    if (g_dlgDone == 1) {
        g_hotMod = g_dlgMod;
        g_hotVk = g_dlgVk;
        g_autoSec = g_dlgAuto;
        g_histMax = g_dlgHist;
        ApplyHotKey(FALSE);
        ApplyAutoSave();
        SaveSettings();
        ApplyVault();   /* re-root tree / graph / orphans when vault changed */
    }
}

/* ------------------------------------------------------------------ */
/* settings (registry)                                                  */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* config backend: registry or portable ini (exe-dir mdlite.ini)       */
/* ------------------------------------------------------------------ */

static BOOL    g_portable;
static wchar_t g_iniPath[MAX_PATH];

static void CfgInit(void)
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    wchar_t *slash = exe;
    for (wchar_t *p = exe; *p; p++)
        if (*p == L'\\' || *p == L'/') slash = p + 1;
    *slash = 0;
    lstrcpynW(g_iniPath, exe, MAX_PATH - 16);
    lstrcatW(g_iniPath, L"mdlite.ini");
    if (GetFileAttributesW(g_iniPath) != INVALID_FILE_ATTRIBUTES)
        g_portable = TRUE;
}

static BOOL CfgHaveKey(const wchar_t *name)
{
    if (g_portable) {
        wchar_t buf[8];
        buf[0] = 0;
        GetPrivateProfileStringW(L"MDLite", name, L"\x1", buf, 8,
                                 g_iniPath);
        return buf[0] != 0 && buf[0] != 1;
    }
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MDLite", 0,
                      KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return FALSE;
    BOOL ok = RegQueryValueExW(k, name, NULL, NULL, NULL, NULL)
              == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

static DWORD CfgGetDword(const wchar_t *name, DWORD def)
{
    if (g_portable)
        return (DWORD)GetPrivateProfileIntW(L"MDLite", name,
                                            (INT)def, g_iniPath);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MDLite", 0,
                      KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return def;
    DWORD v = def, size = sizeof(DWORD);
    if (RegQueryValueExW(k, name, NULL, NULL, (BYTE *)&v, &size)
        != ERROR_SUCCESS)
        v = def;
    RegCloseKey(k);
    return v;
}

static BOOL CfgGetStr(const wchar_t *name, wchar_t *out, int cch)
{
    if (g_portable) {
        out[0] = 0;
        GetPrivateProfileStringW(L"MDLite", name, L"", out, cch,
                                 g_iniPath);
        return out[0] != 0;
    }
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MDLite", 0,
                      KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return FALSE;
    DWORD bsz = (DWORD)(cch * sizeof(wchar_t));
    BOOL ok = RegQueryValueExW(k, name, NULL, NULL,
                               (BYTE *)out, &bsz) == ERROR_SUCCESS
              && bsz >= sizeof(wchar_t);
    if (ok) {
        int nch = (int)(bsz / sizeof(wchar_t)) - 1;
        if (nch > cch - 1) nch = cch - 1;
        if (nch < 0) nch = 0;
        out[nch] = 0;
    }
    RegCloseKey(k);
    return ok;
}

static void CfgSetDword(const wchar_t *name, DWORD v)
{
    if (g_portable) {
        wchar_t buf[16];
        wsprintfW(buf, L"%u", v);
        WritePrivateProfileStringW(L"MDLite", name, buf, g_iniPath);
        return;
    }
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\MDLite", 0, NULL,
                        0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD,
                       (const BYTE *)&v, sizeof(DWORD));
        RegCloseKey(k);
    }
}

static void CfgSetStr(const wchar_t *name, const wchar_t *v)
{
    if (g_portable) {
        WritePrivateProfileStringW(L"MDLite", name, v, g_iniPath);
        return;
    }
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\MDLite", 0, NULL,
                        0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ,
                       (const BYTE *)v,
                       (DWORD)((lstrlenW(v) + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

/* FNV-1a 32-bit hex of a file path: safe ini key for pin groups */
void PathHash(const wchar_t *path, wchar_t *out /*9 chars*/)
{
    DWORD h = 2166136261u;
    for (const wchar_t *p = path; *p; p++) {
        wchar_t c = *p;
        if (c >= L'A' && c <= L'Z') c += 32;   /* case-insensitive */
        h ^= (DWORD)c;
        h *= 16777619u;
    }
    wsprintfW(out, L"%08X", h);
}

/* ------------------------------------------------------------------ */
/* most-recently-used file list                                        */
/* ------------------------------------------------------------------ */

static void MruLoad(void)
{
    g_mruN = 0;
    for (int i = 0; i < MRU_MAX; i++) {
        wchar_t key[8];
        wsprintfW(key, L"MRU%d", i);
        if (CfgGetStr(key, g_mru[g_mruN], MAX_PATH))
            g_mruN++;
    }
}

static void MruSave(void)
{
    for (int i = 0; i < MRU_MAX; i++) {
        wchar_t key[8];
        wsprintfW(key, L"MRU%d", i);
        if (i < g_mruN)
            CfgSetStr(key, g_mru[i]);
        else if (CfgHaveKey(key))
            CfgSetStr(key, L"");
    }
}

static void MruPush(const wchar_t *path)
{
    int at = -1;
    for (int i = 0; i < g_mruN; i++)
        if (lstrcmpiW(g_mru[i], path) == 0) { at = i; break; }
    if (at < 0 && g_mruN < MRU_MAX) at = g_mruN++;
    if (at < 0) at = MRU_MAX - 1;               /* full: drop oldest */
    for (int i = at; i > 0; i--)
        lstrcpynW(g_mru[i], g_mru[i - 1], MAX_PATH);
    lstrcpynW(g_mru[0], path, MAX_PATH);
}

static void MruRemove(int idx)
{
    if (idx < 0 || idx >= g_mruN) return;
    for (int i = idx; i < g_mruN - 1; i++)
        lstrcpynW(g_mru[i], g_mru[i + 1], MAX_PATH);
    g_mruN--;
    MruSave();
}

static void SaveSettings(void)
{
    WINDOWPLACEMENT wp;
    wp.length = sizeof(wp);
    GetWindowPlacement(g_hwnd, &wp);
    CfgSetDword(L"X", (DWORD)wp.rcNormalPosition.left);
    CfgSetDword(L"Y", (DWORD)wp.rcNormalPosition.top);
    CfgSetDword(L"W", (DWORD)(wp.rcNormalPosition.right
                              - wp.rcNormalPosition.left));
    CfgSetDword(L"H", (DWORD)(wp.rcNormalPosition.bottom
                              - wp.rcNormalPosition.top));
    CfgSetDword(L"Max",
                (DWORD)(wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0));
    CfgSetDword(L"Zoom", (DWORD)g_zoom);
    CfgSetDword(L"AutoSec", (DWORD)g_autoSec);
    CfgSetDword(L"HotMod", (DWORD)g_hotMod);
    CfgSetDword(L"HotVk", (DWORD)g_hotVk);
    CfgSetDword(L"HistMax", (DWORD)g_histMax);
    CfgSetDword(L"AiTimeout", (DWORD)g_aiTimeout);
    CfgSetDword(L"AgTimeout", (DWORD)g_agTimeout);
    CfgSetDword(L"AiRounds", (DWORD)g_aiRounds);
    CfgSetDword(L"Topmost", (DWORD)(g_topmost ? 1 : 0));
    CfgSetDword(L"TreeBar", (DWORD)(TreeShown() ? 1 : 0));
    CfgSetDword(L"IndentRet", (DWORD)(g_indentRet ? 1 : 0));
    static const struct { const wchar_t *name; const wchar_t *v; }
    strs[] = { { L"AiBase", g_aiBase }, { L"AiModel", g_aiModel },
               { L"AiKey", g_aiKey },   { L"AiSys", g_aiSys },
               { L"AgentPath", g_agPath }, { L"AgentSys", g_agSys } };
    for (int i = 0; i < 6; i++)
        CfgSetStr(strs[i].name, strs[i].v);
    MruSave();
}

/* hotkey helpers - shared with the settings dialog */
static void ApplyHotKey(BOOL quiet)
{
    UnregisterHotKey(g_hwnd, HOTKEY_SHOW);
    if (g_hotVk == 0) return; /* disabled */
    if (!RegisterHotKey(g_hwnd, HOTKEY_SHOW, g_hotMod, g_hotVk) && !quiet)
        MessageBoxW(g_hwnd,
                    L"注册全局热键失败，可能与其他程序冲突。\n可在设置中更换组合。",
                    APP_NAME, MB_ICONWARNING);
}

static void ApplyAutoSave(void)
{
    KillTimer(g_hwnd, TIMER_AUTO);
    if (g_autoSec > 0)
        SetTimer(g_hwnd, TIMER_AUTO, g_autoSec * 1000, NULL);
    SetTimer(g_hwnd, TIMER_UITICK, 500, NULL);   /* status bar refresh */
}

static void LoadExtraSettings(void)
{
    DWORD v;
    v = CfgGetDword(L"AutoSec", (DWORD)g_autoSec);
    if (v <= 86400) g_autoSec = (int)v;
    v = CfgGetDword(L"HistMax", (DWORD)g_histMax);
    if (v >= 1 && v <= 1000) g_histMax = (int)v;
    g_hotMod = CfgGetDword(L"HotMod", (DWORD)g_hotMod);
    g_hotVk = CfgGetDword(L"HotVk", (DWORD)g_hotVk);
    v = CfgGetDword(L"AiTimeout", (DWORD)g_aiTimeout);
    if (v >= 1 && v <= 300) g_aiTimeout = (int)v;
    v = CfgGetDword(L"AgTimeout", (DWORD)g_agTimeout);
    if (v >= 1 && v <= 3600) g_agTimeout = (int)v;
    v = CfgGetDword(L"AiRounds", (DWORD)g_aiRounds);
    if (v <= 10) g_aiRounds = (int)v;
    g_topmost = CfgGetDword(L"Topmost", 0) != 0;
    TreeSetShown(CfgGetDword(L"TreeBar", 0) != 0);
    g_indentRet = CfgGetDword(L"IndentRet", 1) != 0;
    static const struct { const wchar_t *name; wchar_t *v; int cch; }
    strs[] = { { L"AiBase", g_aiBase, 256 }, { L"AiModel", g_aiModel, 128 },
               { L"AiKey", g_aiKey, 256 },   { L"AiSys", g_aiSys, 1024 },
               { L"AgentPath", g_agPath, 512 },
               { L"AgentSys", g_agSys, 1024 } };
    for (int i = 0; i < 6; i++)
        CfgGetStr(strs[i].name, strs[i].v, strs[i].cch);
    MruLoad();
}

/* draft file path: <exe dir>\\mdlite\\draft.txt */
static void DraftPath(wchar_t *out, int cch)
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) { out[0] = 0; return; }
    wchar_t *slash = exe;
    for (wchar_t *p = exe; *p; p++)
        if (*p == L'\\' || *p == L'/') slash = p;
    *++slash = 0;
    lstrcpynW(out, exe, cch);
    lstrcatW(out, L"mdlite");
    CreateDirectoryW(out, NULL);
    lstrcatW(out, L"\\draft.txt");
}

/* write the current editor content to the draft file. returns FALSE on failure */
static BOOL WriteDraft(void)
{
    if (!g_dirty || !g_hwnd) return TRUE;
    int len = GetWindowTextLengthW(g_edit);
    if (len <= 0) { DeleteFileW(g_path); return TRUE; }
    wchar_t *wbuf = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
    if (!wbuf) return FALSE;
    GetWindowTextW(g_edit, wbuf, len + 1);
    int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, NULL, 0, NULL, NULL);
    if (u8len <= 0) { free(wbuf); return FALSE; }
    char *u8 = (char *)malloc((size_t)u8len + 1);
    if (!u8) { free(wbuf); return FALSE; }
    WideCharToMultiByte(CP_UTF8, 0, wbuf, len, u8, u8len, NULL, NULL);
    free(wbuf);
    wchar_t dp[MAX_PATH]; DraftPath(dp, MAX_PATH);
    BOOL ok = WriteAllBytes(dp, u8, u8len);
    free(u8);
    if (ok) g_draftSaved = TRUE;
    return ok;
}

/* if a draft exists with content, ask the user to restore it.
 * returns TRUE when a draft was shown (dialog consumed the focus);
 * returns FALSE when no draft or user chose to ignore. */
static BOOL TryRestoreDraft(void)
{
    wchar_t dp[MAX_PATH]; DraftPath(dp, MAX_PATH);
    if (GetFileAttributesW(dp) == INVALID_FILE_ATTRIBUTES) return FALSE;
    HANDLE h = CreateFileW(dp, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD sz = GetFileSize(h, NULL);
    if (sz == 0xFFFFFFFF) { CloseHandle(h); return FALSE; }
    char *raw = (char *)malloc(sz + 1);
    if (!raw) { CloseHandle(h); return FALSE; }
    DWORD nr = 0;
    if (!ReadFile(h, raw, (DWORD)sz, &nr, NULL) || nr != sz) {
        free(raw); CloseHandle(h); return FALSE;
    }
    CloseHandle(h);
    raw[sz] = 0;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, raw, (int)sz, NULL, 0);
    if (wlen < 0) { free(raw); return FALSE; }
    wchar_t *w = (wchar_t *)malloc(((size_t)wlen + 1) * sizeof(wchar_t));
    if (!w) { free(raw); return FALSE; }
    int conv = MultiByteToWideChar(CP_UTF8, 0, (const char *)raw, (int)sz, w, wlen);
    free(raw);
    if (conv != wlen) { free(w); return FALSE; }
    if (wlen <= 0 || wlen > 1 << 20) { free(w); return FALSE; }
    /* non-trivial: more than 8 chars */
    int ui = MessageBoxW(g_hwnd,
        L"发现未保存的草稿，是否恢复？", APP_NAME,
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (ui != IDYES) { free(w); DeleteFileW(dp); return FALSE; }
    if (!g_path[0]) { SetPath(NULL); g_dirty = FALSE; }
    SetWindowTextW(g_edit, w);
    g_dirty = FALSE;
    DeleteFileW(dp);
    UpdateTitle();
    InvalidateRect(g_hwnd, NULL, FALSE);
    free(w);
    return TRUE;
}

static BOOL LoadSettings(RECT *rc, BOOL *maxi)
{
    DWORD v[6];
    static const wchar_t *names[6] = { L"X", L"Y", L"W", L"H", L"Max", L"Zoom" };
    BOOL ok = TRUE;
    for (int i = 0; i < 6; i++) {
        if (!CfgHaveKey(names[i]))
            ok = FALSE;
        v[i] = CfgGetDword(names[i], 0);
    }
    if (!ok) return FALSE;
    if (v[2] < 300 || v[2] > 20000 || v[3] < 200 || v[3] > 20000) return FALSE;
    if ((int)v[0] < -30000 || (int)v[0] > 30000) return FALSE;
    if ((int)v[1] < -30000 || (int)v[1] > 30000) return FALSE;
    if (v[5] > 4) v[5] = 1;
    g_zoom = (int)v[5];
    SetRect(rc, (int)v[0], (int)v[1],
            (int)v[0] + (int)v[2], (int)v[1] + (int)v[3]);
    *maxi = v[4] != 0;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* entry                                                               */
/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdLine, int show)
{
    (void)hPrev;

    CfgInit();   /* portable ini detection before any settings access */
    CfgGetStr(L"Vault", g_vaultDir, MAX_PATH);
    TreeSetVault(g_vaultDir);

    /* single instance: a second launch just surfaces the running one
     * (works for tray-hidden windows too - FindWindow sees hidden) */
    HANDLE mut = CreateMutexW(NULL, FALSE, L"MDLite_SingleInstance");
    if (mut && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(L"MDLiteWnd", NULL);
        if (w) {
            ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
        }
        return 0;
    }

    /* per-monitor dpi awareness when available */
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *SPDA_FN)(HANDLE);
    typedef BOOL (WINAPI *SPDAA_FN)(void);
    if (u32) {
        SPDA_FN f = (SPDA_FN)(void *)GetProcAddress(u32,
                        "SetProcessDpiAwarenessContext");
        SPDAA_FN g = (SPDAA_FN)(void *)GetProcAddress(u32,
                        "SetProcessDPIAware");
        if (f) {
            if (!f((HANDLE)-4) && g) g();  /* PER_MONITOR_AWARE_V2 */
        } else if (g) {
            g();
        }
    }

    RECT savedR;
    BOOL savedMax = FALSE;
    BOOL haveSaved = LoadSettings(&savedR, &savedMax);
    LoadExtraSettings();

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"MDLiteWnd";
    wc.style = CS_HREDRAW | CS_VREDRAW;
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, L"MDLiteWnd",
        APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        haveSaved ? savedR.left : CW_USEDEFAULT,
        haveSaved ? savedR.top : CW_USEDEFAULT,
        haveSaved ? savedR.right - savedR.left : 920,
        haveSaved ? savedR.bottom - savedR.top : 640,
        NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, savedMax ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(hwnd);

    ApplyHotKey(TRUE);   /* no nag on startup */
    ApplyAutoSave();

    /* open file given on command line */
    if (cmdLine && cmdLine[0]) {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(cmdLine, &argc);
        if (argv) {
            if (argc >= 1 && argv[0][0])
                LoadFile(argv[0]);
            LocalFree(argv);
        }
    } else {
        /* no file requested: continue the last untitled session that
         * was auto-saved under mdlite\autosave */
        RestoreLatestSession();
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
