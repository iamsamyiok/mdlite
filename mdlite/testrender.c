/* render md_paint into a DIB and inspect pixels */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "markdown.h"

BOOL ReadAllBytes(const wchar_t *path, char **buf, int *len)
{
    *buf = NULL;
    *len = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 64 * 1024 * 1024) {
        CloseHandle(h);
        return FALSE;
    }
    char *b = (char *)malloc(size ? size : 1);
    DWORD got = 0;
    BOOL ok = b && ReadFile(h, b, size, &got, NULL) && got == size;
    CloseHandle(h);
    if (!ok) { free(b); return FALSE; }
    *buf = b;
    *len = (int)size;
    return TRUE;
}


static int g_fail = 0;
static void expect(int cond, const char *what)
{
    printf("%-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) g_fail = 1;
}

#define W 700
#define H 1400

static COLORREF px(const void *bits, int x, int y)
{
    const BYTE *p = (const BYTE *)bits + ((long)y * W + x) * 4;
    return RGB(p[2], p[1], p[0]);
}

static int count_color(const void *bits, int x0, int y0, int x1, int y1,
                       COLORREF c)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (px(bits, x, y) == c) n++;
    return n;
}

int main(void)
{
    MDFonts f;
    ZeroMemory(&f, sizeof(f));
    HDC sdc = GetDC(0);
    int dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    md_init_fonts(&f, sdc, dpi);
    printf("dpi=%d bodyH=%d monoH=%d h1H=%d\n", dpi, f.bodyH, f.monoH,
           f.hH[0]);
    expect(f.bodyH > 8 && f.bodyH < 40, "body font height sane (8..40px)");

    const wchar_t *md =
        L"# Big Title\n\ntext line one\n\n```\ncode line\n```\n";

    MDDoc doc;
    ZeroMemory(&doc, sizeof(doc));
    md_build(&doc, md, lstrlenW(md), &f, sdc, W);
    printf("doc: %d lines, height %d\n", doc.nlines, doc.height);
    for (int i = 0; i < doc.nlines; i++)
        printf("  line %d type=%d y=%d h=%d subs=%d\n", i,
               doc.lines[i].type, doc.lines[i].y, doc.lines[i].height,
               doc.lines[i].nsubs);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP dib = CreateDIBSection(0, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    HDC mem = CreateCompatibleDC(0);
    SelectObject(mem, dib);

    RECT rc = { 0, 0, W, H };
    md_paint(&doc, mem, &rc, 0, &f, NULL);

    int codeBg = count_color(bits, 40, 0, W - 40, H, RGB(0xF5,0xF5,0xF7));
    expect(codeBg > 200, "code background painted");
    int darkTop = count_color(bits, 24, 0, 400, 120, RGB(0x1D,0x1D,0x1F));
    expect(darkTop > 10, "heading text painted");

    /* heading must be rendered with the large heading font (not body font) */
    int y0 = -1, y1 = -1;
    for (int y = 0; y < 120; y++)
        for (int x = 24; x < 400; x++) {
            COLORREF c = px(bits, x, y);
            int lum = GetRValue(c) + GetGValue(c) + GetBValue(c);
            if (lum < 200) { /* dark glyph pixel (ClearType blends around it) */
                if (y0 < 0) y0 = y;
                y1 = y;
            }
        }
    expect(y1 - y0 >= 20, "heading glyph height >= 20px (big font used)");
    printf("  heading glyph y0=%d y1=%d span=%d\n", y0, y1, y1 - y0);
    DeleteDC(mem);
    DeleteObject(dib);
    md_free(&doc);

    /* ---- table + task list rendering ---- */
    const wchar_t *md2 =
        L"| H1 | H2 |\n|---|---|\n| a | b |\n\n"
        L"- [ ] open\n"
        L"- [x] done\n";
    ZeroMemory(&doc, sizeof(doc));
    md_build(&doc, md2, lstrlenW(md2), &f, sdc, W);
    int trows = 0;
    for (int i = 0; i < doc.nlines; i++)
        if (doc.lines[i].type == LT_TABLEROW) trows++;
    expect(trows == 2, "render doc: 2 table rows built");
    int tasks = 0;
    for (int i = 0; i < doc.nlines; i++)
        if (doc.lines[i].task) tasks++;
    expect(tasks == 2, "render doc: 2 task items built");

    bits = NULL;
    dib = CreateDIBSection(0, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    mem = CreateCompatibleDC(0);
    SelectObject(mem, dib);
    md_paint(&doc, mem, &rc, 0, &f, NULL);

    /* header row fill must appear (gray band wider than glyph noise) */
    int hdrFill = count_color(bits, 40, 0, W - 40, doc.height,
                              RGB(0xF5, 0xF5, 0xF7));
    expect(hdrFill > 200, "table header fill painted");
    /* accent-colored checkbox strokes must appear (g_colLink blue) */
    int acc = count_color(bits, 24, 0, W - 24, doc.height, RGB(0x00,0x7A,0xFF));
    expect(acc > 10, "task checkbox accent painted");

    DeleteDC(mem);
    DeleteObject(dib);
    md_free(&doc);

    /* ---- GitHub-style alert callout ---- */
    const wchar_t *mdA =
        L"> [!NOTE] 标题行\n> 正文内容\n\n普通段落\n";
    ZeroMemory(&doc, sizeof(doc));
    md_build(&doc, mdA, lstrlenW(mdA), &f, sdc, W);
    expect(doc.nlines >= 3
           && doc.lines[0].type == LT_QUOTE && doc.lines[0].alert == 1
           && doc.lines[1].type == LT_QUOTE && doc.lines[1].alert == 6,
           "callout: tag + body lines parsed");
    bits = NULL;
    dib = CreateDIBSection(0, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    mem = CreateCompatibleDC(0);
    SelectObject(mem, dib);
    md_paint(&doc, mem, &rc, 0, &f, NULL);
    /* NOTE background #ddf4ff must be painted as a card */
    int noteBg = count_color(bits, 24, 0, W - 24, doc.height,
                             RGB(0xDD, 0xF4, 0xFF));
    expect(noteBg > 300, "callout: NOTE card background painted");
    /* left accent bar #0969da */
    int noteBar = count_color(bits, 20, 0, 40, doc.height,
                              RGB(0x09, 0x69, 0xDA));
    expect(noteBar > 5, "callout: accent bar painted");
    DeleteDC(mem);
    DeleteObject(dib);
    md_free(&doc);

    /* HTML export must emit the alert div for all five types */
    {
        const wchar_t *mdH =
            L"> [!NOTE] a\n> [!TIP] b\n> [!IMPORTANT] c\n"
            L"> [!WARNING] d\n> [!CAUTION] e\n";
        char *html = NULL;
        int hn = md_to_html(mdH, lstrlenW(mdH), &html);
        int ok = hn > 0 && html != NULL;
        if (ok) {
            int divs = 0;
            for (char *p = html; (p = strstr(p, "<div class=\"alert"));
                 p++) divs++;
            ok = divs == 5 && strstr(html, "</div>") != NULL
                 && strstr(html, "t2") != NULL
                 && strstr(html, "t5") != NULL;
        }
        expect(ok, "html: five alert cards exported");
        free(html);
    }

    /* ---- highlighted line: "==full line==" yellow bar ---- */
    const wchar_t *mdL = L"==重点内容==\n\n普通段落\n";
    ZeroMemory(&doc, sizeof(doc));
    md_build(&doc, mdL, lstrlenW(mdL), &f, sdc, W);
    expect(doc.nlines >= 2
           && doc.lines[0].type == LT_TEXT && doc.lines[0].hlbar == 1
           && doc.lines[2].type == LT_TEXT && doc.lines[2].hlbar == 0,
           "hlbar: full-line highlight parsed");
    bits = NULL;
    dib = CreateDIBSection(0, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    mem = CreateCompatibleDC(0);
    SelectObject(mem, dib);
    md_paint(&doc, mem, &rc, 0, &f, NULL);
    int barPx = count_color(bits, 8, 0, W - 8, doc.height,
                            RGB(255, 244, 181));
    expect(barPx > 400, "hlbar: yellow bar painted");
    DeleteDC(mem);
    DeleteObject(dib);
    md_free(&doc);
    {
        const wchar_t *mdLH = L"==高亮行==\n";
        char *html = NULL;
        int hn2 = md_to_html(mdLH, lstrlenW(mdLH), &html);
        expect(hn2 > 0 && html && strstr(html, "<p class=\"hl\">"),
               "html: highlighted line exported");
        free(html);
    }

    /* ---- image rendering (requires i.png) ---- */
    const wchar_t *md3 = L"![x](i.png)\n";
    ZeroMemory(&doc, sizeof(doc));
    md_build(&doc, md3, lstrlenW(md3), &f, sdc, W);
    bits = NULL;
    dib = CreateDIBSection(0, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    mem = CreateCompatibleDC(0);
    SelectObject(mem, dib);
    md_paint(&doc, mem, &rc, 0, &f, L"");
    /* just verify no crash; image may fail if file absent */
    expect(doc.height > 0, "image doc renders without crash");
    DeleteDC(mem);
    DeleteObject(dib);
    md_free(&doc);

    md_free_fonts(&f);
    ReleaseDC(0, sdc);
    printf(g_fail ? "\nRESULT: FAIL\n" : "\nRESULT: ALL PASS\n");
    return g_fail;
}
