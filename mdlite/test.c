/* smoke test: exercise md_build / md_free without crashing */
#include <windows.h>
#include <stdio.h>
#include "markdown.h"

static int g_fail = 0;

static void expect(int cond, const char *what)
{
    printf("%-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) g_fail = 1;
}

int main(void)
{
    MDFonts f;
    ZeroMemory(&f, sizeof(f));
    HDC dc = GetDC(0);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    md_init_fonts(&f, dc, dpi);
    expect(f.body != NULL && f.mono != NULL && f.h[0] != NULL,
           "fonts created");

    const wchar_t *md =
        L"# Title\n"
        L"\n"
        L"plain **bold** *ital* `code` ~~del~~ [link](http://x) ![img](i.png)\n"
        L"very long line without spaces: "
        L"AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEEFFFFFFFFFF\n"
        L"\n"
        L"## Sub *head* **two**\n"
        L"### h3\n"
        L"#### h4\n"
        L"##### h5\n"
        L"###### h6\n"
        L"\n"
        L"- item one\n"
        L"- item two with **bold**\n"
        L"  - nested\n"
        L"1. first\n"
        L"2. second\n"
        L"\n"
        L"> quote line 1\n"
        L"> quote **bold** line 2\n"
        L"\n"
        L"---\n"
        L"\n"
        L"```c\n"
        L"int main(void) {\n"
        L"\treturn 0;\n"
        L"}\n"
        L"```\n"
        L"\n"
        L"unterminated code fence:\n"
        L"```js\n"
        L"let x = 1;\n";

    MDDoc doc;
    ZeroMemory(&doc, sizeof(doc));
    md_build(&doc, md, lstrlenW(md), &f, dc, 700);

    int cnt[16] = {0};
    for (int i = 0; i < doc.nlines; i++)
        cnt[doc.lines[i].type]++;
    printf("doc: %d lines, height %d px\n", doc.nlines, doc.height);

    expect(doc.nlines > 30, "line count plausible");
    expect(cnt[LT_H1] == 1 && cnt[LT_H2] == 1, "headings parsed");
    expect(cnt[LT_H3] == 1 && cnt[LT_H4] == 1 && cnt[LT_H5] == 1
           && cnt[LT_H6] == 1, "all heading levels");
    expect(cnt[LT_ITEM] == 3, "unordered items");
    expect(cnt[LT_OLITEM] == 2, "ordered items");
    expect(cnt[LT_QUOTE] == 2, "quote lines");
    expect(cnt[LT_HR] == 1, "horizontal rule");
    expect(cnt[LT_CODE] >= 3, "code lines (incl. unterminated fence)");
    expect(cnt[LT_CODEPAD] >= 3, "code padding lines");
    expect(doc.height > 500, "layout height computed");

    /* runs must point inside doc.text */
    int ok = 1;
    for (int i = 0; i < doc.nlines && ok; i++) {
        MDLine *L = &doc.lines[i];
        for (int k = 0; k < L->nsubs && ok; k++) {
            MDSub *S = &L->subs[k];
            for (int r = 0; r < S->nruns && ok; r++) {
                MDRun *R = &S->runs[r];
                if (R->len > 0
                    && (R->ptr < doc.text
                        || R->ptr + R->len > doc.text + lstrlenW(doc.text) + 4))
                    ok = 0;
            }
        }
    }
    expect(ok, "runs point inside doc buffer");

    /* tab expansion inside code fence */
    int tabOk = 1;
    for (int i = 0; i < doc.nlines; i++) {
        MDLine *L = &doc.lines[i];
        if (L->type == LT_CODE) {
            for (int k = 0; k < L->nsubs; k++)
                for (int r = 0; r < L->subs[k].nruns; r++) {
                    const wchar_t *p = L->subs[k].runs[r].ptr;
                    int len = L->subs[k].runs[r].len;
                    for (int c = 0; c < len; c++)
                        if (p[c] == L'\t') tabOk = 0;
                }
        }
    }
    expect(tabOk, "tabs expanded to spaces in code");

    /* empty doc */
    md_build(&doc, L"", 0, &f, dc, 700);
    expect(doc.nlines >= 1, "empty doc builds");

    /* CRLF input */
    md_build(&doc, L"a\r\n\r\nb\r\n", 7, &f, dc, 700);
    expect(doc.nlines == 3 && doc.lines[0].type == LT_TEXT
           && doc.lines[1].type == LT_BLANK && doc.lines[2].type == LT_TEXT,
           "CRLF handled");

    /* stress: many lines */
    wchar_t big[20000];
    int n = 0;
    for (int i = 0; i < 2000; i++) {
        big[n++] = L'-'; big[n++] = L' ';
        big[n++] = L'x'; big[n++] = L'\n';
    }
    md_build(&doc, big, n, &f, dc, 700);
    expect(doc.nlines >= 2000, "stress: 2000 list lines");
    md_free(&doc);

    md_free_fonts(&f);
    ReleaseDC(0, dc);
    printf(g_fail ? "\nRESULT: FAIL\n" : "\nRESULT: ALL PASS\n");
    return g_fail;
}
