/* smoke test: exercise md_build / md_free without crashing */
#include <windows.h>
#include <stdio.h>
#include "markdown.h"
#include "editlogic.h"

static int g_fail = 0;

static void expect(int cond, const char *what)
{
    printf("%-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) g_fail = 1;
}

/* markdown.c references gitlite.c's reader for share export; provide a
 * real minimal implementation here so the standalone tests can run. */
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

    /* ---- escapes / autolinks / highlights / nested quotes / emoji ---- */
    char *html = NULL;
    int hl = 0;
    const wchar_t *syn =
        L"\\*not italic\\* plain\n"
        L"<https://example.com/a?b=c> end\n"
        L"==highlighted== text\n"
        L"> outer\n"
        L">> inner\n"
        L"> back to one\n"
        L"a 😀 b\n";
    md_build(&doc, syn, lstrlenW(syn), &f, dc, 700);
    int escOk = 1, linkOk = 0, hlOk = 0;
    for (int i = 0; i < doc.nlines; i++) {
        MDLine *L2 = &doc.lines[i];
        for (int k = 0; k < L2->nsubs && L2->type == LT_TEXT; k++)
            for (int r2 = 0; r2 < L2->subs[k].nruns; r2++) {
                MDRun *R = &L2->subs[k].runs[r2];
                if (R->flags & RF_ITALIC) escOk = 0;
                if ((R->flags & RF_LINK) && R->url
                    && !wcsncmp(R->url, L"https://", 8)) linkOk = 1;
                if (R->flags & RF_HL) hlOk = 1;
            }
    }
    expect(escOk, "escape: backslash prevents emphasis");
    expect(linkOk, "autolink <https://...> becomes link run");
    expect(hlOk, "==mark== becomes highlight run");
    int qDepthOk = doc.nlines >= 7
        && doc.lines[3].type == LT_QUOTE && doc.lines[3].depth == 1
        && doc.lines[4].type == LT_QUOTE && doc.lines[4].depth == 2
        && doc.lines[5].type == LT_QUOTE && doc.lines[5].depth == 1;
    expect(qDepthOk, "nested quote levels parsed");
    /* emoji kept as one run (surrogate pair not split) */
    int emojiOk = 0;
    for (int k = 0; k < doc.lines[6].nsubs; k++)
        for (int r2 = 0; r2 < doc.lines[6].subs[k].nruns; r2++)
            if (doc.lines[6].subs[k].runs[r2].flags == 0
                && doc.lines[6].subs[k].runs[r2].len > 0) emojiOk = 1;
    expect(emojiOk, "emoji line builds runs");
    md_free(&doc);

    /* ---- footnotes ---- */
    const wchar_t *fnmd =
        L"Text with a note[^1] and [^long-label].\n"
        L"\n"
        L"[^1]: first footnote\n"
        L"[^long-label]: second **bold** note\n";
    md_build(&doc, fnmd, lstrlenW(fnmd), &f, dc, 700);
    expect(doc.nfootnotes == 2, "footnote: 2 definitions collected");
    int fnSup = 0;
    for (int k = 0; k < doc.lines[0].nsubs; k++)
        for (int r2 = 0; r2 < doc.lines[0].subs[k].nruns; r2++)
            if (doc.lines[0].subs[k].runs[r2].flags & RF_SUP) fnSup++;
    expect(fnSup == 2, "footnote: inline refs rendered as superscript");
    int fnTail = 0;
    for (int i = 0; i < doc.nlines; i++)
        if (doc.lines[i].type == LT_HR) fnTail = 1;
    expect(fnTail && doc.nlines > 3, "footnote: separator + notes appended");
    md_free(&doc);

    html = NULL;
    hl = md_to_html(fnmd, lstrlenW(fnmd), &html);
    int hFnOl = 0, hFnLi = 0, hFnSup = 0;
    if (html) {
        for (int i = 0; i + 8 < hl; i++) {
            if (!strncmp(html + i, "<section class=\"footnotes\"", 26)) hFnOl = 1;
            if (!strncmp(html + i, "id=\"fn-1\"", 9)) hFnLi = 1;
            if (!strncmp(html + i, "<sup>[1]</sup>", 14)) hFnSup = 1;
        }
    }
    expect(hFnOl && hFnLi && hFnSup, "html: footnotes section emitted");
    free(html);

    /* ---- wiki links ---- */
    {
        const wchar_t *wmd =
            L"see [[Target Note]] and [[tgt|wiki]] here.\n"
            L"\n"
            L"normal [link](https://x) stays a link\n"
            L"code span `[[not wiki]]` is literal\n";
        md_build(&doc, wmd, lstrlenW(wmd), &f, dc, 700);
        int plainN = 0, pipeN = 0, lblOk = 0, codeLeak = 0;
        for (int li = 0; li < doc.nlines; li++)
            for (int si = 0; si < doc.lines[li].nsubs; si++)
                for (int ri = 0; ri < doc.lines[li].subs[si].nruns; ri++) {
                    const MDRun *r = &doc.lines[li].subs[si].runs[ri];
                    if (!(r->flags & RF_WIKILINK)) continue;
                    /* word-wrapping may split one link into several
                     * runs; count unique targets instead */
                    if (r->urlLen == 11
                        && !wcsncmp(r->url, L"Target Note", 11))
                        plainN++;
                    else if (r->urlLen == 3 && !wcsncmp(r->url, L"tgt", 3)) {
                        pipeN++;
                        if (r->len == 4 && !wcsncmp(r->ptr, L"wiki", 4))
                            lblOk = 1;
                    } else
                        codeLeak++;
                }
        expect(plainN >= 1, "wikilink: plain target kept as url");
        expect(pipeN >= 1, "wikilink: [[target|label]] splits text/target");
        expect(lblOk, "wikilink: label text used for display");
        expect(codeLeak == 0, "wikilink: no bogus targets parsed");
        md_free(&doc);

        html = NULL;
        hl = md_to_html(wmd, lstrlenW(wmd), &html);
        int hWiki1 = 0, hWiki2 = 0;
        if (html) {
            for (int i = 0; i + 21 < hl; i++) {
                if (!strncmp(html + i, "href=\"Target Note.md\"", 21))
                    hWiki1 = 1;
                if (!strncmp(html + i, "href=\"tgt.md\">wiki</a>", 22))
                    hWiki2 = 1;
            }
        }
        expect(hWiki1, "wikilink: html href target.md emitted");
        expect(hWiki2, "wikilink: html label split emitted");
        free(html);
    }

    /* ---- html export: tables + tasks ---- */
    hl = md_to_html(tbl, lstrlenW(tbl), &html);
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
    hl = md_to_html(syn, lstrlenW(syn), &html);
    int hEsc = 0, hLink = 0, hMark = 0, bqOpen = 0, bqClose = 0, hInner = 0;
    if (html) {
        for (int i = 0; i + 8 < hl; i++) {
            if (!strncmp(html + i, "<em>", 4)) hEsc = 1;
            if (!strncmp(html + i, "href=\"https://example.com", 25)) hLink = 1;
            if (!strncmp(html + i, "<mark>", 6)) hMark = 1;
            if (!strncmp(html + i, "<blockquote>", 12)) bqOpen++;
            if (!strncmp(html + i, "</blockquote>", 13)) bqClose++;
            if (!strncmp(html + i, "<p>inner</p>", 12)) hInner = 1;
        }
    }
    expect(!hEsc && hLink && hMark, "html: escape/autolink/mark emitted");
    expect(bqOpen == 2 && bqClose == 2 && hInner,
           "html: nested blockquote nesting");
    free(html);

    /* ---- share export: local image embedded as data uri ---- */
    {
        /* minimal 1x1 png */
        static const char PNG[] = {
            (char)0x89,'P','N','G',(char)0x0D,(char)0x0A,(char)0x1A,(char)0x0A,
            0,0,0,(char)0x0D,'I','H','D','R',0,0,0,1,0,0,0,1,8,6,0,0,0,
            (char)0x1F,(char)0x15,(char)0xC4,(char)0x89,0,0,0,(char)0x0A,
            'I','D','A','T',(char)0x78,(char)0x9C,(char)0x63,0,1,0,0,5,0,1,
            (char)0x0D,(char)0x0A,(char)0x2D,(char)0xB4,0,0,0,0,'I','E','N','D',
            (char)0xAE,(char)0x42,(char)0x60,(char)0x82
        };
        wchar_t tmpDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tmpDir);
        wchar_t pngPath[MAX_PATH], mdPath[MAX_PATH];
        wsprintfW(pngPath, L"%smdl_test.png", tmpDir);
        wsprintfW(mdPath, L"%smdl_test.md", tmpDir);
        HANDLE h = CreateFileW(pngPath, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, PNG, sizeof(PNG), &w, NULL);
            CloseHandle(h);
        }
        const wchar_t *shareMd =
            L"![本地图片](mdl_test.png) ![远程](https://x/y.png)\n";
        html = NULL;
        hl = md_to_html_standalone(shareMd, lstrlenW(shareMd), tmpDir, &html);
        int hasData = 0, hasRemote = 0;
        if (html) {
            for (int i = 0; i + 20 < hl; i++) {
                if (!strncmp(html + i, "src=\"data:image/png;base64,", 27))
                    hasData = 1;
                if (!strncmp(html + i, "src=\"https://x/y.png\"", 21))
                    hasRemote = 1;
            }
        }
        expect(hl > 0 && html != NULL && hasData,
               "share html: local image inlined as data uri");
        expect(hasRemote, "share html: remote url kept as-is");
        free(html);
        DeleteFileW(pngPath);
        (void)mdPath;
    }

    /* ---- editor logic: list continuation ---- */
    {
        wchar_t cont[112];
        int ex = 0, cl;

        cl = ListContinuation(L"- item", 6, cont, 112, &ex);
        expect(cl == 4 && !ex && !wcscmp(cont, L"\r\n- "),
               "listcont: dash item continues");
        cl = ListContinuation(L"3. third", 8, cont, 112, &ex);
        expect(cl == 5 && !ex && !wcscmp(cont, L"\r\n4. "),
               "listcont: ordered marker increments");
        cl = ListContinuation(L"- [ ] buy milk", 14, cont, 112, &ex);
        expect(cl == 8 && !ex && !wcscmp(cont, L"\r\n- [ ] "),
               "listcont: task item continues unchecked");
        cl = ListContinuation(L"- ", 2, cont, 112, &ex);
        expect(cl == 2 && ex, "listcont: empty item exits list");
        cl = ListContinuation(L"  10. deep", 10, cont, 112, &ex);
        expect(cl == 8 && !ex && !wcscmp(cont, L"\r\n  11. "),
               "listcont: indent kept, big number ok");
        cl = ListContinuation(L"plain text", 10, cont, 112, &ex);
        expect(cl == 0, "listcont: non-list line ignored");
        cl = ListContinuation(L"-nospace", 8, cont, 112, &ex);
        expect(cl == 0, "listcont: dash without space ignored");
        cl = ListContinuation(L"*x*", 3, cont, 112, &ex);
        expect(cl == 0, "listcont: emphasis not a marker");
        cl = ListContinuation(L"- done", 6, cont, 0, &ex);
        expect(cl == 0, "listcont: tiny buffer safe");
    }

    md_free_fonts(&f);
    ReleaseDC(0, dc);
    printf(g_fail ? "\nRESULT: FAIL\n" : "\nRESULT: ALL PASS\n");
    return g_fail;
}
