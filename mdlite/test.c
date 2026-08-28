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
    md_build(&doc, L"a\r\n\r\nb\r\n", 8, &f, dc, 700);
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

    /* ---- tables ---- */
    const wchar_t *tbl =
        L"| Name | Qty | Price |\n"
        L"|------|:---:|------:|\n"
        L"| apple | 2 | 1.50 |\n"
        L"| banana | 12 | 0.25 |\n"
        L"\n"
        L"after table\n";
    md_build(&doc, tbl, lstrlenW(tbl), &f, dc, 700);
    int rows = 0;
    MDLine *hdr = NULL;
    for (int i = 0; i < doc.nlines; i++)
        if (doc.lines[i].type == LT_TABLEROW) {
            if (!hdr) hdr = &doc.lines[i];
            rows++;
        }
    expect(rows == 3, "table: 3 rows (header + 2 body)");
    expect(hdr && hdr->isHeader && hdr->ncells == 3,
           "table: header row with 3 cells");
    expect(hdr && hdr->aligns[0] == 0 && hdr->aligns[1] == 1
           && hdr->aligns[2] == 2, "table: left/center/right alignment");
    int widthsFit = 1;
    for (int i = 0; i < doc.nlines; i++)
        if (doc.lines[i].type == LT_TABLEROW) {
            int sum = 0;
            for (int c = 0; c < doc.lines[i].ncells; c++)
                sum += doc.lines[i].colW[c];
            if (sum > 700 - 48 + 8) widthsFit = 0; /* content width */
        }
    expect(widthsFit, "table: column widths fit content width");
    /* cell text points into doc buffer, subs laid out */
    int cellOk = 1;
    for (int i = 0; i < doc.nlines && cellOk; i++)
        if (doc.lines[i].type == LT_TABLEROW)
            for (int c = 0; c < doc.lines[i].ncells && cellOk; c++) {
                MDCell *C = &doc.lines[i].cells[c];
                if (C->len < 0) cellOk = 0;
                for (int k = 0; k < C->nsubs && cellOk; k++)
                    for (int r2 = 0; r2 < C->subs[k].nruns; r2++)
                        if (C->subs[k].runs[r2].w < 0) cellOk = 0;
            }
    expect(cellOk, "table: cells well-formed, run widths cached");
    md_free(&doc);

    /* table without leading bar + ragged rows */
    const wchar_t *tbl2 =
        L"a | b\n"
        L"--- | ---\n"
        L"1 |\n"
        L"| 2 | 3 | 4 |\n";
    md_build(&doc, tbl2, lstrlenW(tbl2), &f, dc, 700);
    rows = 0;
    hdr = NULL;
    for (int i = 0; i < doc.nlines; i++)
        if (doc.lines[i].type == LT_TABLEROW) {
            if (!hdr) hdr = &doc.lines[i];
            rows++;
        }
    expect(rows == 3, "table: no-leading-bar + ragged rows parsed");
    expect(hdr && hdr->ncells == 2, "table: column count from header");
    md_free(&doc);

    /* lone pipe line is NOT a table (no delimiter row) */
    md_build(&doc, L"| not a table\nplain\n", 20, &f, dc, 700);
    expect(doc.nlines == 2 && doc.lines[0].type == LT_TEXT,
           "table: pipe line without delimiter stays text");
    md_free(&doc);

    /* ---- task lists ---- */
    const wchar_t *taskmd =
        L"- [ ] todo item\n"
        L"- [x] done item\n"
        L"- [X] upper X done\n"
        L"- plain item\n";
    md_build(&doc, taskmd, lstrlenW(taskmd), &f, dc, 700);
    expect(doc.nlines == 4 && doc.lines[0].task == 1
           && doc.lines[1].task == 2 && doc.lines[2].task == 2
           && doc.lines[3].task == 0, "task list markers detected");
    md_free(&doc);

    /* ---- code block extents cached ---- */
    const wchar_t *codemd = L"```\nabc\n```\ntext\n";
    md_build(&doc, codemd, lstrlenW(codemd), &f, dc, 700);
    expect(doc.ncodeBlocks == 1 && doc.codeBlocks[0].top >= 0
           && doc.codeBlocks[0].bottom > doc.codeBlocks[0].top,
           "code block extents cached");
    md_free(&doc);
    md_build(&doc, L"```\nunterminated", 15, &f, dc, 700);
    expect(doc.ncodeBlocks == 1, "unterminated fence also cached");
    md_free(&doc);

    /* ---- html export: tables + tasks ---- */
    char *html = NULL;
    int hl = md_to_html(tbl, lstrlenW(tbl), &html);
    expect(hl > 0 && html != NULL, "html: generated");
    int hasTable = 0, hasTh = 0, hasCenter = 0, hasRight = 0, hasTd = 0;
    if (html) {
        for (int i = 0; i + 6 < hl; i++) {
            if (!strncmp(html + i, "<table", 6)) hasTable = 1;
            if (!strncmp(html + i, "<th", 3)) hasTh = 1;
            if (!strncmp(html + i, "<td", 3)) hasTd = 1;
            if (!strncmp(html + i, "text-align:center", 17)) hasCenter = 1;
            if (!strncmp(html + i, "text-align:right", 16)) hasRight = 1;
        }
    }
    expect(hasTable && hasTh && hasTd, "html: table emitted");
    expect(hasCenter && hasRight, "html: column alignment styles");
    free(html);

    html = NULL;
    hl = md_to_html(taskmd, lstrlenW(taskmd), &html);
    int hasBox = 0, hasChecked = 0, hasTaskUl = 0;
    if (html) {
        for (int i = 0; i + 8 < hl; i++) {
            if (!strncmp(html + i, "type=\"checkbox\"", 15)) hasBox = 1;
            if (!strncmp(html + i, "disabled checked", 16)) hasChecked = 1;
            if (!strncmp(html + i, "<ul class=\"task\"", 16)) hasTaskUl = 1;
        }
    }
    expect(hasBox && hasChecked && hasTaskUl, "html: task checkboxes");
    free(html);

    md_free_fonts(&f);
    ReleaseDC(0, dc);
    printf(g_fail ? "\nRESULT: FAIL\n" : "\nRESULT: ALL PASS\n");
    return g_fail;
}
