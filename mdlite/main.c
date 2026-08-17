/* MDLite - a tiny markdown editor & viewer
   build: x86_64-w64-mingw32-gcc -Os ... */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <winhttp.h>
#include "markdown.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#define APP_NAME     L"MDLite"
#define UNTITLED     L"未命名"
#define WM_EDITCMD   (WM_USER + 1)
#define WM_AI_CHUNK  (WM_USER + 2)
#define WM_AI_DONE   (WM_USER + 3)
#define WM_AG_CHUNK  (WM_USER + 4)
#define WM_AG_DONE   (WM_USER + 5)
#define WM_TRAYICON  (WM_USER + 6)

#define IDM_OPEN     1001
#define IDM_SAVE     1002
#define IDM_SAVEAS   1003
#define IDM_NEW      1004
#define IDM_TOGGLE   1005
#define IDM_FIND     1006
#define IDM_FINDNEXT 1007
#define IDM_FINDPREV 1008
#define IDM_ZOOM     1009
#define IDM_SETTINGS 1010
#define IDM_TRAY_SHOW  1011
#define IDM_TRAY_QUIT  1012
#define IDM_OUTLINE    1013
#define IDM_COPYHTML   1014
#define IDM_EXPORTHTML 1015
#define IDM_HELP       1016
#define IDM_TOPMOST    1017
#define IDM_PRINT      1018
#define IDM_EXPORTTXT  1019
#define IDM_AI_MENU    1020
#define IDM_MRU_BASE   2000   /* + index, up to 2009 */

/* view states */
#define VIEW_EDIT    0
#define VIEW_SPLIT   1
#define VIEW_PREVIEW 2

/* timers / hotkey ids */
#define TIMER_AUTO   2
#define TIMER_KICK   3
#define TIMER_AICHUNK 4
#define TIMER_UITICK 5
#define HOTKEY_SHOW  1

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

static HWND   g_hwnd;
static HWND   g_edit;
static WNDPROC g_editProc;
static int    g_dpi = 96;

static HFONT  g_fontEdit;
static HFONT  g_fontStatus;
static HFONT  g_fontHeader;
static HFONT  g_fontHeaderBold;
static MDFonts g_fonts;

static BOOL   g_preview;   /* preview engine active (EDIT off, SPLIT/PREVIEW on) */
static int    g_view;     /* VIEW_EDIT / VIEW_SPLIT / VIEW_PREVIEW */
static MDDoc  g_doc;
static int    g_scrollY;
static BOOL   g_splitDrag;  /* dragging the split preview scrollbar */

static wchar_t g_path[MAX_PATH] = L"";
static wchar_t g_name[MAX_PATH] = L"";
static BOOL   g_dirty;

static int    g_hoverId = -1;   /* header hover: 0 open 1 save 2 history 3 settings 10 seg-edit 11 seg-split 12 seg-preview */

/* zoom levels (percent), cycled with Ctrl+wheel */
static const int ZOOMS[5] = { 85, 100, 115, 130, 150 };
static int g_zoom = 1;

/* autosave interval in seconds (0 = off); global hotkey */
static int  g_autoSec = 60;
static int  g_histMax = 20;   /* history entries kept per file */
static UINT g_hotMod = MOD_CONTROL | MOD_SHIFT;
static UINT g_hotVk  = VK_SPACE;

/* history panel state */
static BOOL  g_loading;        /* suppress EN_CHANGE side effects */
static char  g_archivedTree[41]; /* tree already recorded (skip re-archiving) */

/* AI provider config (OpenAI-compatible chat/completions) */
static wchar_t g_aiBase[256]  = L"";
static wchar_t g_aiModel[128] = L"";
static wchar_t g_aiKey[256]   = L"";
static wchar_t g_aiSys[1024]  = L"";
static int     g_aiTimeout    = 10;
static int     g_agTimeout    = 60;   /* agent kill timeout, seconds */
static int     g_aiRounds     = 2;    /* prior Q/A turns sent as context */
static BOOL    g_topmost;             /* keep window on top */
static BOOL    g_indentRet    = TRUE; /* Enter inherits leading blanks */

/* most-recently-used file list */
#define MRU_MAX 10
static wchar_t g_mru[MRU_MAX][MAX_PATH];
static int     g_mruN;

/* rolling in-session AI context (question/answer pairs), newest last */
#define AI_CTX_MAX 10
static wchar_t *g_aiCtxQ[AI_CTX_MAX], *g_aiCtxA[AI_CTX_MAX];
static int      g_aiCtxCnt;
static wchar_t  g_aiLastQ[1024];      /* current turn being streamed */
static wchar_t *g_aiLastA;
static int      g_aiLastALen;

/* AI streaming state */
static volatile BOOL  g_aiBusy;
static volatile LONG  g_aiCancelFlag;
static volatile HANDLE g_aiReq;       /* active WinHTTP request (cancel) */
/* chunk batching: coalesce SSE deltas so the edit control and the view
 * update at a smooth ~15fps instead of jumping on every tiny chunk */
static wchar_t *g_aiPending;   /* heap buffer of pending text */
static int      g_aiPendingLen, g_aiPendingCap;

/* Agent (opencode) config */
static wchar_t g_agPath[512] = L"";   /* opencode.exe full path; "" = PATH */
static wchar_t g_agSys[1024] = L"";   /* agent prompt prepended to question */

/* background task slots: 0 = AI chat, 1 = agent. each slot tracks where
 * to insert (anchored to the question line so concurrent user edits in
 * other places never displace or lose the streamed answer) */
#define N_TASKS 2
typedef struct {
    BOOL     active;
    wchar_t  anchor[2048]; /* question line text (no CRLF) */
    int      insEnd;       /* current insert offset (refreshed via anchor) */
    int      totalLen;     /* streamed chars so far (offset compensation) */
} BgTask;
static BgTask g_tasks[N_TASKS];

/* agent process state */
static volatile BOOL  g_agBusy;
static volatile LONG  g_agCancel;
static volatile HANDLE g_agProc;      /* active agent process (cancel) */
static wchar_t *g_agPending;
static int      g_agPendingLen, g_agPendingCap;

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
static int    g_stBusySec = -1;  /* busy seconds last drawn */

/* task timing (status readout) */
static DWORD  g_aiStartTick, g_agStartTick;

static int SC(int px) { return MulDiv(px, g_dpi, 96); }
static int HeaderH(void) { return SC(48); }
static int StatusH(void) { return SC(26); }

int text_w(HDC hdc, HFONT font, const wchar_t *s, int len);
static void SaveSettings(void);
static void ShowSettings(void);
static void ApplyHotKey(BOOL quiet);
static void ApplyAutoSave(void);
static void MruLoad(void);
static void MruSave(void);
static void MruRemove(int idx);
static void MruPush(const wchar_t *path);
static BOOL ConfirmDiscard(void);
static void DoPlainExport(void);
static void DoPrint(void);
static void SetTopmost(BOOL on);
static BOOL CfgHaveKey(const wchar_t *name);
static DWORD CfgGetDword(const wchar_t *name, DWORD def);
static BOOL CfgGetStr(const wchar_t *name, wchar_t *out, int cch);
static void CfgSetDword(const wchar_t *name, DWORD v);
static void CfgSetStr(const wchar_t *name, const wchar_t *v);
static void PathHash(const wchar_t *path, wchar_t *out);
static void StartAi(const wchar_t *question);
static void ShowSelAiMenu(void);

static void GetBodyRect(RECT *rc)
{
    GetClientRect(g_hwnd, rc);
    rc->top += HeaderH() + (g_findShown ? SC(40) : 0);
    rc->bottom -= StatusH();
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

static void CreateUiFonts(void)
{
    HDC dc = GetDC(g_hwnd);
    if (g_fontEdit)      DeleteObject(g_fontEdit);
    if (g_fontStatus)    DeleteObject(g_fontStatus);
    if (g_fontHeader)    DeleteObject(g_fontHeader);
    if (g_fontHeaderBold) DeleteObject(g_fontHeaderBold);

    int fdpi = MulDiv(g_dpi, ZOOMS[g_zoom], 100); /* text scales, chrome stays */

    const wchar_t *uiFace   = WineEnv() ? L"WenQuanYi Micro Hei" : L"Segoe UI";
    const wchar_t *monoFace = WineEnv() ? L"Noto Sans Mono CJK SC" : L"Consolas";

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
        RECT rcClient;
        GetClientRect(g_hwnd, &rcClient);
        int x = SC(60);
        int w = rcClient.right - SC(60) - SC(170);
        if (w > SC(420)) w = SC(420);
        if (w < SC(120)) w = SC(120);
        MoveWindow(g_findEdit, x, HeaderH() + SC(7), w, SC(26), TRUE);
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
}

static BOOL LoadFile(const wchar_t *path)
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
    if (rd >= 3 && (BYTE)u8[0] == 0xEF && (BYTE)u8[1] == 0xBB
        && (BYTE)u8[2] == 0xBF) {
        off = 3; rd -= 3;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, u8 + off, rd, NULL, 0);
    wchar_t *wbuf = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
    if (!wbuf) { free(u8); return FALSE; }
    MultiByteToWideChar(CP_UTF8, 0, u8 + off, rd, wbuf, wlen);
    wbuf[wlen] = 0;
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
    return TRUE;
}

static BOOL WriteAllBytes(const wchar_t *path, const char *buf, int len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, buf, len, &wr, NULL) && (int)wr == len;
    CloseHandle(h);
    return ok;
}

static BOOL ReadAllBytes(const wchar_t *path, char **buf, int *len)
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
static BOOL BuildHistoryDir(wchar_t *dir, int cch)
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

/* zlib stream with stored (uncompressed) deflate blocks */
static int ZlibStore(BYTE *dst, const BYTE *src, int len)
{
    int o = 0;
    dst[o++] = 0x78;
    dst[o++] = 0x01; /* CMF/FLG, valid zlib header */
    int off = 0;
    while (off < len || (len == 0 && off == 0)) {
        int chunk = len - off;
        if (chunk > 65535) chunk = 65535;
        int final = (off + chunk >= len) && !(len == 0 && off == 0);
        if (len == 0 && off == 0) { final = 1; chunk = 0; }
        dst[o++] = final ? 0x01 : 0x00; /* BFINAL + BTYPE=00 */
        dst[o++] = (BYTE)(chunk & 0xFF);
        dst[o++] = (BYTE)((chunk >> 8) & 0xFF);
        dst[o++] = (BYTE)(~chunk & 0xFF);
        dst[o++] = (BYTE)((~chunk >> 8) & 0xFF);
        if (chunk) { memcpy(dst + o, src + off, chunk); o += chunk; }
        off += chunk;
        if (len == 0) break;
    }
    /* adler32 */
    DWORD a = 1, b = 0;
    for (int i = 0; i < len; i++) {
        a = (a + src[i]) % 65521;
        b = (b + a) % 65521;
    }
    DWORD ad = (b << 16) | a;
    dst[o++] = (BYTE)((ad >> 24) & 0xFF);
    dst[o++] = (BYTE)((ad >> 16) & 0xFF);
    dst[o++] = (BYTE)((ad >> 8) & 0xFF);
    dst[o++] = (BYTE)(ad & 0xFF);
    return o;
}

/* inverse of ZlibStore; returns body length or -1 */
static int ZlibUnstore(const BYTE *src, int srclen, BYTE *dst, int dstcap)
{
    if (srclen < 6) return -1;
    int o = 0;
    int i = 2; /* skip zlib header */
    while (i < srclen - 4) {
        int final = src[i] & 1;
        int type = (src[i] >> 1) & 3;
        if (type != 0) return -1; /* only stored blocks are supported */
        i++;
        int chunk = src[i] | (src[i + 1] << 8);
        int nlen = src[i + 2] | (src[i + 3] << 8);
        if ((chunk ^ 0xFFFF) != nlen) return -1;
        i += 4;
        if (i + chunk > srclen || o + chunk > dstcap) return -1;
        memcpy(dst + o, src + i, chunk);
        o += chunk;
        i += chunk;
        if (final) return o;
    }
    return -1;
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

    wchar_t objDir[MAX_PATH + 16], objPath[MAX_PATH + 32], tmpPath[MAX_PATH + 32];
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
    BYTE *z = (BYTE *)malloc(total + total / 65535 * 5 + 16);
    if (!z) { free(raw); return FALSE; }
    int zlen = ZlibStore(z, raw, total);
    free(raw);

    wsprintfW(tmpPath, L"%s\\tmp_%s", objDir, tailW);
    BOOL ok = FALSE;
    if (WriteAllBytes(tmpPath, (const char *)z, zlen))
        ok = MoveFileExW(tmpPath, objPath, MOVEFILE_REPLACE_EXISTING);
    free(z);
    return ok;
}

/* read loose object, returns malloc'd body and length (header skipped) */
static BOOL ReadLoose(const wchar_t *repo, const char *hex,
                      char **body, int *bodylen)
{
    *body = NULL;
    *bodylen = 0;
    wchar_t path[MAX_PATH + 64];
    wchar_t dirW[8], restW[48];
    AsciiToWide(hex, dirW, 3);
    AsciiToWide(hex + 2, restW, 48);
    wsprintfW(path, L"%s\\objects\\%s\\%s", repo, dirW, restW);
    char *raw = NULL;
    int rawlen = 0;
    if (!ReadAllBytes(path, &raw, &rawlen)) return FALSE;

    BYTE *plain = (BYTE *)malloc(rawlen);
    if (!plain) { free(raw); return FALSE; }
    int plen = ZlibUnstore((const BYTE *)raw, rawlen, plain, rawlen);
    free(raw);
    if (plen < 0) { free(plain); return FALSE; }

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

static BOOL RepoRefPath(const wchar_t *repo, wchar_t *path, int cch)
{
    lstrcpynW(path, repo, cch);
    lstrcpynW(path + lstrlenW(path), L"\\refs\\heads\\master", cch - lstrlenW(path));
    return TRUE;
}

static BOOL ReadMaster(const wchar_t *repo, char hex[41])
{
    hex[0] = 0;
    wchar_t path[MAX_PATH + 64];
    RepoRefPath(repo, path, MAX_PATH + 64);
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

static BOOL WriteMaster(const wchar_t *repo, const char *hex)
{
    char body[48];
    wsprintfA(body, "%s\n", hex);
    wchar_t path[MAX_PATH + 64];
    RepoRefPath(repo, path, MAX_PATH + 64);
    return WriteAllBytes(path, body, lstrlenA(body));
}

static BOOL BuildRepoDir(wchar_t *repo, int cch)
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

static BOOL EnsureRepo(const wchar_t *repo)
{
    wchar_t sub[MAX_PATH + 32];
    wsprintfW(sub, L"%s\\refs\\heads", repo);
    if (GetFileAttributesW(sub) == INVALID_FILE_ATTRIBUTES) {
        wchar_t objs[MAX_PATH + 32], refs[MAX_PATH + 32];
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
static const char *CommitLine(const char *body, int blen,
                              const char *prefix, int plen, int *vlen);

static BOOL CloneCommit(const wchar_t *repo, const char *origHex,
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
static const char *CommitLine(const char *body, int blen,
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

    wchar_t pattern[MAX_PATH + 16];
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

/* archive on save: real git commit, skipped when content is unchanged */
static void GitArchive(const char *u8, int u8len, BOOL isAuto)
{
    wchar_t repo[MAX_PATH];
    if (!BuildRepoDir(repo, MAX_PATH)) return;
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
                      parentOvr, commitHex))
        lstrcpynA(g_archivedTree, treeHex, 41);
}

static BOOL SaveFileEx(const wchar_t *path, BOOL isAuto)
{
    int len = GetWindowTextLengthW(g_edit);
    wchar_t *wbuf = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!wbuf) return FALSE;
    GetWindowTextW(g_edit, wbuf, len + 1);

    int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, NULL, 0, NULL, NULL);
    char *u8 = (char *)malloc(u8len + 1);
    if (!u8) { free(wbuf); return FALSE; }
    WideCharToMultiByte(CP_UTF8, 0, wbuf, len, u8, u8len, NULL, NULL);
    free(wbuf);

    if (!WriteAllBytes(path, u8, u8len)) { free(u8); return FALSE; }

    SetPath(path);
    g_dirty = FALSE;
    UpdateTitle();
    GitArchive(u8, u8len, isAuto);
    free(u8);
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

/* ---- HTML export / copy (item 10) ---- */

static char *BuildHtml(int *outLen)
{
    int len = GetWindowTextLengthW(g_edit);
    wchar_t *wbuf = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!wbuf) return NULL;
    GetWindowTextW(g_edit, wbuf, len + 1);
    char *html = NULL;
    int hl = md_to_html(wbuf, len, &html);
    free(wbuf);
    if (!hl) return NULL;
    *outLen = hl;
    return html;
}

/* build CF_HTML clipboard format (byte offsets patched after assembly) */
static char *BuildCfHtml(const char *bodyUtf8, int *outLen)
{
    static const char *head =
        "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n"
        "StartFragment:0000000000\r\nEndFragment:0000000000\r\n"
        "<html><body>\r\n<!--StartFragment-->";
    static const char *mid = "<!--EndFragment-->\r\n</body></html>";
    int hlen = (int)strlen(head), mlen = (int)strlen(mid);
    int blen = (int)strlen(bodyUtf8); /* body ends with \n; safe as text */
    int total = hlen + blen + mlen + 1;
    char *buf = (char *)malloc(total);
    if (!buf) return NULL;
    memcpy(buf, head, hlen);
    memcpy(buf + hlen, bodyUtf8, blen);
    memcpy(buf + hlen + blen, mid, mlen);
    buf[hlen + blen + mlen] = 0;
    /* strip trailing newlines from the fragment */
    int fend = hlen + blen;
    while (fend > hlen && (buf[fend-1] == '\n' || buf[fend-1] == '\r')) fend--;
    char num[17];
    wsprintfA(num, "%010d", hlen);
    memcpy(buf + 23, num, 10);                     /* StartHTML */
    wsprintfA(num, "%010d", hlen + blen + mlen);
    memcpy(buf + 43, num, 10);                     /* EndHTML   */
    wsprintfA(num, "%010d", hlen);
    memcpy(buf + 69, num, 10);                     /* StartFrag */
    wsprintfA(num, "%010d", fend);
    memcpy(buf + 93, num, 10);                     /* EndFrag   */
    *outLen = hlen + blen + mlen;
    return buf;
}

static void CopyHtml(void)
{
    int hl = 0;
    char *html = BuildHtml(&hl);
    if (!html) return;

    /* wide version of the html source (for plain-text targets) */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, html, hl, NULL, 0);
    wchar_t *whtml = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
    int cfLen = 0;
    char *cfHtml = BuildCfHtml(html, &cfLen);
    BOOL ok = FALSE;
    if (whtml && cfHtml && OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        HGLOBAL gw = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(wchar_t));
        HGLOBAL gc = GlobalAlloc(GMEM_MOVEABLE, cfLen + 1);
        if (gw && gc) {
            wchar_t *pw = (wchar_t *)GlobalLock(gw);
            char *pc = (char *)GlobalLock(gc);
            if (pw && pc) {
                MultiByteToWideChar(CP_UTF8, 0, html, hl, pw, wlen);
                pw[wlen] = 0;
                memcpy(pc, cfHtml, cfLen);
                pc[cfLen] = 0;
                GlobalUnlock(gw);
                GlobalUnlock(gc);
                ok = SetClipboardData(CF_UNICODETEXT, gw) != NULL;
                UINT cf = RegisterClipboardFormatW(L"HTML Format");
                if (cf) SetClipboardData(cf, gc);
            }
            if (ok) { gw = NULL; gc = NULL; } /* system owns on success */
        }
        if (gw) GlobalFree(gw);
        if (gc) GlobalFree(gc);
        CloseClipboard();
    }
    free(whtml);
    free(cfHtml);
    free(html);
    MessageBoxW(g_hwnd, ok ? L"HTML 已复制到剪贴板。" : L"复制失败。",
                APP_NAME, MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
}

static void ExportHtml(void)
{
    int hl = 0;
    char *html = BuildHtml(&hl);
    if (!html) return;

    wchar_t buf[MAX_PATH];
    buf[0] = 0;
    /* default name: document name with .html */
    if (g_name[0]) {
        lstrcpynW(buf, g_name, MAX_PATH);
        wchar_t *dot = wcsrchr(buf, L'.');
        if (dot) *dot = 0;
        lstrcpynW(buf + lstrlenW(buf), L".html", MAX_PATH - lstrlenW(buf));
    } else lstrcpynW(buf, L"未命名.html", MAX_PATH);

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"HTML (*.html;*.htm)\0*.html;*.htm\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"html";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) { free(html); return; }
    BOOL ok = WriteAllBytes(buf, html, hl);
    free(html);
    if (ok) {
        wchar_t msg[MAX_PATH + 32];
        wsprintfW(msg, L"已导出：%s", buf);
        MessageBoxW(g_hwnd, msg, APP_NAME, MB_OK | MB_ICONINFORMATION);
    } else
        MessageBoxW(g_hwnd, L"导出失败。", APP_NAME, MB_ICONERROR);
}

/* ---- plain-text export (item 33): strip markdown syntax ---- */

/* in-place-ish single-pass stripper; returns new length */
static int StripMarkdown(const wchar_t *in, int len, wchar_t *out,
                         int cchOut)
{
    int o = 0, i = 0;
    BOOL inCode = FALSE;   /* ``` fenced block: keep content, drop fence */
    while (i < len && o < cchOut - 2) {
        /* line-based processing */
        int ls = i;
        while (i < len && in[i] != L'\n' && in[i] != L'\r') i++;
        int le = i;
        while (i < len && (in[i] == L'\n' || in[i] == L'\r')) i++;
        int nl = i - le;   /* newline chars consumed */
        int s = ls, e = le;
        if (e - s >= 3 && in[s] == L'`' && in[s + 1] == L'`'
            && in[s + 2] == L'`') {
            inCode = !inCode;
            continue;      /* drop fence lines entirely */
        }
        if (!inCode) {
            /* heading marker */
            while (e - s > 0 && in[s] == L'#') s++;
            while (e - s > 0 && in[s] == L' ') s++;
            /* list / quote markers -> drop marker, keep one space */
            if (e - s > 0 && (in[s] == L'-' || in[s] == L'+'
                              || in[s] == L'*') && e - s > 1
                && in[s + 1] == L' ') s++;
            else if (e - s > 1 && in[s] == L'>') { s++; }
        }
        /* inline: copy with pair markers removed, links expanded */
        for (int k = s; k < e && o < cchOut - 2; k++) {
            wchar_t c = in[k];
            if (c == L'`' || c == L'~') continue;      /* code/strike */
            if ((c == L'*')) {
                /* collapse ** and * : drop all asterisks */
                continue;
            }
            if (c == L'_') {
                /* drop emphasis markers, keep snake_case (alnum both sides) */
                int la = (k > s) && (iswalnum((wint_t)in[k - 1]) != 0);
                int ra = (k + 1 < e) && (iswalnum((wint_t)in[k + 1]) != 0);
                if (la && ra) out[o++] = c;
                continue;
            }
            if (c == L'[') {
                /* [text](url) or ![alt](src) */
                BOOL img = (k > s && in[k - 1] == L'!');
                int close = -1, paren = -1;
                for (int q = k + 1; q < e; q++) {
                    if (in[q] == L']') { close = q; break; }
                }
                if (close >= 0 && close + 1 < e && in[close + 1] == L'(') {
                    for (int q = close + 2; q < e; q++) {
                        if (in[q] == L')') { paren = q; break; }
                    }
                }
                if (paren > close) {
                    if (img) {
                        if (o > 0 && out[o - 1] == L'!') o--;
                        static const wchar_t pic[] = L"[图片]";
                        int pl = 4;
                        for (int q = 0; q < pl && o < cchOut - 2; q++)
                            out[o++] = pic[q];
                    } else {
                        for (int q = k + 1; q < close && o < cchOut - 2;
                             q++)
                            out[o++] = in[q];
                        out[o++] = L' ';
                        out[o++] = L'(';
                        for (int q = close + 2; q < paren && o < cchOut - 2;
                             q++)
                            out[o++] = in[q];
                        out[o++] = L')';
                    }
                    k = paren;
                    continue;
                }
            }
            out[o++] = c;
        }
        /* preserve line break (normalize to \r\n for notepad) */
        if (o < cchOut - 2) { out[o++] = L'\r'; out[o++] = L'\n'; }
        (void)nl;
    }
    out[o] = 0;
    return o;
}

static void DoPlainExport(void)
{
    int len = GetWindowTextLengthW(g_edit);
    wchar_t *src = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    wchar_t *dst = (wchar_t *)malloc((len + 2) * sizeof(wchar_t) + 64);
    if (!src || !dst) { free(src); free(dst); return; }
    GetWindowTextW(g_edit, src, len + 1);
    int ol = StripMarkdown(src, len, dst, len + 64);
    free(src);
    if (ol <= 0) {
        MessageBoxW(g_hwnd, L"文档为空。", APP_NAME, MB_ICONINFORMATION);
        free(dst);
        return;
    }
    int u8cap = WideCharToMultiByte(CP_UTF8, 0, dst, ol, NULL, 0,
                                    NULL, NULL) + 3;
    char *u8 = (char *)malloc(u8cap);
    if (!u8) { free(dst); return; }
    u8[0] = (char)0xEF; u8[1] = (char)0xBB; u8[2] = (char)0xBF; /* BOM */
    int u8l = WideCharToMultiByte(CP_UTF8, 0, dst, ol, u8 + 3,
                                  u8cap - 3, NULL, NULL);
    free(dst);
    if (u8l <= 0) { free(u8); return; }

    wchar_t buf[MAX_PATH];
    buf[0] = 0;
    if (g_name[0]) {
        lstrcpynW(buf, g_name, MAX_PATH);
        wchar_t *dot = wcsrchr(buf, L'.');
        if (dot) *dot = 0;
        lstrcpynW(buf + lstrlenW(buf), L".txt",
                  MAX_PATH - lstrlenW(buf));
    } else lstrcpynW(buf, L"未命名.txt", MAX_PATH);
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) { free(u8); return; }
    BOOL ok = WriteAllBytes(buf, u8, u8l + 3);
    free(u8);
    if (ok) {
        wchar_t msg[MAX_PATH + 32];
        wsprintfW(msg, L"已导出：%s", buf);
        MessageBoxW(g_hwnd, msg, APP_NAME, MB_OK | MB_ICONINFORMATION);
    } else
        MessageBoxW(g_hwnd, L"导出失败。", APP_NAME, MB_ICONERROR);
}

/* ---- window topmost toggle (item 45) ---- */

static void SetTopmost(BOOL on)
{
    g_topmost = on;
    SetWindowPos(g_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

/* ---- print / PDF export (item 29) ---- */

static void DoPrint(void)
{
    PRINTDLGW pd;
    ZeroMemory(&pd, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = g_hwnd;
    pd.Flags = PD_RETURNDC;
    if (!PrintDlgW(&pd)) return;              /* cancelled */
    HDC dc = pd.hDC;
    if (!dc) {
        MessageBoxW(g_hwnd, L"未找到可用打印机。",
                    APP_NAME, MB_ICONWARNING);
        if (pd.hDevMode) GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
        return;
    }
    int pw = GetDeviceCaps(dc, HORZRES);
    int ph = GetDeviceCaps(dc, VERTRES);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    RECT page = { MulDiv(15, dpi, 25), MulDiv(15, dpi, 25),
                  pw - MulDiv(15, dpi, 25), ph - MulDiv(15, dpi, 25) };

    int len = GetWindowTextLengthW(g_edit);
    wchar_t *src = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!src) { DeleteDC(dc); return; }
    GetWindowTextW(g_edit, src, len + 1);

    MDFonts pf;
    md_init_fonts(&pf, dc, dpi);
    MDDoc doc;
    md_build(&doc, src, len, &pf, dc, page.right - page.left);

    DOCINFOW di;
    ZeroMemory(&di, sizeof(di));
    di.cbSize = sizeof(di);
    di.lpszDocName = g_name[0] ? g_name : L"MDLite";
    if (StartDocW(dc, &di) > 0) {
        int pageH = page.bottom - page.top;
        int pages = (doc.height + pageH - 1) / pageH;
        if (pages < 1) pages = 1;
        for (int p = 0; p < pages; p++) {
            StartPage(dc);
            md_paint(&doc, dc, &page, p * pageH, &pf);
            EndPage(dc);
        }
        EndDoc(dc);
    } else
        MessageBoxW(g_hwnd, L"启动打印任务失败。", APP_NAME,
                    MB_ICONERROR);

    md_free(&doc);
    md_free_fonts(&pf);
    free(src);
    DeleteDC(dc);
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
}

static void ShowHelp(void)
{
    static const wchar_t *help =
        L"MDLite 使用帮助\r\n"
        L"\r\n"
        L"视图：编辑 / 分屏 / 预览三态，Ctrl+/ 切换，或点右上分段。\r\n"
        L"文件：Ctrl+N 新建，Ctrl+O 打开，Ctrl+S 保存，Ctrl+Shift+S 另存；拖拽 .md 直接打开。\r\n"
        L"查找：Ctrl+F，Enter/Shift+Enter 下一个/上一个，Esc 关闭。\r\n"
        L"大纲：Ctrl+P 弹出标题列表，输入过滤，Enter 跳转，Esc 关闭。\r\n"
        L"AI：行首输入 /问题 后回车发送（OpenAI 兼容接口），Esc 中断。\r\n"
        L"Agent：行首输入 //任务 后回车，调用 opencode 在文档目录执行，Esc 终止；超时秒数可在设置中调整。\r\n"
        L"历史：点「历史」查看版本，点击恢复；行右侧 ✕ 删除单条；上限在设置中调整。\r\n"
        L"自动保存：设置中可配间隔（秒，0=关闭），默认 60 秒。\r\n"
        L"HTML：更多菜单可复制 / 导出 HTML，与预览同款渲染。\r\n"
        L"热键：设置中可配全局呼出热键（默认 Ctrl+Shift+Space）。\r\n"
        L"缩放：Ctrl+滚轮 85%-150% 五档。\r\n"
        L"托盘：关闭窗口最小化到托盘，右键退出。";
    MessageBoxW(g_hwnd, help, L"MDLite 帮助", MB_ICONINFORMATION);
}

/* "more" dropdown anchored under header button 4 */
static RECT BtnRect(int id);
static void ShowMoreMenu(void)
{
    HMENU pm = CreatePopupMenu();
    AppendMenuW(pm, MF_STRING, IDM_OUTLINE,    L"大纲\tCtrl+P");
    AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(pm, MF_STRING, IDM_COPYHTML,   L"复制为 HTML");
    AppendMenuW(pm, MF_STRING, IDM_EXPORTHTML, L"导出 HTML…");
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
            wchar_t txt[192];
            wchar_t dir[MAX_PATH];
            lstrcpynW(dir, p, (int)(slash - p) + 1);
            wsprintfW(txt, L"%s\t%s", name, dir);
            AppendMenuW(sub, MF_STRING, IDM_MRU_BASE + i, txt);
        }
        AppendMenuW(pm, MF_POPUP, (UINT_PTR)sub, L"最近文件");
        AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(pm, MF_STRING | (g_topmost ? MF_CHECKED : 0),
                IDM_TOPMOST, L"窗口置顶");
    AppendMenuW(pm, MF_STRING, IDM_PRINT,      L"打印 / 导出 PDF…");
    AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(pm, MF_STRING, IDM_HELP,       L"帮助");
    POINT pt = { BtnRect(4).left, BtnRect(4).bottom + SC(2) };
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

static BOOL ConfirmDiscard(void)
{
    if (!g_dirty) return TRUE;
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

static RECT BtnRect(int id) /* 0 open 1 save 2 history 3 settings 4 more */
{
    RECT rc;
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
    for (int i = 0; i < 5; i++) {
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

static void PinLoad(const wchar_t *path)
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

static BOOL PinHas(const char *sha)
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
    static const wchar_t *labels[5] =
        { L"\x25B6 打开", L"\x25A0 保存", L"\x2630 历史", L"\x2699 配置",
          L"\x22EF 更多" };
    for (int i = 0; i < 5; i++) {
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
static void StatusTick(void)
{
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
    } else {
        wsprintfW(right, L"%s   UTF-8", g_dirty ? L"未保存" : L"已保存");
    }
    int rw = text_w(dc, g_fontStatus, right, lstrlenW(right));
    SetTextColor(dc, (g_aiBusy || g_agBusy) ? COL_ACCENT
                        : (g_dirty ? COL_ACCENT : COL_STATTXT));
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

static void ShowFindBar(void)
{
    if (g_view == VIEW_PREVIEW) SetView(VIEW_SPLIT); /* keep editing */
    if (!g_findEdit) {
        g_findEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_findQuery,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, g_hwnd, (HMENU)2, NULL, NULL);
        g_findProc = (WNDPROC)SetWindowLongPtrW(g_findEdit, GWLP_WNDPROC,
                                                (LONG_PTR)FindProc);
        SendMessageW(g_findEdit, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
    }
    g_findShown = TRUE;
    ShowWindow(g_findEdit, SW_SHOW);
    LayoutChildren();
    InvalidateRect(g_hwnd, NULL, TRUE);
    SetFocus(g_findEdit);
    SendMessageW(g_findEdit, EM_SETSEL, 0, -1);
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

static LRESULT CALLBACK FindProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
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

    static const wchar_t *lbl = L"查找";
    HFONT old = (HFONT)SelectObject(dc, g_fontHeader);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, COL_TITLE);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    TextOutW(dc, SC(14), rc.top + (SC(40) - tm.tmHeight) / 2,
             lbl, lstrlenW(lbl));

    const wchar_t *info = g_findInfo[0] ? g_findInfo : L"Enter 下一个 · Shift+Enter 上一个";
    int w = text_w(dc, g_fontStatus, info, lstrlenW(info));
    SelectObject(dc, g_fontStatus);
    SetTextColor(dc, COL_STATTXT);
    TextOutW(dc, rc.right - SC(14) - w,
             rc.top + (SC(40) - tm.tmHeight) / 2, info, lstrlenW(info));
    SelectObject(dc, old);
}

/* ------------------------------------------------------------------ */
/* edit subclass                                                       */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* AI slash command: OpenAI-compatible streaming chat via WinHTTP      */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t base[256], model[128], key[256], sys[1024], q[1024];
    int timeout;
} AiJob;

/* escape a wide string into a JSON string body (no quotes) */
static int JsonEscape(const wchar_t *s, wchar_t *out, int cap)
{
    int n = 0;
    for (; *s; s++) {
        if (n + 8 >= cap) break;
        switch (*s) {
        case L'"':  out[n++] = L'\\'; out[n++] = L'"';  break;
        case L'\\': out[n++] = L'\\'; out[n++] = L'\\'; break;
        case L'\n': out[n++] = L'\\'; out[n++] = L'n';  break;
        case L'\r': out[n++] = L'\\'; out[n++] = L'r';  break;
        case L'\t': out[n++] = L'\\'; out[n++] = L't';  break;
        default:
            if (*s < 0x20) {
                static const wchar_t HX[] = L"0123456789abcdef";
                out[n++] = L'\\'; out[n++] = L'u';
                out[n++] = HX[(*s >> 12) & 15];
                out[n++] = HX[(*s >> 8) & 15];
                out[n++] = HX[(*s >> 4) & 15];
                out[n++] = HX[*s & 15];
            } else {
                out[n++] = *s;
            }
        }
    }
    out[n] = 0;
    return n;
}

/* read a JSON string at p (p points at the opening quote);
 * returns chars consumed, fills out (NUL-terminated). 0 on error. */
static int JsonReadString(const char *p, const char *end, wchar_t *out,
                          int outMax)
{
    const char *start = p;
    if (p >= end || *p != '"') return 0;
    p++;
    int n = 0;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            out[n] = 0;
            return (int)(p - start) + 1;
        }
        if (c == '\\') {
            p++;
            if (p >= end) return 0;
            char e = *p;
            if (e == 'u') {
                if (p + 4 >= end) return 0;
                int v = 0;
                for (int k = 1; k <= 4; k++) {
                    char h = p[k];
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= h - '0';
                    else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
                    else return 0;
                }
                if (n < outMax - 1) out[n++] = (wchar_t)v;
                p += 5;
                continue;
            }
            wchar_t m = 0;
            switch (e) {
            case '"':  m = L'"';  break;
            case '\\': m = L'\\'; break;
            case '/':  m = L'/';  break;
            case 'b':  m = L'\b'; break;
            case 'f':  m = L'\f'; break;
            case 'n':  m = L'\n'; break;
            case 'r':  m = L'\r'; break;
            case 't':  m = L'\t'; break;
            default: return 0;
            }
            if (n < outMax - 1) out[n++] = m;
            p++;
            continue;
        }
        /* raw UTF-8 byte -> decode */
        int need = 1;
        if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        if (c < 0x80) {
            if (n < outMax - 1) out[n++] = (wchar_t)c;
            p++;
        } else {
            if (p + need > end) return 0;
            DWORD cp = 0;
            if (need == 2) cp = c & 0x1F;
            else if (need == 3) cp = c & 0x0F;
            else cp = c & 0x07;
            BOOL bad = FALSE;
            for (int k = 1; k < need; k++) {
                unsigned char cc = (unsigned char)p[k];
                if ((cc & 0xC0) != 0x80) { bad = TRUE; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (bad) return 0;
            wchar_t w[2];
            int wl = 0;
            if (cp < 0x10000) w[wl++] = (wchar_t)cp;
            else {
                cp -= 0x10000;
                w[wl++] = (wchar_t)(0xD800 + (cp >> 10));
                w[wl++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
            }
            for (int k = 0; k < wl; k++)
                if (n < outMax - 1) out[n++] = w[k];
            p += need;
        }
    }
    return 0;
}

/* find "key" [ws]* ':' [ws]* '"' inside [p,end); returns pointer to the
 * value's opening quote, or NULL. whitespace-tolerant. */
static void AiPost(const wchar_t *txt);
static const char *JsonFindStr(const char *p, const char *end, const char *key)
{
    int kl = (int)lstrlenA(key);
    for (const char *q = p; q + kl < end; q++) {
        if (memcmp(q, key, kl) != 0) continue;
        const char *r = q + kl;
        while (r < end && (*r == ' ' || *r == '\t')) r++;
        if (r >= end || *r != ':') continue;
        r++;
        while (r < end && (*r == ' ' || *r == '\t')) r++;
        if (r < end && *r == '"') return r;
    }
    return NULL;
}

/* extract answer text from one SSE "data:" payload or a whole JSON body;
 * handles streamed "delta":{"content":..} and non-streamed
 * "message":{"content":..} shapes. returns wchar count, 0 when none. */
static int SseParsePayload(const char *pl, int plen, wchar_t *out, int outMax)
{
    const char *end = pl + plen;
    const char *d = JsonFindStr(pl, end, "\"delta\"");
    const char *v = d ? JsonFindStr(d, end, "\"content\"") : NULL;
    if (!v) v = JsonFindStr(pl, end, "\"content\"");
    if (!v) return 0;
    int used = JsonReadString(v, end, out, outMax);
    if (used > 0) return lstrlenW(out);
    return 0;
}

/* parse one SSE/JSON line and post visible text; returns TRUE on [DONE] */
static BOOL AiHandleLine(const char *ln, int ll, wchar_t *chunk, int *got)
{
    if (ll > 5 && memcmp(ln, "data:", 5) == 0) {
        const char *pl = ln + 5;
        int plen = ll - 5;
        while (plen > 0 && (*pl == ' ' || *pl == '\t')) { pl++; plen--; }
        if (plen == 5 && memcmp(pl, "[DONE]", 5) == 0) return TRUE;
        int cn = SseParsePayload(pl, plen, chunk, 8192);
        if (cn > 0) { AiPost(chunk); *got += cn; }
    } else if (ll > 2) {
        int off = 0;
        while (off < ll && (ln[off] == ' ' || ln[off] == '\t'
                            || ln[off] == '\r')) off++;
        /* non-SSE line: maybe a whole non-stream JSON body */
        if (ln[off] == '{') {
            int cn = SseParsePayload(ln + off, ll - off, chunk, 8192);
            if (cn > 0) { AiPost(chunk); *got += cn; }
        }
    }
    return FALSE;
}

static void AiPost(const wchar_t *txt)
{
    int n = lstrlenW(txt);
    wchar_t *heap = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!heap) return;
    memcpy(heap, txt, (n + 1) * sizeof(wchar_t));
    PostMessageW(g_hwnd, WM_AI_CHUNK, n, (LPARAM)heap);
}

static void AiPostDone(const wchar_t *err)
{
    int n = err ? lstrlenW(err) : 0;
    wchar_t *heap = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!heap) return;
    if (err) memcpy(heap, err, (n + 1) * sizeof(wchar_t));
    else heap[0] = 0;
    PostMessageW(g_hwnd, WM_AI_DONE, err ? 0 : 1, (LPARAM)heap);
}

static DWORD WINAPI AiThreadProc(LPVOID param)
{
    AiJob job = *(AiJob *)param;
    free(param);

    wchar_t *errText = NULL;
    wchar_t *doneText = NULL;

    /* full endpoint url: base minus trailing slashes + /chat/completions
     * (skip when the base already ends with it) */
    wchar_t url[600];
    lstrcpynW(url, job.base, 580);
    int ul = lstrlenW(url);
    while (ul > 0 && url[ul - 1] == L'/') url[--ul] = 0;
    static const wchar_t EP[] = L"/chat/completions";
    const int epl = 17;
    BOOL hasEp = FALSE;
    if (ul >= epl - 1 && wcscmp(url + ul - (epl - 1), EP + 1) == 0
        && (ul == epl - 1 || url[ul - epl] == L'/'))
        hasEp = TRUE;
    if (!hasEp) lstrcpynW(url + ul, EP, 600 - ul);

    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[512];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 511;
    uc.dwSchemeLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        doneText = L"Base URL 无效";
        goto post;
    }

    HINTERNET ses = WinHttpOpen(L"MDLite/1.0",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { doneText = L"网络初始化失败"; goto post; }
    HINTERNET conn = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!conn) { WinHttpCloseHandle(ses); doneText = L"连接失败"; goto post; }
    HINTERNET req = WinHttpOpenRequest(conn, L"POST", path, NULL,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       (uc.nScheme == INTERNET_SCHEME_HTTPS)
                                           ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        WinHttpCloseHandle(conn); WinHttpCloseHandle(ses);
        doneText = L"创建请求失败"; goto post;
    }
    g_aiReq = req;
    InterlockedExchange((volatile LONG *)&g_aiCancelFlag, 0);

    int to = job.timeout > 0 ? job.timeout : 10;
    WinHttpSetTimeouts(ses, 5000, 5000, to * 1000, to * 1000);

    /* request body (UTF-8, JSON-escaped fields) */
    char body[28672];
    wchar_t esc[5120];
    int bo = 0;
    int bodyCap = 28000;
    strcpy(body + bo, "{\"model\":\"");
    bo += lstrlenA(body + bo);
    {
        JsonEscape(job.model, esc, 5120);
        int bl = WideCharToMultiByte(CP_UTF8, 0, esc, -1, body + bo,
                                     bodyCap - bo, NULL, NULL) - 1;
        if (bl < 0) bl = 0;
        bo += bl;
    }
    strcpy(body + bo, "\",\"stream\":true,\"messages\":[");
    bo += lstrlenA(body + bo);
    if (job.sys[0]) {
        JsonEscape(job.sys, esc, 5120);
        strcpy(body + bo, "{\"role\":\"system\",\"content\":\"");
        bo += lstrlenA(body + bo);
        int bl = WideCharToMultiByte(CP_UTF8, 0, esc, -1, body + bo,
                                     bodyCap - bo, NULL, NULL) - 1;
        if (bl < 0) bl = 0;
        bo += bl;
        strcpy(body + bo, "\"},");
        bo += lstrlenA(body + bo);
    }
    /* prior turns: collect newest-first within a budget, emit oldest-first */
    {
        const wchar_t *hq[AI_CTX_MAX], *ha[AI_CTX_MAX];
        int hn = 0, budget = 8000; /* wchar budget for history */
        int rounds = g_aiRounds;
        if (rounds > AI_CTX_MAX) rounds = AI_CTX_MAX;
        for (int i = g_aiCtxCnt - 1; i >= 0 && hn < rounds; i--) {
            int ql = lstrlenW(g_aiCtxQ[i]);
            int al = lstrlenW(g_aiCtxA[i]);
            if (ql + al > budget) break;
            budget -= ql + al;
            hq[hn] = g_aiCtxQ[i];
            ha[hn] = g_aiCtxA[i];
            hn++;
        }
        for (int i = hn - 1; i >= 0; i--) {
            const wchar_t *turn[2] = { hq[i], ha[i] };
            for (int t = 0; t < 2; t++) {
                JsonEscape(turn[t], esc, 5120);
                strcpy(body + bo, "{\"role\":\"");
                bo += lstrlenA(body + bo);
                lstrcpyA(body + bo, t ? "assistant" : "user");
                bo += lstrlenA(body + bo);
                strcpy(body + bo, "\",\"content\":\"");
                bo += lstrlenA(body + bo);
                int bl = WideCharToMultiByte(CP_UTF8, 0, esc, -1, body + bo,
                                             bodyCap - bo, NULL, NULL) - 1;
                if (bl < 0) bl = 0;
                bo += bl;
                strcpy(body + bo, "\"},");
                bo += lstrlenA(body + bo);
            }
        }
    }
    {
        JsonEscape(job.q, esc, 5120);
        strcpy(body + bo, "{\"role\":\"user\",\"content\":\"");
        bo += lstrlenA(body + bo);
        int bl = WideCharToMultiByte(CP_UTF8, 0, esc, -1, body + bo,
                                     bodyCap - bo, NULL, NULL) - 1;
        if (bl < 0) bl = 0;
        bo += bl;
        strcpy(body + bo, "\"}");
        bo += lstrlenA(body + bo);
    }
    strcpy(body + bo, "]}");
    bo += lstrlenA(body + bo);

    wchar_t hdrs[600];
    if (job.key[0])
        wsprintfW(hdrs, L"Content-Type: application/json\r\n"
                        L"Authorization: Bearer %s\r\n", job.key);
    else
        lstrcpynW(hdrs, L"Content-Type: application/json\r\n", 600);

    BOOL ok = WinHttpSendRequest(req, hdrs, (DWORD)-1,
                                 (LPVOID)body, bo, bo, 0)
              && WinHttpReceiveResponse(req, NULL);
    if (!ok) {
        DWORD we = GetLastError();
        if (g_aiCancelFlag || we == ERROR_INVALID_HANDLE)
            doneText = L"已取消";
        else if (we == ERROR_WINHTTP_TIMEOUT)
            doneText = L"请求超时";
        else {
            errText = (wchar_t *)malloc(64 * sizeof(wchar_t));
            if (errText)
                wsprintfW(errText, L"发送失败（错误码 %u）", we);
            doneText = errText;
        }
        goto cleanup;
    }

    /* HTTP status */
    DWORD status = 0, szd = sizeof(status);
    WinHttpQueryHeaders(req,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL,
        &status, &szd, NULL);
    if (status != 200) {
        errText = (wchar_t *)malloc(64 * sizeof(wchar_t));
        if (errText) wsprintfW(errText, L"HTTP %u", status);
        doneText = errText;
        goto cleanup;
    }

    /* stream + SSE (also tolerates non-stream whole-JSON bodies) */
    {
        char sse[16384];
        int sseLen = 0;
        BOOL fin = FALSE;
        int got = 0;              /* chars successfully shown */
        wchar_t chunk[8192];
        while (!fin && !g_aiCancelFlag) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) {
                DWORD we = GetLastError();
                if (g_aiCancelFlag || we == ERROR_INVALID_HANDLE)
                    doneText = L"已取消";
                else if (we == ERROR_WINHTTP_TIMEOUT)
                    doneText = L"接收超时";
                else
                    doneText = L"连接中断";
                break;
            }
            if (avail == 0) break; /* server closed: stream end */
            DWORD cap = (DWORD)(sizeof(sse) - 1 - (size_t)sseLen);
            if (cap == 0) sseLen = 0, cap = sizeof(sse) - 1; /* giant line */
            if (avail > cap) avail = cap;
            DWORD rd = 0;
            if (!WinHttpReadData(req, sse + sseLen, avail, &rd) || rd == 0) {
                DWORD we = GetLastError();
                if (g_aiCancelFlag || we == ERROR_INVALID_HANDLE)
                    doneText = L"已取消";
                else if (we == ERROR_WINHTTP_TIMEOUT)
                    doneText = L"接收超时";
                else
                    doneText = L"连接中断";
                break;
            }
            sseLen += rd;
            /* process complete lines */
            int ls = 0;
            for (int i = 0; i < sseLen; i++) {
                if (sse[i] != '\n') continue;
                int le = i;
                if (le > ls && sse[le - 1] == '\r') le--;
                if (!fin && AiHandleLine(sse + ls, le - ls, chunk, &got))
                    fin = TRUE;
                ls = i + 1;
            }
            if (ls > 0) {
                memmove(sse, sse + ls, sseLen - ls);
                sseLen -= ls;
            }
        }
        /* server closed without trailing newline: last unterminated line */
        if (!doneText && sseLen > 0)
            AiHandleLine(sse, sseLen, chunk, &got);
        /* never leave the user without feedback */
        if (!doneText && got == 0) {
            if (g_aiCancelFlag) doneText = L"已取消";
            else doneText = L"未收到内容（模型或接口可能不兼容，"
                             L"请检查 Base URL 与模型）";
        }
    }

cleanup:
    g_aiReq = NULL;
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(ses);
post:
    AiPostDone(doneText);
    free(errText);
    return 0;
}

/* -------- shared background-task insert machinery (AI + agent) -------- */

/* find the insert point: end of the line that matches the task's anchor.
 * the anchor is the question line text; when the user edits elsewhere the
 * line moves but is still found, so the answer always lands directly
 * under its question. falls back to document end when the anchor is gone
 * (e.g. user deleted the question) so no streamed text is ever lost. */
static int TaskLocate(BgTask *t)
{
    if (!t->anchor[0]) {
        int len = GetWindowTextLengthW(g_edit);
        if (t->insEnd < len) t->insEnd = len;
        return t->insEnd;
    }
    int aLen = lstrlenW(t->anchor);
    int total = GetWindowTextLengthW(g_edit);
    /* scan lines from the top; compare each line against the anchor */
    int line = 0, pos = 0;
    wchar_t buf[2048];
    while (pos <= total) {
        int ll = (int)SendMessageW(g_edit, EM_LINELENGTH, pos, 0);
        if (ll > 0) {
            int want = ll + 1;
            if (want > 2048) want = 2048;
            buf[0] = (wchar_t)(want - 1);
            int n = (int)SendMessageW(g_edit, EM_GETLINE, line,
                                      (LPARAM)buf);
            if (n > 0) {
                buf[n] = 0;
                if (n >= aLen && wcsncmp(buf, t->anchor, aLen) == 0) {
                    /* derive the offset from the anchor every time:
                     * answer block starts right below the question
                     * ("\r\n---\r\n" = 7 chars) plus everything this task
                     * has streamed so far. user edits anywhere else can
                     * shift raw offsets, so never trust a stored one. */
                    int lineEnd = pos + ll;
                    t->insEnd = lineEnd + 7 + t->totalLen;
                    return t->insEnd;
                }
            }
        }
        if (pos + ll >= total) break;
        pos = pos + ll + 2;   /* skip CRLF */
        line++;
    }
    /* anchor lost: append at document end */
    if (t->insEnd < total) t->insEnd = total;
    return t->insEnd;
}

/* insert text at the task's tracked position without stealing the user's
 * caret: remember the caret, insert, then restore it shifted by however
 * many chars landed before it */
static void TaskInsert(BgTask *t, const wchar_t *txt)
{
    DWORD s, e;
    BOOL hadSel = TRUE;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    int at = TaskLocate(t);
    int n = lstrlenW(txt);
    SendMessageW(g_edit, EM_SETSEL, at, at);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)txt);
    t->insEnd = at + n;
    t->totalLen += n;
    /* restore the user's caret/selection, compensated for the insert */
    if (s >= (DWORD)at) s += n;
    if (e >= (DWORD)at) e += n;
    SendMessageW(g_edit, EM_SETSEL, s, e);
    if (s == e) hadSel = FALSE;
    /* keep the answer tail roughly in view only when the user's caret
     * sits inside the streaming region (watching it arrive) */
    if (s >= (DWORD)at && s <= (DWORD)(at + n))
        SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
    (void)hadSel;
}

/* begin a task slot: record anchor and open the answer block under the
 * question line with a leading separator */
static BOOL TaskBegin(int slot, const wchar_t *question)
{
    (void)question;
    BgTask *t = &g_tasks[slot];
    DWORD s, e;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    int li = (int)SendMessageW(g_edit, EM_LINEFROMCHAR, s, 0);
    int ls = (int)SendMessageW(g_edit, EM_LINEINDEX, li, 0);
    int ll = (int)SendMessageW(g_edit, EM_LINELENGTH, ls, 0);
    int lineEnd = ls + ll;

    /* remember the question line as anchor (truncate long ones) */
    wchar_t buf[2048];
    buf[0] = 2048;
    int n = (int)SendMessageW(g_edit, EM_GETLINE, li, (LPARAM)buf);
    if (n > 0) buf[n] = 0; else buf[0] = 0;
    lstrcpynW(t->anchor, buf, 2048);

    SendMessageW(g_edit, EM_SETSEL, lineEnd, lineEnd);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n---\r\n");
    t->insEnd = lineEnd + 5;
    t->totalLen = 0;
    t->active = TRUE;
    return TRUE;
}

/* finish a task slot: trailing separator (+ optional error note) */
static void TaskFinish(int slot, const wchar_t *err)
{
    BgTask *t = &g_tasks[slot];
    wchar_t tail[512];
    if (err && err[0])
        wsprintfW(tail, L"\r\n---\r\n（%s）\r\n", err);
    else
        lstrcpynW(tail, L"\r\n---\r\n", 512);
    DWORD s, e;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    int at = TaskLocate(t);
    int n = lstrlenW(tail);
    SendMessageW(g_edit, EM_SETSEL, at, at);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)tail);
    if (s >= (DWORD)at) s += n;
    if (e >= (DWORD)at) e += n;
    SendMessageW(g_edit, EM_SETSEL, s, e);
    t->active = FALSE;
}

/* duplicate at most `max` characters of s (NUL-terminated) */
static wchar_t *DupTruncW(const wchar_t *s, int max)
{
    int n = lstrlenW(s);
    if (n > max) n = max;
    wchar_t *d = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (d) { memcpy(d, s, n * sizeof(wchar_t)); d[n] = 0; }
    return d;
}

/* remember a finished Q/A turn for multi-turn context */
static void StoreAiTurn(const wchar_t *q, const wchar_t *a)
{
    if (!q || !a || !q[0] || !a[0]) return;
    if (g_aiCtxCnt >= AI_CTX_MAX) {
        free(g_aiCtxQ[0]);
        free(g_aiCtxA[0]);
        for (int i = 1; i < AI_CTX_MAX; i++) {
            g_aiCtxQ[i - 1] = g_aiCtxQ[i];
            g_aiCtxA[i - 1] = g_aiCtxA[i];
        }
        g_aiCtxCnt = AI_CTX_MAX - 1;
    }
    wchar_t *dq = DupTruncW(q, 600);
    wchar_t *da = DupTruncW(a, 2400);
    if (dq && da) {
        g_aiCtxQ[g_aiCtxCnt] = dq;
        g_aiCtxA[g_aiCtxCnt] = da;
        g_aiCtxCnt++;
    } else {
        free(dq);
        free(da);
    }
}

/* wide-char append with bounds check (wsprintfW caps at 1024 chars) */
static int Wa(wchar_t *dst, int cap, int off, const wchar_t *s);

/* ------------------------------------------------------------------ */
/* selection AI (item 24): Ctrl+J acts on the selected text            */
/* ------------------------------------------------------------------ */

static BOOL     g_selAi;           /* selection-AI session active */
static DWORD    g_selAiStart;      /* original selection start */
static DWORD    g_selAiIns;        /* streaming insertion point */
static wchar_t *g_selAiBackup;     /* original selected text */

static void SelAiInsert(const wchar_t *txt)
{
    int n = lstrlenW(txt);
    if (n <= 0) return;
    SendMessageW(g_edit, EM_SETSEL, g_selAiIns, g_selAiIns);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)txt);
    g_selAiIns += n;
    SendMessageW(g_edit, EM_SETSEL, g_selAiIns, g_selAiIns);
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
}

static void SelAiFinish(const wchar_t *err)
{
    if (err && err[0] && g_selAiBackup) {
        /* failed or interrupted: undo the streamed text, restore */
        SendMessageW(g_edit, EM_SETSEL, g_selAiStart, g_selAiIns);
        SendMessageW(g_edit, EM_REPLACESEL, FALSE,
                     (LPARAM)g_selAiBackup);
        int bl = lstrlenW(g_selAiBackup);
        SendMessageW(g_edit, EM_SETSEL, g_selAiStart,
                     g_selAiStart + (DWORD)bl);
    }
    free(g_selAiBackup);
    g_selAiBackup = NULL;
    g_selAi = FALSE;
}

/* tiny modal input popup (custom prompt for selection AI) */
static BOOL PromptInput(const wchar_t *title, wchar_t *out, int cch)
{
    HWND pw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"#32770" /* built-in dialog class: frame + title bar */,
        title, WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU,
        0, 0, SC(380), SC(130), g_hwnd, NULL, NULL, NULL);
    if (!pw) return FALSE;
    RECT rw;
    GetWindowRect(pw, &rw);
    RECT rm;
    GetWindowRect(g_hwnd, &rm);
    SetWindowPos(pw, 0,
                 rm.left + (rm.right - rm.left - (rw.right - rw.left)) / 2,
                 rm.top + (rm.bottom - rm.top - (rw.bottom - rw.top)) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", out,
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        SC(14), SC(14), SC(352), SC(26), pw, (HMENU)1, NULL, NULL);
    CreateWindowExW(0, L"BUTTON", L"确定",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        SC(220), SC(56), SC(66), SC(28), pw, (HMENU)IDOK, NULL, NULL);
    CreateWindowExW(0, L"BUTTON", L"取消",
        WS_CHILD | WS_VISIBLE,
        SC(296), SC(56), SC(66), SC(28), pw, (HMENU)IDCANCEL, NULL, NULL);
    SendMessageW(ed, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
    HWND bn = GetDlgItem(pw, IDOK);
    if (bn) SendMessageW(bn, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
    bn = GetDlgItem(pw, IDCANCEL);
    if (bn) SendMessageW(bn, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
    ShowWindow(pw, SW_SHOW);
    UpdateWindow(pw);
    SetFocus(ed);
    BOOL ok = FALSE;
    MSG msg;
    for (;;) {
        while (IsWindow(pw) && GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (!IsWindow(pw)) break;
            if ((msg.hwnd == pw || IsChild(pw, msg.hwnd))
                && msg.message == WM_KEYDOWN) {
                if (msg.wParam == VK_RETURN) {
                    GetWindowTextW(ed, out, cch);
                    ok = out[0] != 0;
                    goto done;
                }
                if (msg.wParam == VK_ESCAPE) { ok = FALSE; goto done; }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (!IsWindow(pw)) { ok = FALSE; goto done2; }
        }
        if (!IsWindow(pw)) break;
    }
done:
    DestroyWindow(pw);
done2:
    return ok;
}

static void StartSelAi(const wchar_t *instr)
{
    if (!g_aiBase[0] || !g_aiKey[0]) {
        MessageBoxW(g_hwnd,
            L"尚未配置 AI。\n按 Ctrl+, 打开设置，填写 Base URL 与 API Key。",
            APP_NAME, MB_ICONINFORMATION);
        return;
    }
    DWORD s, e;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    if (e <= s) return;
    int slen = (int)(e - s);
    if (slen > 2048) slen = 2048;
    wchar_t *sel = (wchar_t *)malloc((slen + 1) * sizeof(wchar_t));
    if (!sel) return;
    /* copy the selection directly from the window text */
    {
        wchar_t *all = NULL;
        int total = GetWindowTextLengthW(g_edit);
        all = (wchar_t *)malloc((total + 1) * sizeof(wchar_t));
        if (!all) { free(sel); return; }
        GetWindowTextW(g_edit, all, total + 1);
        int copy = (int)(e - s);
        if (copy > slen) copy = slen;
        memcpy(sel, all + s, copy * sizeof(wchar_t));
        sel[copy] = 0;
        free(all);
    }

    /* build question: instruction + content (cap 950 chars total) */
    wchar_t q[1024];
    int o = 0;
    o = Wa(q, 1024, o, instr);
    o = Wa(q, 1024, o, L"以下内容：\n\n");
    int room = 1010 - o - 4;
    int cl = lstrlenW(sel);
    if (cl > room) {
        for (int k = 0; k < room && o < 1010; k++) q[o++] = sel[k];
        q[o++] = L'…';
        q[o] = 0;
    } else {
        o = Wa(q, 1024, o, sel);
    }
    free(sel);

    g_selAiBackup = (wchar_t *)malloc((e - s + 1) * sizeof(wchar_t));
    if (!g_selAiBackup) return;
    {
        wchar_t *all = NULL;
        int total = GetWindowTextLengthW(g_edit);
        all = (wchar_t *)malloc((total + 1) * sizeof(wchar_t));
        if (!all) { free(g_selAiBackup); g_selAiBackup = NULL; return; }
        GetWindowTextW(g_edit, all, total + 1);
        int bl = (int)(e - s);
        memcpy(g_selAiBackup, all + s, bl * sizeof(wchar_t));
        g_selAiBackup[bl] = 0;
        free(all);
    }
    g_selAiStart = s;
    g_selAiIns = s;
    g_selAi = TRUE;
    /* remove the selection; the answer streams in its place */
    SendMessageW(g_edit, EM_SETSEL, s, e);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)L"");
    StartAi(q);
}

static void ShowSelAiMenu(void)
{
    DWORD s, e;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    if (e <= s) {
        MessageBoxW(g_hwnd, L"请先选中要处理的文字，再按 Ctrl+J。",
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }
    static const struct { const wchar_t *label; const wchar_t *instr; }
    cmds[] = {
        { L"润色",         L"请润色并直接输出改进后的文字：" },
        { L"翻译成中文",   L"请翻译成中文并直接输出译文：" },
        { L"翻译成英文",   L"请翻译成英文并直接输出译文：" },
        { L"总结",         L"请用一两句话总结以下内容：" },
        { L"解释",         L"请解释以下内容：" },
    };
    HMENU pm = CreatePopupMenu();
    for (int i = 0; i < 5; i++)
        AppendMenuW(pm, MF_STRING, 500 + i, cmds[i].label);
    AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(pm, MF_STRING, 506, L"自定义…");
    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(pm, TPM_LEFTALIGN | TPM_RIGHTBUTTON
                                | TPM_RETURNCMD, pt.x, pt.y, 0, g_hwnd,
                             NULL);
    DestroyMenu(pm);
    if (cmd >= 500 && cmd < 505) {
        StartSelAi(cmds[cmd - 500].instr);
    } else if (cmd == 506) {
        wchar_t buf[256] = L"";
        if (PromptInput(L"自定义 AI 指令", buf, 256))
            StartSelAi(buf);
    }
}

static void StartAi(const wchar_t *question)
{
    AiJob *job = (AiJob *)malloc(sizeof(AiJob));
    if (!job) return;
    lstrcpynW(job->base, g_aiBase, 256);
    lstrcpynW(job->model, g_aiModel, 128);
    lstrcpynW(job->key, g_aiKey, 256);
    lstrcpynW(job->sys, g_aiSys, 1024);
    lstrcpynW(job->q, question, 1024);
    job->timeout = g_aiTimeout;

    /* remember this turn; the answer accumulates via WM_AI_CHUNK */
    lstrcpynW(g_aiLastQ, question, 1024);
    if (!g_aiLastA)
        g_aiLastA = (wchar_t *)malloc(2410 * sizeof(wchar_t));
    if (g_aiLastA) { g_aiLastA[0] = 0; g_aiLastALen = 0; }

    if (!g_selAi)
        TaskBegin(0, question);   /* anchor + leading separator */

    g_aiBusy = TRUE;
    g_aiStartTick = GetTickCount();
    g_stBusySec = -1;
    InvalidateRect(g_hwnd, NULL, FALSE);

    HANDLE th = CreateThread(NULL, 0, AiThreadProc, job, 0, NULL);
    if (th) CloseHandle(th);
    else {
        free(job);
        g_aiBusy = FALSE;
        TaskFinish(0, L"无法启动任务");
    }
}

static void AiCancel(void)
{
    if (!g_aiBusy) return;
    InterlockedExchange((volatile LONG *)&g_aiCancelFlag, 1);
    /* the worker notices the flag between reads; also close the request
     * so a blocked read unblocks immediately */
    if (g_aiReq) {
        WinHttpCloseHandle((HINTERNET)g_aiReq);
        g_aiReq = NULL;
    }
}

/* flush batched AI text into the editor in one shot */
static void AiFlushPending(void)
{
    KillTimer(g_hwnd, TIMER_AICHUNK);
    if (!g_aiPendingLen || !g_aiPending) {
        g_aiPendingLen = 0;
        return;
    }
    if (g_selAi)
        SelAiInsert(g_aiPending);
    else
        TaskInsert(&g_tasks[0], g_aiPending);
    g_aiPendingLen = 0;
    if (g_aiPending) g_aiPending[0] = 0;
}

static void AiCancel(void);
static void AiFlushPending(void);

/* ------------------------------------------------------------------ */
/* Agent: "opencode run <prompt>" as a background task                  */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t exe[512];    /* resolved opencode path (unquoted) */
    BOOL    useCmd;      /* .cmd/.bat needs cmd.exe wrapper */
    wchar_t prompt[2048];
    wchar_t cwd[MAX_PATH];
    int timeout;
} AgJob;

/* wide-char append with bounds check (wsprintfW caps at 1024 chars) */
static int Wa(wchar_t *dst, int cap, int off, const wchar_t *s)
{
    while (*s && off < cap - 1) dst[off++] = *s++;
    return off;
}

/* strip ANSI escape sequences (CSI/OSC/2-char) in place */
static void StripAnsiW(wchar_t *s)
{
    int i = 0, o = 0;
    while (s[i]) {
        if (s[i] == 27 && s[i + 1] == L'[') {
            i += 2;
            while (s[i] && !(s[i] >= L'@' && s[i] <= L'~')) i++;
            if (s[i]) i++;
        } else if (s[i] == 27 && s[i + 1] == L']') {
            i += 2;
            while (s[i] && s[i] != 7
                   && !(s[i] == 27 && s[i + 1] == L'\\')) i++;
            if (s[i] == 7) i++;
            else if (s[i] == 27) i += 2;
        } else if (s[i] == 27) {
            i += 2;
        } else {
            s[o++] = s[i++];
        }
    }
    s[o] = 0;
}

/* resolve the opencode executable.
 * returns 0 = direct exe, 1 = script needing cmd.exe, -1 = not found.
 * out is filled with the unquoted path. */
static int ResolveOpenCode(wchar_t *out, int cch)
{
    out[0] = 0;
    wchar_t raw[512];
    lstrcpynW(raw, g_agPath, 512);
    int len = lstrlenW(raw);
    if (len >= 2 && raw[0] == L'"' && raw[len - 1] == L'"') {
        raw[len - 1] = 0;
        for (int i = 0; raw[i + 1]; i++) raw[i] = raw[i + 1];
        raw[len - 2] = 0;
    }
    if (raw[0]) {
        lstrcpynW(out, raw, cch);
        len = lstrlenW(out);
        int s = len - 4;
        if (s >= 0) {
            wchar_t lo[5];
            for (int i = 0; i < 4; i++)
                lo[i] = (out[s + i] >= L'A' && out[s + i] <= L'Z')
                        ? out[s + i] + 32 : out[s + i];
            lo[4] = 0;
            if (!lstrcmpW(lo, L".cmd") || !lstrcmpW(lo, L".bat"))
                return 1;
        }
        return 0;
    }
    /* no user path: search PATH dirs only (skip cwd to avoid hijack).
     * npm installs opencode as a .cmd shim on windows */
    wchar_t env[2048];
    DWORD pn = GetEnvironmentVariableW(L"PATH", env, 2048);
    if (pn == 0 || pn >= 2048) return -1;
    static const wchar_t *names[3] =
        { L"opencode.exe", L"opencode.cmd", L"opencode.bat" };
    for (int i = 0; i < 3; i++) {
        wchar_t f[MAX_PATH];
        if (SearchPathW(env, names[i], NULL, MAX_PATH, f, NULL)) {
            lstrcpynW(out, f, cch);
            return i == 0 ? 0 : 1;
        }
    }
    return -1;
}

static void AgPost(const wchar_t *txt)
{
    int n = lstrlenW(txt);
    wchar_t *heap = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!heap) return;
    memcpy(heap, txt, (n + 1) * sizeof(wchar_t));
    PostMessageW(g_hwnd, WM_AG_CHUNK, n, (LPARAM)heap);
}

static void AgPostDone(const wchar_t *err)
{
    int n = err ? lstrlenW(err) : 0;
    wchar_t *heap = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!heap) return;
    if (err) memcpy(heap, err, (n + 1) * sizeof(wchar_t));
    else heap[0] = 0;
    PostMessageW(g_hwnd, WM_AG_DONE, err ? 0 : 1, (LPARAM)heap);
}

static DWORD WINAPI AgThreadProc(LPVOID param)
{
    AgJob *job = (AgJob *)param;

    /* command line:
     *   direct: "<exe>" run "<prompt>"
     *   script: "<sysdir>\cmd.exe" /d /s /c ""<exe>" run "<prompt>>"
     * cmd.exe cannot run .cmd/.bat via CreateProcess directly; prompt
     * quotes doubled and newlines flattened for the script case */
    wchar_t cmd[3072];
    int co = 0;
    if (job->useCmd) {
        wchar_t sysdir[MAX_PATH];
        GetSystemDirectoryW(sysdir, MAX_PATH);
        cmd[co++] = L'"';
        co = Wa(cmd, 3072, co, sysdir);
        co = Wa(cmd, 3072, co, L"\\cmd.exe\" /d /s /c \"");
        cmd[co++] = L'"';
        co = Wa(cmd, 3072, co, job->exe);
        co = Wa(cmd, 3072, co, L"\" run \"");
    } else {
        cmd[co++] = L'"';
        co = Wa(cmd, 3072, co, job->exe);
        co = Wa(cmd, 3072, co, L"\" run \"");
    }
    for (const wchar_t *p = job->prompt; *p && co < 3000; p++) {
        if (*p == L'"' && job->useCmd) { cmd[co++] = L'"'; cmd[co++] = L'"'; }
        else if ((*p == L'\n' || *p == L'\r') && job->useCmd)
            cmd[co++] = L' ';
        else cmd[co++] = *p;
    }
    cmd[co++] = L'"';
    if (job->useCmd) cmd[co++] = L'"';
    cmd[co] = 0;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL, rdErr = NULL, wrErr = NULL, hNul = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        free(job); AgPostDone(L"无法创建管道"); return 0;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&rdErr, &wrErr, &sa, 0)) {
        CloseHandle(rd); CloseHandle(wr);
        free(job); AgPostDone(L"无法创建管道"); return 0;
    }
    SetHandleInformation(rdErr, HANDLE_FLAG_INHERIT, 0);
    /* stdin must be a valid handle: node/bun runtimes probe it; NULL
     * makes some builds die silently. NUL device reads EOF forever. */
    hNul = CreateFileW(L"NUL", GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       OPEN_EXISTING, 0, NULL);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (hNul != INVALID_HANDLE_VALUE) ? hNul : NULL;
    si.hStdOutput = wr;
    si.hStdError = wrErr;   /* collect diagnostics (gui proc has none) */

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL,
                        job->cwd[0] ? job->cwd : NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        CloseHandle(rdErr); CloseHandle(wrErr);
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        free(job); AgPostDone(L"启动 opencode 失败（检查路径配置）");
        return 0;
    }
    g_agProc = pi.hProcess;
    CloseHandle(pi.hThread);
    CloseHandle(wr);        /* parent must close its write end */
    CloseHandle(wrErr);

    /* read stdout as UTF-8, forward as chunks; accumulate stderr for
     * diagnostics when the agent produces no output */
    char buf[8192];
    DWORD rdBytes = 0;
    wchar_t wbuf[8192];
    char errAcc[8192];
    int errLen = 0;
    BOOL timedOut = FALSE;
    DWORD startTick = GetTickCount();
    int got = 0;
    /* UTF-8 sequences can split across pipe reads; carry the tail */
    char carry[4];
    int carryN = 0;
    for (;;) {
        if (g_agCancel) { TerminateProcess(pi.hProcess, 1); timedOut = 2; break; }
        DWORD avail = 0;
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            int prep = carryN;
            if (prep) { memcpy(buf, carry, prep); carryN = 0; }
            DWORD want = avail < (DWORD)(8191 - prep) ? avail
                                                      : (DWORD)(8191 - prep);
            if (!ReadFile(rd, buf + prep, want, &rdBytes, NULL) || !rdBytes)
                break;
            int total = prep + (int)rdBytes;
            /* stash a partial trailing sequence for the next round */
            if ((buf[total - 1] & 0xC0) == 0x80) {
                int q = total - 1, back = 0;
                while (q >= 0 && (buf[q] & 0xC0) == 0x80 && back < 3)
                    { q--; back++; }
                int keep = 0;
                if (q >= 0 && (buf[q] & 0x80)) {
                    int need = (buf[q] & 0xE0) == 0xC0 ? 2
                             : (buf[q] & 0xF0) == 0xE0 ? 3
                             : (buf[q] & 0xF8) == 0xF0 ? 4 : 0;
                    if (need && total - q < need) keep = total - q;
                }
                if (keep) { memcpy(carry, buf + total - keep, keep);
                            carryN = keep; total -= keep; }
            }
            int wn = MultiByteToWideChar(CP_UTF8, 0, buf, total,
                                         wbuf, 8191);
            if (wn > 0) {
                wbuf[wn] = 0;
                StripAnsiW(wbuf);
                if (wbuf[0]) { AgPost(wbuf); got += wn; }
            }
            continue;
        }
        DWORD availE = 0;
        if (PeekNamedPipe(rdErr, NULL, 0, NULL, &availE, NULL)
            && availE > 0) {
            DWORD want = availE < 1024 ? availE : 1024;
            if (!ReadFile(rdErr, buf, want, &rdBytes, NULL) || !rdBytes)
                break;
            int n = (int)rdBytes;
            if (n > 8191 - errLen) n = 8191 - errLen;
            if (n > 0) { memcpy(errAcc + errLen, buf, n); errLen += n; }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) break;
        if (job->timeout > 0
            && GetTickCount() - startTick > (DWORD)job->timeout * 1000) {
            TerminateProcess(pi.hProcess, 1);
            timedOut = 1;
            break;
        }
    }
    /* final drain: blocking reads until the pipe breaks (EOF). this is
     * the only reliable way to collect data still buffered in the pipe
     * when the process-handle signaled early; on the timeout/cancel
     * paths the child was already terminated, so the pipes break at
     * once */
    for (;;) {
        int prep = carryN;
        if (prep) { memcpy(buf, carry, prep); carryN = 0; }
        if (!ReadFile(rd, buf + prep, 8191 - prep, &rdBytes, NULL)
            || rdBytes == 0)
            break;
        int total = prep + (int)rdBytes;
        int wn = MultiByteToWideChar(CP_UTF8, 0, buf, total, wbuf, 8191);
        if (wn > 0) {
            wbuf[wn] = 0;
            StripAnsiW(wbuf);
            if (wbuf[0]) { AgPost(wbuf); got += wn; }
        }
    }
    for (;;) {
        if (!ReadFile(rdErr, buf, 2048, &rdBytes, NULL) || rdBytes == 0)
            break;
        int n = (int)rdBytes;
        if (n > 8191 - errLen) n = 8191 - errLen;
        if (n > 0) { memcpy(errAcc + errLen, buf, n); errLen += n; }
    }

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(rd);
    CloseHandle(rdErr);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);
    g_agProc = NULL;

    /* no output (or timed out)? surface the agent's own diagnostics so
     * failures are visible (auth errors, missing model, crash logs...) */
    if ((got == 0 || timedOut == 1) && errLen > 0) {
        int wn = MultiByteToWideChar(CP_UTF8, 0, errAcc, errLen,
                                     wbuf, 8191);
        if (wn > 0) {
            wbuf[wn] = 0;
            StripAnsiW(wbuf);
            AgPost(timedOut == 1 ? L"超时前 Agent 错误输出：\n"
                                 : L"Agent 未产生输出，其错误输出如下：\n");
            AgPost(wbuf);
            AgPost(L"\n");
        }
    }

    wchar_t err[128];
    err[0] = 0;
    if (timedOut == 1)
        wsprintfW(err, L"Agent 执行超时（上限 %d 秒）", job->timeout);
    else if (timedOut == 2) lstrcpynW(err, L"已取消", 128);
    else if (got == 0)
        lstrcpynW(err, errLen ? L"Agent 无输出（见错误信息）"
                              : L"Agent 无输出", 128);
    AgPostDone(err[0] ? err : NULL);
    free(job);
    return 0;
}

static void StartAgent(const wchar_t *question)
{
    InterlockedExchange((volatile LONG *)&g_agCancel, 0);
    AgJob *job = (AgJob *)malloc(sizeof(AgJob));
    if (!job) return;
    int mode = ResolveOpenCode(job->exe, 512);
    if (mode < 0) {
        free(job);
        TaskBegin(1, question);
        TaskFinish(1, L"未找到 opencode（请安装或在设置中填写路径）");
        return;
    }
    job->useCmd = (mode == 1);
    job->timeout = g_agTimeout > 0 ? g_agTimeout : 60;
    /* prompt = agent system prompt prepended, then the user question */
    if (g_agSys[0])
        wsprintfW(job->prompt, L"%s\r\n\r\n%s", g_agSys, question);
    else
        lstrcpynW(job->prompt, question, 2048);
    /* run in the directory of the open document (agent needs the project
     * context); fall back to the user profile dir for unsaved docs */
    if (g_path[0]) {
        lstrcpynW(job->cwd, g_path, MAX_PATH);
        wchar_t *slash = wcsrchr(job->cwd, L'\\');
        if (slash) *slash = 0;
        else job->cwd[0] = 0;
    } else
        lstrcpynW(job->cwd, L".", MAX_PATH);

    TaskBegin(1, question);

    g_agBusy = TRUE;
    g_agStartTick = GetTickCount();
    g_stBusySec = -1;
    InvalidateRect(g_hwnd, NULL, FALSE);

    HANDLE th = CreateThread(NULL, 0, AgThreadProc, job, 0, NULL);
    if (th) CloseHandle(th);
    else {
        free(job);
        g_agBusy = FALSE;
        TaskFinish(1, L"无法启动任务");
    }
}

static void AgCancel(void)
{
    if (!g_agBusy) return;
    InterlockedExchange((volatile LONG *)&g_agCancel, 1);
}

static void AgFlushPending(void)
{
    KillTimer(g_hwnd, TIMER_AICHUNK);
    if (!g_agPendingLen || !g_agPending) {
        g_agPendingLen = 0;
        return;
    }
    TaskInsert(&g_tasks[1], g_agPending);
    g_agPendingLen = 0;
    if (g_agPending) g_agPending[0] = 0;
}


/* ------------------------------------------------------------------ */
/* editor subclass                                                      */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEWHEEL && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(g_hwnd, WM_EDITCMD, IDM_ZOOM,
                     ((short)HIWORD(wp) > 0) ? 1 : -1);
        return 0;
    }

    if (msg == WM_KEYDOWN) {
        BOOL ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && wp == 'S') {
            BOOL shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            SendMessageW(g_hwnd, WM_EDITCMD,
                         shift ? IDM_SAVEAS : IDM_SAVE, 0);
            return 0;
        }
        if (ctrl && wp == 'O') {
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_OPEN, 0);
            return 0;
        }
        if (ctrl && wp == 'N') {
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_NEW, 0);
            return 0;
        }
        if (ctrl && wp == 'F') {
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_FIND, 0);
            return 0;
        }
        if (ctrl && wp == 'P') {
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_OUTLINE, 0);
            return 0;
        }
        if (ctrl && wp == 'J' && !g_aiBusy && !g_agBusy) {
            ShowSelAiMenu();
            return 0;
        }
        if (ctrl && wp == VK_OEM_2) { /* Ctrl+/ */
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_TOGGLE, 0);
            return 0;
        }
        if (ctrl && wp == VK_OEM_COMMA) { /* Ctrl+, settings */
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_SETTINGS, 0);
            return 0;
        }
        if (ctrl && wp == 'A') {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        if (ctrl && wp == 'C') { SendMessageW(h, WM_COPY, 0, 0); return 0; }
        if (ctrl && wp == 'X') { SendMessageW(h, WM_CUT, 0, 0);  return 0; }
        if (ctrl && wp == 'V') { SendMessageW(h, WM_PASTE, 0, 0); return 0; }
        if (wp == VK_TAB) return 0; /* handled in WM_CHAR */
        if (wp == VK_ESCAPE && (g_aiBusy || g_agBusy)) {
            if (g_aiBusy) AiCancel();
            if (g_agBusy) AgCancel();
            return 0;
        }
        if (wp == VK_RETURN && !ctrl) {
            /* "/q" -> AI chat, "//q" -> agent; both run in background
             * and insert their answer right under the question line */
            DWORD s0, e0;
            SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
            if (s0 == e0) {
                int li = (int)SendMessageW(h, EM_LINEFROMCHAR, s0, 0);
                int ls = (int)SendMessageW(h, EM_LINEINDEX, li, 0);
                int ll = (int)SendMessageW(h, EM_LINELENGTH, ls, 0);
                if (ll >= 2 && ll < 1024) {
                    wchar_t buf[1026];
                    buf[0] = 1024;
                    int n = (int)SendMessageW(h, EM_GETLINE, li,
                                              (LPARAM)buf);
                    if (n > 0) buf[n] = 0; else buf[0] = 0;
                    if (buf[0] == L'/' && buf[1] == L'/') {
                        /* agent command */
                        if (!buf[2]) return 0;
                        if (g_agBusy) {
                            MessageBoxW(g_hwnd,
                                L"Agent 任务进行中，按 Esc 可取消。",
                                APP_NAME, MB_ICONINFORMATION);
                            return 0;
                        }
                        StartAgent(buf + 2);
                        return 0;
                    }
                    if (buf[0] == L'/') {
                        if (!g_aiBase[0] || !g_aiModel[0]) {
                            MessageBoxW(g_hwnd,
                                L"未配置 AI：请先在「配置」中填写"
                                L" Base URL 与模型。",
                                APP_NAME, MB_ICONINFORMATION);
                            return 0;
                        }
                        if (!buf[1]) return 0;
                        if (g_aiBusy) {
                            MessageBoxW(g_hwnd,
                                L"AI 任务进行中，按 Esc 可取消。",
                                APP_NAME, MB_ICONINFORMATION);
                            return 0;
                        }
                        StartAi(buf + 1);
                        return 0; /* we manage the newline ourselves */
                    }
                }
            }
        }
    }
    else if (msg == WM_CHAR) {
        if (wp == VK_TAB && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"    ");
            return 0;
        }
        /* Enter: inherit leading blanks from the current line */
        if (wp == L'\r' && g_indentRet
            && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            DWORD s0, e0;
            SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
            int li = (int)SendMessageW(h, EM_LINEFROMCHAR, s0, 0);
            int ls = (int)SendMessageW(h, EM_LINEINDEX, li, 0);
            int llen = (int)SendMessageW(h, EM_LINELENGTH, s0, 0);
            wchar_t lbuf[512];
            *(LPWORD)lbuf = (WORD)(sizeof(lbuf) / sizeof(wchar_t));
            int got = (int)SendMessageW(h, EM_GETLINE, li,
                                        (LPARAM)lbuf);
            if (got > 0) lbuf[got] = 0; else lbuf[0] = 0;
            int pre = 0;
            while (lbuf[pre] == L' ' || lbuf[pre] == L'\t') pre++;
            if (pre > 0 && pre >= got) {
                /* blank indented line: clear it, raw newline */
                SendMessageW(h, EM_SETSEL, ls, ls + llen);
                SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"\r\n");
                SendMessageW(h, EM_SETSEL, ls + 2, ls + 2);
                return 0;
            }
            if (pre > 0 && pre < 64) {
                wchar_t ins[80];
                ins[0] = L'\r'; ins[1] = L'\n';
                for (int k = 0; k < pre; k++)
                    ins[2 + k] = lbuf[k];
                ins[2 + pre] = 0;
                SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)ins);
                return 0;
            }
            /* fall through: default newline */
        }
        /* swallow control chars except backspace, enter, Ctrl+Z (undo) */
        if (wp < 32 && wp != VK_BACK && wp != '\r' && wp != '\n'
            && wp != VK_TAB && wp != 26)
            return 0;
    }
    return CallWindowProcW(g_editProc, h, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* main window proc                                                    */
/* ------------------------------------------------------------------ */

/* clickable links in preview (item 49) */
#define LINK_MAX 256
static struct { RECT rc; wchar_t url[520]; } g_links[LINK_MAX];
static int g_linkN;

static void LinkSink(void *ctx, RECT rc, const wchar_t *url, int urlLen)
{
    (void)ctx;
    if (g_linkN >= LINK_MAX) return;
    int n = urlLen < 519 ? urlLen : 519;
    for (int i = 0; i < n; i++) g_links[g_linkN].url[i] = url[i];
    g_links[g_linkN].url[n] = 0;
    g_links[g_linkN].rc = rc;
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

static void OpenLink(int i)
{
    if (i < 0 || i >= g_linkN) return;
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
        g_edit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE
            | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
            0, 0, 0, 0, hwnd, (HMENU)1, NULL, NULL);
        /* multiline edit caps at 64K chars by default; long AI sessions
         * would silently drop inserts past the limit */
        SendMessageW(g_edit, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
        g_editProc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC,
                                                (LONG_PTR)EditProc);
        HDC dc = GetDC(hwnd);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(hwnd, dc);
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
            md_paint(&g_doc, mem, &rcBody, g_scrollY, &g_fonts);
        } else if (g_view == VIEW_SPLIT) {
            RECT rcPrev;
            PreviewRect(&rcPrev);
            RECT rcWhite = { rcPrev.left, rcPrev.top, rcPrev.right + 1,
                             rcPrev.bottom };
            FillRect(mem, &rcWhite, (HBRUSH)GetStockObject(WHITE_BRUSH));
            md_paint(&g_doc, mem, &rcPrev, g_scrollY, &g_fonts);
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
        if (g_findShown) DrawFindBar(mem, &rcClient);
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
        case IDM_TOGGLE:   SetView((g_view + 1) % 3); return 0;
        case IDM_FIND:     ShowFindBar();           return 0;
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
        case IDM_EXPORTTXT:  DoPlainExport();       return 0;
        case IDM_PRINT:      DoPrint();             return 0;
        case IDM_TOPMOST:
            SetTopmost(!g_topmost);
            SaveSettings();
            return 0;
        case IDM_HELP:       ShowHelp();            return 0;
        }
        return 0;

    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            if (wp == 'S') { SendMessageW(hwnd, WM_EDITCMD, IDM_SAVE, 0); return 0; }
            if (wp == 'O') { SendMessageW(hwnd, WM_EDITCMD, IDM_OPEN, 0); return 0; }
            if (wp == 'N') { SendMessageW(hwnd, WM_EDITCMD, IDM_NEW, 0);  return 0; }
            if (wp == 'F') { SendMessageW(hwnd, WM_EDITCMD, IDM_FIND, 0); return 0; }
            if (wp == VK_OEM_COMMA) { SendMessageW(hwnd, WM_EDITCMD, IDM_SETTINGS, 0); return 0; }
            if (wp == VK_OEM_2) { SendMessageW(hwnd, WM_EDITCMD, IDM_TOGGLE, 0); return 0; }
        }
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
        break;

    case WM_LBUTTONDOWN: {
        int px = (short)LOWORD(lp);
        int py = (short)HIWORD(lp);
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
        else if (id == 2) ShowHistoryMenu();
        else if (id == 3) ShowSettings();
        else if (id == 4) ShowMoreMenu();
        else if (id >= 10 && id <= 12) SetView(id - 10);
        return 0;
    }

    case WM_SETCURSOR:
        if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if ((g_view == VIEW_PREVIEW || g_view == VIEW_SPLIT)
                && HitLink(pt.x, pt.y) >= 0) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_MOUSEMOVE: {
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
        int id = HitTestHeader((POINT){ (short)LOWORD(lp), (short)HIWORD(lp) });
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

    case WM_LBUTTONUP:
        if (g_splitDrag) {
            g_splitDrag = FALSE;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSELEAVE:
        if (g_hoverId != -1) {
            g_hoverId = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
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
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            SendMessageW(hwnd, WM_EDITCMD, IDM_ZOOM,
                         ((short)HIWORD(wp) > 0) ? 1 : -1);
            return 0;
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
            }
            return 0;
        }
        return 0;

    case WM_AI_CHUNK: {
        wchar_t *txt = (wchar_t *)lp;
        int n = (int)wp;
        if (g_aiBusy && txt && n > 0) {
            if (g_aiPendingLen + n + 1 > g_aiPendingCap) {
                int cap = g_aiPendingCap ? g_aiPendingCap : 4096;
                while (cap < g_aiPendingLen + n + 1) cap *= 2;
                wchar_t *nb = (wchar_t *)realloc(g_aiPending,
                                                 cap * sizeof(wchar_t));
                if (nb) { g_aiPending = nb; g_aiPendingCap = cap; }
            }
            if (g_aiPending && g_aiPendingLen + n < g_aiPendingCap) {
                memcpy(g_aiPending + g_aiPendingLen, txt,
                       n * sizeof(wchar_t));
                g_aiPendingLen += n;
                g_aiPending[g_aiPendingLen] = 0;
            }
            /* batch for 70ms: enough to merge multi-token bursts,
             * short enough to feel like live streaming */
            SetTimer(hwnd, TIMER_AICHUNK, 70, NULL);
            /* accumulate for multi-turn context (capped) */
            if (g_aiLastA && g_aiLastALen < 2400) {
                int room = 2400 - g_aiLastALen;
                int cp = n < room ? n : room;
                memcpy(g_aiLastA + g_aiLastALen, txt,
                       cp * sizeof(wchar_t));
                g_aiLastALen += cp;
                g_aiLastA[g_aiLastALen] = 0;
            }
        }
        free(txt);
        return 0;
    }

    case WM_AI_DONE: {
        wchar_t *err = (wchar_t *)lp;
        g_aiBusy = FALSE;
        KillTimer(hwnd, TIMER_AICHUNK);
        AiFlushPending();
        if (g_selAi) {
            SelAiFinish(err);
        } else {
            /* successful turns become context for the next question */
            if ((!err || !err[0]) && g_aiLastQ[0] && g_aiLastA
                && g_aiLastA[0])
                StoreAiTurn(g_aiLastQ, g_aiLastA);
            TaskFinish(0, err);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        free(err);
        return 0;
    }

    case WM_AG_CHUNK: {
        wchar_t *txt = (wchar_t *)lp;
        int n = (int)wp;
        if (g_agBusy && txt && n > 0) {
            if (g_agPendingLen + n + 1 > g_agPendingCap) {
                int cap = g_agPendingCap ? g_agPendingCap : 4096;
                while (cap < g_agPendingLen + n + 1) cap *= 2;
                wchar_t *nb = (wchar_t *)realloc(g_agPending,
                                                 cap * sizeof(wchar_t));
                if (nb) { g_agPending = nb; g_agPendingCap = cap; }
            }
            if (g_agPending && g_agPendingLen + n < g_agPendingCap) {
                memcpy(g_agPending + g_agPendingLen, txt,
                       n * sizeof(wchar_t));
                g_agPendingLen += n;
                g_agPending[g_agPendingLen] = 0;
            }
            SetTimer(hwnd, TIMER_AICHUNK, 70, NULL);
        }
        free(txt);
        return 0;
    }

    case WM_AG_DONE: {
        wchar_t *err = (wchar_t *)lp;
        g_agBusy = FALSE;
        KillTimer(hwnd, TIMER_AICHUNK);
        AgFlushPending();
        TaskFinish(1, err);
        InvalidateRect(hwnd, NULL, FALSE);
        free(err);
        return 0;
    }

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
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* settings dialog (hotkey + autosave)                                  */
/* ------------------------------------------------------------------ */

static HWND   g_setDlg;
static HFONT  g_setFont;            /* dialog text font (CJK-safe face) */
static HWND   g_hkEdit;
static WNDPROC g_hkProc;
static UINT   g_dlgMod, g_dlgVk;   /* dialog-local hotkey state */
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
                  WineEnv() ? L"WenQuanYi Micro Hei" : L"SimHei",
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
            { L"STATIC",  L"Open Code 路径（空 = 自动查找）", SS_LEFT,
              0, 24, 538, 260, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
              205, 24, 560, 264, 26 },
            { L"STATIC",  L"超时(秒)", SS_LEFT, 0, 300, 538, 70, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_NUMBER | WS_TABSTOP,
              207, 300, 560, 60, 26 },
            { L"STATIC",  L"Agent 提示词", SS_LEFT, 0, 24, 596, 130, 18 },
            { L"EDIT",    L"", WS_BORDER | ES_MULTILINE | ES_WANTRETURN
                              | WS_VSCROLL | WS_TABSTOP,
              206, 24, 618, 398, 44 },
            /* -- footer buttons -- */
            { L"BUTTON",  L"确定", BS_OWNERDRAW | BS_DEFPUSHBUTTON
                              | WS_TABSTOP,
              108, 238, 688, 88, 32 },
            { L"BUTTON",  L"取消", BS_OWNERDRAW | WS_TABSTOP,
              109, 334, 688, 88, 32 },
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
    int dw = SC(446), dh = SC(732)
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
        InvalidateRect(g_hwnd, NULL, FALSE);
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
static void PathHash(const wchar_t *path, wchar_t *out /*9 chars*/)
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
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
