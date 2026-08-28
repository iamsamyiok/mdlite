/* MDLite - HTML / plain-text export & printing */
#define _WIN32_WINNT 0x0601
#include "mdlite.h"
#include "markdown.h"
#include <stdlib.h>
#include <string.h>

/* ---- HTML export / copy (item 10) ---- */

char *BuildHtml(int *outLen)
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

void CopyHtml(void)
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

void ExportHtml(void)
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

void DoPlainExport(void)
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
/* ---- print / PDF export (item 29) ---- */

void DoPrint(void)
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