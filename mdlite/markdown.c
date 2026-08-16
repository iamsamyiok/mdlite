/* MDLite - markdown parser + GDI renderer */
#include "markdown.h"
#include <stdlib.h>
#include <string.h>

/* Apple-style palette (macOS system colors) */
COLORREF g_colText        = RGB(0x1D,0x1D,0x1F); /* label */
COLORREF g_colHeading     = RGB(0x1D,0x1D,0x1F);
COLORREF g_colQuote       = RGB(0x6E,0x6E,0x73); /* secondaryLabel */
COLORREF g_colCodeBg      = RGB(0xF5,0xF5,0xF7); /* fill / windowBg */
COLORREF g_colCodeInlineBg= RGB(0xF0,0xF0,0xF4);
COLORREF g_colLink        = RGB(0x00,0x7A,0xFF); /* systemBlue */
COLORREF g_colHr          = RGB(0xD2,0xD2,0xD7); /* separator */
COLORREF g_colQuoteBar    = RGB(0xC9,0xC9,0xCE);
COLORREF g_colHeadingRule = RGB(0xE8,0xE8,0xED);

/* ------------------------------------------------------------------ */
/* fonts                                                               */
/* ------------------------------------------------------------------ */

static HFONT make_font(int dpi, const wchar_t *face, int pt10,
                       int bold, int italic, int underline)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(pt10, dpi, 720); /* pt10 = pt * 10 */
    lf.lfWeight = bold ? FW_BOLD : FW_REGULAR;
    lf.lfItalic = (BYTE)italic;
    lf.lfUnderline = (BYTE)underline;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

static int font_cellheight(HDC hdc, HFONT f)
{
    HFONT old = (HFONT)SelectObject(hdc, f);
    TEXTMETRICW tm;
    GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, old);
    return tm.tmHeight;
}

void md_init_fonts(MDFonts *f, HDC hdc, int dpi)
{
    md_free_fonts(f);
    f->body     = make_font(dpi, L"Segoe UI", 105, 0, 0, 0); /* 10.5pt */
    f->bold     = make_font(dpi, L"Segoe UI", 105, 1, 0, 0);
    f->ital     = make_font(dpi, L"Segoe UI", 105, 0, 1, 0);
    f->boldital = make_font(dpi, L"Segoe UI", 105, 1, 1, 0);
    f->mono     = make_font(dpi, L"Consolas", 95, 0, 0, 0);
    /* heading ladder: each level clearly larger than the next */
    f->h[0]     = make_font(dpi, L"Segoe UI", 260, 1, 0, 0); /* 26pt */
    f->h[1]     = make_font(dpi, L"Segoe UI", 200, 1, 0, 0); /* 20pt */
    f->h[2]     = make_font(dpi, L"Segoe UI", 160, 1, 0, 0); /* 16pt */
    f->h[3]     = make_font(dpi, L"Segoe UI", 135, 1, 0, 0); /* 13.5pt */
    f->h[4]     = make_font(dpi, L"Segoe UI", 118, 1, 0, 0); /* 11.8pt */
    f->h[5]     = make_font(dpi, L"Segoe UI", 108, 1, 0, 0); /* 10.8pt */
    f->bodyH = font_cellheight(hdc, f->body);
    f->monoH = font_cellheight(hdc, f->mono);
    for (int i = 0; i < 6; i++)
        f->hH[i] = font_cellheight(hdc, f->h[i]);
}

void md_free_fonts(MDFonts *f)
{
    if (f->body)     DeleteObject(f->body);
    if (f->bold)     DeleteObject(f->bold);
    if (f->ital)     DeleteObject(f->ital);
    if (f->boldital) DeleteObject(f->boldital);
    if (f->mono)     DeleteObject(f->mono);
    for (int i = 0; i < 6; i++)
        if (f->h[i]) DeleteObject(f->h[i]);
    ZeroMemory(f, sizeof(*f));
}

/* ------------------------------------------------------------------ */
/* dynamic helpers                                                     */
/* ------------------------------------------------------------------ */

static int round_up(int n) { return n ? n * 2 : 8; }

static MDLine *push_line(MDDoc *d, int type)
{
    if (d->nlines == d->caplines) {
        d->caplines = round_up(d->caplines);
        d->lines = (MDLine *)realloc(d->lines, d->caplines * sizeof(MDLine));
    }
    MDLine *L = &d->lines[d->nlines++];
    ZeroMemory(L, sizeof(*L));
    L->type = type;
    return L;
}

typedef struct RunBuf {
    MDRun *v;
    int n, cap;
} RunBuf;

static void push_run(RunBuf *b, const wchar_t *p, int len, int flags)
{
    if (len <= 0) return;
    if (b->n == b->cap) {
        b->cap = round_up(b->cap);
        b->v = (MDRun *)realloc(b->v, b->cap * sizeof(MDRun));
    }
    b->v[b->n].ptr = p;
    b->v[b->n].len = len;
    b->v[b->n].flags = flags;
    b->n++;
}

static void free_runs(RunBuf *b) { free(b->v); b->v = NULL; b->n = b->cap = 0; }

/* ------------------------------------------------------------------ */
/* inline parsing                                                      */
/* ------------------------------------------------------------------ */

/* find closing marker starting at s[1]; returns total consumed length
   (leading marker + content + trailing marker) or 0 */
static int match_marker(const wchar_t *s, int len, const wchar_t *m)
{
    int ml = lstrlenW(m);
    for (int i = 1; i + ml <= len; i++)
        if (!wcsncmp(s + i, m, ml))
            return i + ml;
    return 0;
}

/* match "[text](url)" starting at s[0]=='['; fills content range,
   returns total consumed length or 0 */
static int match_link(const wchar_t *s, int len, int *contentStart,
                      int *contentLen)
{
    int i = 1, depth = 1;
    while (i < len && depth) {
        if (s[i] == L'[') depth++;
        else if (s[i] == L']') depth--;
        i++;
    }
    if (depth) return 0;
    int closeEnd = i; /* char right after ']' */
    if (closeEnd >= len || s[closeEnd] != L'(') return 0;
    int j = closeEnd + 1;
    while (j < len && s[j] != L')') {
        if (s[j] == L'(') return 0;
        j++;
    }
    if (j >= len) return 0;
    *contentStart = 1;
    *contentLen = closeEnd - 2;
    return j + 1;
}

static void parse_inline(const wchar_t *s, int len, int flags, int depth,
                         RunBuf *out)
{
    int i = 0;
    while (i < len) {
        wchar_t c = s[i];
        int matched = 0;

        if (c == L'`') {
            int j = i + 1;
            while (j < len && s[j] != L'`') j++;
            if (j < len && j > i + 1) {
                if (i > 0) push_run(out, s, i, flags);
                push_run(out, s + i + 1, j - i - 1, flags | RF_CODE);
                s += j + 1; len -= j + 1; i = 0;
                matched = 1;
            }
        }
        else if (depth < 3 && c == L'*' && i + 1 < len && s[i+1] == L'*') {
            int m = match_marker(s + i, len - i, L"**");
            if (m > 4) {
                if (i > 0) push_run(out, s, i, flags);
                parse_inline(s + i + 2, m - 4, flags | RF_BOLD, depth + 1, out);
                i += m; s += i; len -= i; i = 0;
                matched = 1;
            }
        }
        else if (depth < 3 && c == L'*' && i + 1 < len && s[i+1] != L'*'
                 && !iswspace(s[i+1])) {
            int m = match_marker(s + i, len - i, L"*");
            if (m > 2) {
                if (i > 0) push_run(out, s, i, flags);
                parse_inline(s + i + 1, m - 2, flags | RF_ITALIC, depth + 1, out);
                i += m; s += i; len -= i; i = 0;
                matched = 1;
            }
        }
        else if (depth < 3 && c == L'~' && i + 1 < len && s[i+1] == L'~') {
            int m = match_marker(s + i, len - i, L"~~");
            if (m > 4) {
                if (i > 0) push_run(out, s, i, flags);
                parse_inline(s + i + 2, m - 4, flags | RF_STRIKE, depth + 1, out);
                i += m; s += i; len -= i; i = 0;
                matched = 1;
            }
        }
        else if (c == L'[' || (c == L'!' && i + 1 < len && s[i+1] == L'[')) {
            int off = (c == L'!') ? 1 : 0;
            int cs = 0, cl = 0;
            int m = match_link(s + i + off, len - i - off, &cs, &cl);
            if (m > 0) {
                if (i > 0) push_run(out, s, i, flags);
                int lf = flags | (off ? RF_IMAGE : RF_LINK);
                parse_inline(s + i + off + cs, cl, lf, depth + 1, out);
                i += off + m; s += i; len -= i; i = 0;
                matched = 1;
            }
        }

        if (!matched) i++;
    }
    if (len > 0) push_run(out, s, len, flags);
}

/* ------------------------------------------------------------------ */
/* block helpers                                                       */
/* ------------------------------------------------------------------ */

static int is_blank(const wchar_t *s, int len)
{
    for (int i = 0; i < len; i++)
        if (!iswspace(s[i])) return 0;
    return 1;
}

static int lead_spaces(const wchar_t *s, int len)
{
    int i = 0;
    while (i < len && s[i] == L' ') i++;
    return i;
}

static int is_hr(const wchar_t *s, int len)
{
    if (len < 3) return 0;
    wchar_t c = s[0];
    if (c != L'-' && c != L'*' && c != L'_') return 0;
    int count = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == c) count++;
        else if (s[i] != L' ') return 0;
    }
    return count >= 3;
}

/* expand tabs to 4 spaces in-place; may realloc, returns (new) buffer */
static wchar_t *expand_tabs(wchar_t *buf, int *plen)
{
    int len = *plen, tabs = 0;
    for (int i = 0; i < len; i++)
        if (buf[i] == L'\t') tabs++;
    if (!tabs) return buf;
    int newLen = len + tabs * 3;
    wchar_t *nb = (wchar_t *)realloc(buf, (newLen + 1) * sizeof(wchar_t));
    if (!nb) return buf; /* keep original on failure */
    int w = newLen, r = len;
    while (r > 0) {
        r--;
        if (nb[r] == L'\t') {
            nb[--w] = L' '; nb[--w] = L' ';
            nb[--w] = L' '; nb[--w] = L' ';
        } else {
            nb[--w] = nb[r];
        }
    }
    nb[newLen] = 0;
    *plen = newLen;
    return nb;
}

/* ------------------------------------------------------------------ */
/* layout (word wrap)                                                  */
/* ------------------------------------------------------------------ */

static HFONT font_for(const MDFonts *f, int flags, int hl)
{
    int b = (flags & RF_BOLD) != 0, i = (flags & RF_ITALIC) != 0;
    if (flags & RF_CODE) return f->mono;
    if (hl) return f->h[hl - 1]; /* heading fonts are pre-built bold */
    if (b && i) return f->boldital;
    if (b) return f->bold;
    if (i) return f->ital;
    return f->body;
}

int text_w(HDC hdc, HFONT font, const wchar_t *s, int len)
{
    HFONT old = (HFONT)SelectObject(hdc, font);
    SIZE sz;
    int ok = GetTextExtentPoint32W(hdc, s, len, &sz);
    SelectObject(hdc, old);
    return ok ? sz.cx : 0;
}

/* append current run buffer as a finished sub-line */
static void flush_sub(MDLine *L, RunBuf *cur, int lineH)
{
    if (L->nsubs == 0 && cur->n == 0) {
        /* keep one empty sub so blank paragraph lines still hold height */
    }
    if (cur->n == 0) return;
    L->subs = (MDSub *)realloc(L->subs, (L->nsubs + 1) * sizeof(MDSub));
    L->subs[L->nsubs].runs = cur->v;
    L->subs[L->nsubs].nruns = cur->n;
    L->subs[L->nsubs].height = lineH;
    L->nsubs++;
    cur->v = NULL; cur->n = cur->cap = 0;
}

static void append_sub_owned(MDLine *L, MDRun *runs, int nruns, int lineH)
{
    L->subs = (MDSub *)realloc(L->subs, (L->nsubs + 1) * sizeof(MDSub));
    L->subs[L->nsubs].runs = runs;
    L->subs[L->nsubs].nruns = nruns;
    L->subs[L->nsubs].height = lineH;
    L->nsubs++;
}

/* wrap runs into sub-lines using greedy word fill + hard word split */
static void wrap_line(MDLine *L, RunBuf *rb, const MDFonts *f, HDC hdc,
                      int availW, int lineH, int hl)
{
    if (rb->n == 0 || availW < 40) {
        append_sub_owned(L, NULL, 0, lineH);
        return;
    }

    /* expand runs into tokens: words and space gaps, flags preserved */
    typedef struct Tok { const wchar_t *ptr; int len; int flags; int isSpace; int wid; } Tok;
    Tok *t = NULL;
    int nt = 0, tcap = 0;
    for (int r = 0; r < rb->n; r++) {
        const wchar_t *p = rb->v[r].ptr;
        int len = rb->v[r].len, flags = rb->v[r].flags;
        HFONT fo = font_for(f, flags, hl);
        int i = 0;
        while (i < len) {
            int j = i;
            int isSp = (p[i] == L' ');
            while (j < len && (p[j] == L' ') == isSp) j++;
            if (nt == tcap) {
                tcap = round_up(tcap);
                t = (Tok *)realloc(t, tcap * sizeof(Tok));
            }
            t[nt].ptr = p + i;
            t[nt].len = j - i;
            t[nt].flags = flags;
            t[nt].isSpace = isSp;
            t[nt].wid = text_w(hdc, fo, p + i, j - i);
            nt++;
            i = j;
        }
    }

    RunBuf cur = {0};
    int x = 0, i = 0;
    while (i < nt) {
        if (t[i].isSpace) {
            if (x == 0) { i++; continue; }              /* swallow line-leading */
            if (x + t[i].wid <= availW) {
                push_run(&cur, t[i].ptr, t[i].len, t[i].flags);
                x += t[i].wid;
                i++;
                continue;
            }
            flush_sub(L, &cur, lineH); x = 0; i++;      /* trailing gap dropped */
            continue;
        }
        int wid = t[i].wid;
        if (x > 0 && x + wid > availW) {                /* wrap before word */
            flush_sub(L, &cur, lineH);
            x = 0;
            continue;                                    /* reprocess word */
        }
        if (wid > availW && t[i].len > 1) {
            /* hard split oversized word (long CJK run / URL) */
            HFONT fo = font_for(f, t[i].flags, hl);
            int lo = 1, hi = t[i].len;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                if (text_w(hdc, fo, t[i].ptr, mid) <= availW) lo = mid;
                else hi = mid - 1;
            }
            flush_sub(L, &cur, lineH);
            MDRun *one = (MDRun *)malloc(sizeof(MDRun) * 2);
            one[0].ptr = t[i].ptr; one[0].len = lo;      one[0].flags = t[i].flags;
            one[1].ptr = t[i].ptr + lo; one[1].len = t[i].len - lo; one[1].flags = t[i].flags;
            append_sub_owned(L, one, 2, lineH);
            t[i].ptr += lo; t[i].len -= lo;
            t[i].wid = text_w(hdc, fo, t[i].ptr, t[i].len);
            x = 0;
            continue;                                    /* remainder re-enters */
        }
        push_run(&cur, t[i].ptr, t[i].len, t[i].flags);
        x += wid;
        i++;
    }
    flush_sub(L, &cur, lineH);
    free(t);
    free_runs(&cur);
    if (L->nsubs == 0)
        append_sub_owned(L, NULL, 0, lineH);
}

/* ------------------------------------------------------------------ */
/* build                                                               */
/* ------------------------------------------------------------------ */

void md_build(MDDoc *doc, const wchar_t *src, int srcLen,
              const MDFonts *f, HDC hdc, int width)
{
    md_free(doc);
    ZeroMemory(doc, sizeof(*doc));
    doc->width = width;

    doc->text = (wchar_t *)malloc((srcLen + 1) * sizeof(wchar_t));
    memcpy(doc->text, src, srcLen * sizeof(wchar_t));
    doc->text[srcLen] = 0;
    doc->text = expand_tabs(doc->text, &srcLen);

    int contentW = width - 48; /* 24px margins each side */
    if (contentW < 100) contentW = 100;

    int inCode = 0, codeId = 0;
    int y = 0, pos = 0;
    int pendingBlank = 0;
    int headingLevel = 0;
    int total = srcLen;
    const wchar_t *p = doc->text;

    while (pos <= total) {
        const wchar_t *ls = p + pos;
        int len = 0;
        while (pos + len < total && p[pos + len] != L'\n') len++;
        int advance = len + 1;
        while (len > 0 && ls[len-1] == L'\r') len--;

        int rawLen = advance - 1;
        if (is_blank(ls, rawLen)) {
            MDLine *L = push_line(doc, LT_BLANK);
            L->height = pendingBlank ? 0 : (f->bodyH * 4) / 5;
            pendingBlank = 1;
            L->y = y; y += L->height;
            pos += advance;
            if (pos > total) break;
            continue;
        }
        pendingBlank = 0;

        int ls_spaces = lead_spaces(ls, len);
        const wchar_t *s = ls + ls_spaces;
        len -= ls_spaces;

        MDLine *L = NULL;
        RunBuf rb = {0};
        int lineH = f->bodyH + (f->bodyH >> 1); /* 1.5 line spacing */
        int spaceAbove = 0, spaceBelow = 0;

        if (inCode) {
            if (len >= 3 && s[0] == L'`' && s[1] == L'`' && s[2] == L'`') {
                L = push_line(doc, LT_CODEPAD);
                L->code = codeId;
                L->height = 10;
                inCode = 0;
            } else {
                L = push_line(doc, LT_CODE);
                L->code = codeId;
                push_run(&rb, s, len, RF_CODE);
                int ch = f->monoH + (f->monoH >> 2);
                wrap_line(L, &rb, f, hdc, contentW - 24, ch, 0);
                L->height = 0;
                for (int k = 0; k < L->nsubs; k++)
                    L->height += L->subs[k].height;
            }
            free_runs(&rb);
            L->y = y; y += L->height;
            pos += advance;
            if (pos > total) break;
            continue;
        }

        if (len >= 3 && s[0] == L'`' && s[1] == L'`' && s[2] == L'`') {
            inCode = 1; codeId++;
            L = push_line(doc, LT_CODEPAD);
            L->code = codeId;
            L->height = 10;
            L->y = y; y += L->height;
            pos += advance;
            if (pos > total) break;
            continue;
        }

        if (is_hr(s, len)) {
            L = push_line(doc, LT_HR);
            L->height = f->bodyH + 12;
            L->y = y; y += L->height;
            pos += advance;
            if (pos > total) break;
            continue;
        }

        if (s[0] == L'#') {
            int level = 0;
            while (level < len && level < 6 && s[level] == L'#') level++;
            if (level < len && s[level] == L' ') {
                s += level + 1; len -= level + 1;
                L = push_line(doc, LT_H1 + level - 1);
                headingLevel = level;
                lineH = f->hH[level-1] + 4;
                spaceAbove = (level <= 2) ? 28 : 18;
                spaceBelow = (level <= 2) ? 10 : 8;
                goto have_line;
            }
        }

        if (s[0] == L'>') {
            if (len > 1 && s[1] == L' ') { s += 2; len -= 2; }
            else { s += 1; len -= 1; }
            L = push_line(doc, LT_QUOTE);
            goto have_line;
        }

        if ((s[0] == L'-' || s[0] == L'*' || s[0] == L'+') &&
            len > 1 && s[1] == L' ') {
            L = push_line(doc, LT_ITEM);
            L->depth = ls_spaces / 2;
            s += 2; len -= 2;
            goto have_line;
        }

        if (s[0] >= L'0' && s[0] <= L'9') {
            int d = 0;
            while (d < len && s[d] >= L'0' && s[d] <= L'9') d++;
            if (d < len && s[d] == L'.' && d + 1 < len && s[d+1] == L' ') {
                int num = 0;
                for (int k = 0; k < d; k++) num = num * 10 + (s[k] - L'0');
                L = push_line(doc, LT_OLITEM);
                L->depth = ls_spaces / 2;
                L->itemNum = num;
                s += d + 2; len -= d + 2;
                goto have_line;
            }
        }

        L = push_line(doc, LT_TEXT);

    have_line:
        parse_inline(s, len, 0, 0, &rb);
        wrap_line(L, &rb, f, hdc,
                  contentW - (L->type == LT_QUOTE ? 20 : 0), lineH,
                  headingLevel);
        headingLevel = 0;
        free_runs(&rb);
        L->padTop = spaceAbove;
        L->height = spaceAbove + spaceBelow;
        for (int k = 0; k < L->nsubs; k++)
            L->height += L->subs[k].height;
        L->y = y;
        y += L->height;
        pos += advance;
        if (pos > total) break;
    }

    doc->height = y;
}

void md_free(MDDoc *doc)
{
    if (!doc) return;
    for (int i = 0; i < doc->nlines; i++) {
        MDLine *L = &doc->lines[i];
        for (int k = 0; k < L->nsubs; k++)
            free(L->subs[k].runs);
        free(L->subs);
    }
    free(doc->lines);
    free(doc->text);
    ZeroMemory(doc, sizeof(*doc));
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */

static void draw_runs(HDC hdc, const MDSub *sub, int x, int y,
                      const MDFonts *f, COLORREF defCol, int hl)
{
    SetBkMode(hdc, TRANSPARENT);
    for (int i = 0; i < sub->nruns; i++) {
        const MDRun *r = &sub->runs[i];
        HFONT fo = font_for(f, r->flags, hl);
        HFONT old = (HFONT)SelectObject(hdc, fo);
        TEXTMETRICW tm;
        GetTextMetricsW(hdc, &tm);
        int w = text_w(hdc, fo, r->ptr, r->len);
        int ry = y + (sub->height - tm.tmHeight) / 2;

        if (r->flags & RF_CODE) {
            RECT rc = { x, ry, x + w + 4, ry + tm.tmHeight };
            HRGN rg = CreateRoundRectRgn(rc.left, rc.top, rc.right, rc.bottom,
                                         6, 6);
            HBRUSH br = CreateSolidBrush(g_colCodeInlineBg);
            FillRgn(hdc, rg, br);
            DeleteObject(rg);
            DeleteObject(br);
        }

        COLORREF col = defCol;
        if (r->flags & RF_LINK) col = g_colLink;
        if (r->flags & RF_IMAGE) col = g_colQuote;

        if (r->flags & RF_STRIKE) {
            RECT rc = { x, ry + tm.tmHeight / 2, x + w,
                        ry + tm.tmHeight / 2 + 1 };
            HBRUSH br = CreateSolidBrush(col);
            FillRect(hdc, &rc, br);
            DeleteObject(br);
        }

        SetTextColor(hdc, col);
        ExtTextOutW(hdc, x + (r->flags & RF_CODE ? 2 : 0), ry, 0, NULL,
                    r->ptr, r->len, NULL);

        if (r->flags & RF_LINK) {
            HPEN pen = CreatePen(PS_SOLID, 1, g_colLink);
            HPEN op = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, x, ry + tm.tmHeight, NULL);
            LineTo(hdc, x + w, ry + tm.tmHeight);
            SelectObject(hdc, op);
            DeleteObject(pen);
        }
        SelectObject(hdc, old);
        x += w + (r->flags & RF_CODE ? 4 : 0);
    }
}

void md_paint(const MDDoc *doc, HDC hdc, const RECT *rc, int scrollY,
              const MDFonts *f)
{
    if (!doc || !doc->lines) return;

    FillRect(hdc, rc, (HBRUSH)GetStockObject(WHITE_BRUSH));

    int margin = 24;
    int contentW = rc->right - rc->left - margin * 2;
    int yOff = rc->top;          /* document y=0 maps here */
    int viewTop = scrollY;
    int viewBottom = scrollY + (rc->bottom - rc->top);

    /* pass 1: code block backgrounds */
    int i = 0;
    while (i < doc->nlines) {
        if (doc->lines[i].code) {
            int id = doc->lines[i].code;
            int j = i, top = -1, bottom = -1;
            while (j < doc->nlines && doc->lines[j].code == id) {
                if (top < 0) top = doc->lines[j].y;
                bottom = doc->lines[j].y + doc->lines[j].height;
                j++;
            }
            if (bottom > viewTop && top < viewBottom) {
                RECT cb = { rc->left + margin, top - scrollY + yOff,
                            rc->left + margin + contentW,
                            bottom - scrollY + yOff };
                HRGN rg = CreateRoundRectRgn(cb.left, cb.top, cb.right,
                                             cb.bottom, 14, 14);
                HBRUSH br = CreateSolidBrush(g_colCodeBg);
                FillRgn(hdc, rg, br);
                DeleteObject(rg);
                DeleteObject(br);
            }
            i = j;
        } else i++;
    }

    /* pass 2: content */
    for (i = 0; i < doc->nlines; i++) {
        const MDLine *L = &doc->lines[i];
        int top = L->y - scrollY + yOff;
        int bottom = top + L->height;
        if (bottom < rc->top) continue;
        if (top > rc->bottom) break;

        int x = rc->left + margin;
        int subY = top + L->padTop;

        switch (L->type) {
        case LT_BLANK:
        case LT_CODEPAD:
            break;

        case LT_HR: {
            int cy = top + L->height / 2;
            HPEN pen = CreatePen(PS_SOLID, 1, g_colHr);
            HPEN op = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, x, cy, NULL);
            LineTo(hdc, x + contentW, cy);
            SelectObject(hdc, op);
            DeleteObject(pen);
            break;
        }

        case LT_CODE:
            for (int k = 0; k < L->nsubs; k++) {
                draw_runs(hdc, &L->subs[k], x + 12, subY, f, g_colText, 0);
                subY += L->subs[k].height;
            }
            break;

        case LT_QUOTE: {
            HPEN pen = CreatePen(PS_SOLID | PS_ENDCAP_ROUND, 3, g_colQuoteBar);
            HPEN op = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, x + 1, top, NULL);
            LineTo(hdc, x + 1, bottom);
            SelectObject(hdc, op);
            DeleteObject(pen);
            for (int k = 0; k < L->nsubs; k++) {
                draw_runs(hdc, &L->subs[k], x + 16, subY, f, g_colQuote, 0);
                subY += L->subs[k].height;
            }
            break;
        }

        case LT_ITEM: {
            int bx = x + L->depth * 18;
            HFONT old = (HFONT)SelectObject(hdc, f->bold);
            SetTextColor(hdc, g_colText);
            SetBkMode(hdc, TRANSPARENT);
            ExtTextOutW(hdc, bx, top + L->padTop, 0, NULL, L"\x2022", 1, NULL);
            SelectObject(hdc, old);
            for (int k = 0; k < L->nsubs; k++) {
                draw_runs(hdc, &L->subs[k], bx + 18, subY, f, g_colText, 0);
                subY += L->subs[k].height;
            }
            break;
        }

        case LT_OLITEM: {
            wchar_t buf[16];
            int tx = x + L->depth * 18;
            wsprintfW(buf, L"%d.", L->itemNum);
            HFONT old = (HFONT)SelectObject(hdc, f->body);
            SetTextColor(hdc, g_colText);
            SetBkMode(hdc, TRANSPARENT);
            ExtTextOutW(hdc, tx, top + L->padTop, 0, NULL, buf,
                        lstrlenW(buf), NULL);
            SelectObject(hdc, old);
            for (int k = 0; k < L->nsubs; k++) {
                draw_runs(hdc, &L->subs[k], tx + 32, subY, f, g_colText, 0);
                subY += L->subs[k].height;
            }
            break;
        }

        default: { /* paragraph text + headings */
            int hl = 0;
            if (L->type >= LT_H1 && L->type <= LT_H6)
                hl = L->type - LT_H1 + 1;
            if (hl == 1 || hl == 2) {
                int ruleY = bottom - 2;
                HPEN pen = CreatePen(PS_SOLID, 1, g_colHeadingRule);
                HPEN op = (HPEN)SelectObject(hdc, pen);
                MoveToEx(hdc, x, ruleY, NULL);
                LineTo(hdc, x + contentW, ruleY);
                SelectObject(hdc, op);
                DeleteObject(pen);
            }
            COLORREF defCol = (hl == 6) ? g_colQuote : g_colText;
            for (int k = 0; k < L->nsubs; k++) {
                draw_runs(hdc, &L->subs[k], x, subY, f, defCol, hl);
                subY += L->subs[k].height;
            }
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ */
/* HTML export: same syntax subset as the renderer                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    int len, cap;
} HtmlOut;

static void h_app(HtmlOut *o, const char *s)
{
    int n = (int)strlen(s);
    if (o->len + n + 1 > o->cap) {
        int cap = o->cap ? o->cap * 2 : 4096;
        while (cap < o->len + n + 1) cap *= 2;
        char *nb = (char *)realloc(o->buf, cap);
        if (nb) { o->buf = nb; o->cap = cap; }
    }
    if (o->buf && o->len + n < o->cap) {
        memcpy(o->buf + o->len, s, n);
        o->len += n;
        o->buf[o->len] = 0;
    }
}

/* append a wide char range as escaped UTF-8 */
static void h_appw(HtmlOut *o, const wchar_t *s, int len)
{
    for (int i = 0; i < len; i++) {
        wchar_t c = s[i];
        switch (c) {
        case L'&': h_app(o, "&amp;");  continue;
        case L'<': h_app(o, "&lt;");   continue;
        case L'>': h_app(o, "&gt;");   continue;
        case L'"': h_app(o, "&quot;"); continue;
        }
        if (c < 0x80) {
            char b[2] = { (char)c, 0 };
            h_app(o, b);
        } else if (c < 0x800) {
            char b[3] = { (char)(0xC0 | (c >> 6)),
                          (char)(0x80 | (c & 0x3F)), 0 };
            h_app(o, b);
        } else {
            char b[4] = { (char)(0xE0 | (c >> 12)),
                          (char)(0x80 | ((c >> 6) & 0x3F)),
                          (char)(0x80 | (c & 0x3F)), 0 };
            h_app(o, b);
        }
    }
}

/* append raw wide range as UTF-8 without escaping (urls) */
static void h_appurl(HtmlOut *o, const wchar_t *s, int len)
{
    for (int i = 0; i < len; i++) {
        wchar_t c = s[i];
        if (c == L'&') { h_app(o, "&amp;"); continue; }
        if (c == L'"') { h_app(o, "%22"); continue; }
        if (c == L'<') { continue; }
        if (c < 0x80) {
            char b[2] = { (char)c, 0 };
            h_app(o, b);
        } else if (c < 0x800) {
            char b[3] = { (char)(0xC0 | (c >> 6)),
                          (char)(0x80 | (c & 0x3F)), 0 };
            h_app(o, b);
        } else {
            char b[4] = { (char)(0xE0 | (c >> 12)),
                          (char)(0x80 | ((c >> 6) & 0x3F)),
                          (char)(0x80 | (c & 0x3F)), 0 };
            h_app(o, b);
        }
    }
}

static void h_inline(HtmlOut *o, const wchar_t *s, int len, int depth)
{
    for (int i = 0; i < len; i++) {
        wchar_t c = s[i];
        if (depth < 4 && c == L'*' && i + 1 < len && s[i+1] == L'*') {
            int m = i + 2;
            while (m + 1 < len && !(s[m] == L'*' && s[m+1] == L'*')) m++;
            if (m + 1 < len) {
                h_app(o, "<strong>");
                h_inline(o, s + i + 2, m - i - 2, depth + 1);
                h_app(o, "</strong>");
                i = m + 1;
                continue;
            }
        }
        if (depth < 4 && c == L'*' && i + 1 < len && s[i+1] != L'*'
            && s[i+1] != L' ') {
            int m = i + 1;
            while (m < len && s[m] != L'*') m++;
            if (m < len && m > i + 1) {
                h_app(o, "<em>");
                h_inline(o, s + i + 1, m - i - 1, depth + 1);
                h_app(o, "</em>");
                i = m;
                continue;
            }
        }
        if (depth < 4 && c == L'~' && i + 1 < len && s[i+1] == L'~') {
            int m = i + 2;
            while (m + 1 < len && !(s[m] == L'~' && s[m+1] == L'~')) m++;
            if (m + 1 < len) {
                h_app(o, "<del>");
                h_inline(o, s + i + 2, m - i - 2, depth + 1);
                h_app(o, "</del>");
                i = m + 1;
                continue;
            }
        }
        if (c == L'`') {
            int m = i + 1;
            while (m < len && s[m] != L'`') m++;
            if (m < len) {
                h_app(o, "<code>");
                h_appw(o, s + i + 1, m - i - 1);
                h_app(o, "</code>");
                i = m;
                continue;
            }
        }
        if (c == L'!' && i + 1 < len && s[i+1] == L'[') {
            int rb = i + 2;
            while (rb < len && s[rb] != L']') rb++;
            if (rb + 1 < len && s[rb+1] == L'(') {
                int rp = rb + 2;
                while (rp < len && s[rp] != L')') rp++;
                if (rp < len) {
                    h_app(o, "<img src=\"");
                    h_appurl(o, s + rb + 2, rp - rb - 2);
                    h_app(o, "\" alt=\"");
                    h_appw(o, s + i + 2, rb - i - 2);
                    h_app(o, "\">");
                    i = rp;
                    continue;
                }
            }
        }
        if (c == L'[') {
            int rb = i + 1;
            while (rb < len && s[rb] != L']') rb++;
            if (rb + 1 < len && s[rb+1] == L'(') {
                int rp = rb + 2;
                while (rp < len && s[rp] != L')') rp++;
                if (rp < len) {
                    h_app(o, "<a href=\"");
                    h_appurl(o, s + rb + 2, rp - rb - 2);
                    h_app(o, "\">");
                    h_inline(o, s + i + 1, rb - i - 1, depth + 1);
                    h_app(o, "</a>");
                    i = rp;
                    continue;
                }
            }
        }
        h_appw(o, &c, 1);
    }
}

/* convert src to a full standalone HTML document (UTF-8). returns the
 * byte length, *out must be freed by the caller. 0 on error. */
int md_to_html(const wchar_t *src, int srcLen, char **out)
{
    if (!src || !out) return 0;
    *out = NULL;
    HtmlOut o = {0};
    h_app(&o, "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
              "<title>MDLite</title>\n<style>\n"
              "body{font-family:-apple-system,'Segoe UI',sans-serif;"
              "max-width:760px;margin:32px auto;padding:0 16px;"
              "line-height:1.6;color:#1d1d1f}\n"
              "h1,h2{border-bottom:1px solid #d2d2d7;padding-bottom:6px}\n"
              "code{background:#f5f5f7;border-radius:3px;padding:1px 5px;"
              "font-family:Consolas,monospace;font-size:.9em}\n"
              "pre code{display:block;padding:12px;overflow-x:auto}\n"
              "blockquote{border-left:4px solid #d2d2d7;margin:0;"
              "padding:2px 16px;color:#6e6e73}\n"
              "a{color:#007aff}\nimg{max-width:100%}\n"
              "hr{border:none;border-top:1px solid #d2d2d7}\n"
              "</style>\n</head>\n<body>\n");

    int inCode = 0, inP = 0, inUl = 0, inOl = 0, inQ = 0;
    int pos = 0;
    while (pos <= srcLen) {
        const wchar_t *ls = src + pos;
        int len = 0;
        while (pos + len < srcLen && src[pos + len] != L'\n') len++;
        int adv = len + 1;
        while (len > 0 && ls[len-1] == L'\r') len--;

        /* blank line closes all open blocks */
        int blank = 1;
        for (int i = 0; i < len; i++)
            if (ls[i] != L' ' && ls[i] != L'\t') { blank = 0; break; }
        if (blank) {
            if (inP) { h_app(&o, "</p>\n"); inP = 0; }
            if (inUl) { h_app(&o, "</ul>\n"); inUl = 0; }
            if (inOl) { h_app(&o, "</ol>\n"); inOl = 0; }
            if (inQ) { h_app(&o, "</blockquote>\n"); inQ = 0; }
            pos += adv;
            if (pos > srcLen) break;
            continue;
        }

        int lead = 0;
        while (lead < len && ls[lead] == L' ') lead++;
        const wchar_t *s = ls + lead;
        int sl = len - lead;

        if (inCode) {
            if (sl >= 3 && s[0] == L'`' && s[1] == L'`' && s[2] == L'`') {
                h_app(&o, "</code></pre>\n");
                inCode = 0;
            } else {
                h_appw(&o, s, sl);
                h_app(&o, "\n");
            }
            pos += adv;
            if (pos > srcLen) break;
            continue;
        }

        if (sl >= 3 && s[0] == L'`' && s[1] == L'`' && s[2] == L'`') {
            if (inP) { h_app(&o, "</p>\n"); inP = 0; }
            if (inUl) { h_app(&o, "</ul>\n"); inUl = 0; }
            if (inOl) { h_app(&o, "</ol>\n"); inOl = 0; }
            if (inQ) { h_app(&o, "</blockquote>\n"); inQ = 0; }
            h_app(&o, "<pre><code>");
            inCode = 1;
            pos += adv;
            if (pos > srcLen) break;
            continue;
        }

        if (sl >= 3 && (s[0] == L'-' || s[0] == L'*')
            && s[1] == s[0] && s[2] == s[0]) {
            if (inP) { h_app(&o, "</p>\n"); inP = 0; }
            if (inUl) { h_app(&o, "</ul>\n"); inUl = 0; }
            if (inOl) { h_app(&o, "</ol>\n"); inOl = 0; }
            h_app(&o, "<hr>\n");
            pos += adv;
            if (pos > srcLen) break;
            continue;
        }

        if (s[0] == L'#') {
            int lv = 0;
            while (lv < sl && lv < 6 && s[lv] == L'#') lv++;
            if (lv < sl && s[lv] == L' ') {
                if (inP) { h_app(&o, "</p>\n"); inP = 0; }
                if (inUl) { h_app(&o, "</ul>\n"); inUl = 0; }
                if (inOl) { h_app(&o, "</ol>\n"); inOl = 0; }
                if (inQ) { h_app(&o, "</blockquote>\n"); inQ = 0; }
                const char *tags[6] = { "h1", "h2", "h3", "h4", "h5", "h6" };
                h_app(&o, "<");
                h_app(&o, tags[lv-1]);
                h_app(&o, ">");
                h_inline(&o, s + lv + 1, sl - lv - 1, 0);
                h_app(&o, "</");
                h_app(&o, tags[lv-1]);
                h_app(&o, ">\n");
                pos += adv;
                if (pos > srcLen) break;
                continue;
            }
        }

        if (s[0] == L'>') {
            const wchar_t *qs = s + 1;
            int ql = sl - 1;
            if (ql > 0 && qs[0] == L' ') { qs++; ql--; }
            if (inP) { h_app(&o, "</p>\n"); inP = 0; }
            if (inUl) { h_app(&o, "</ul>\n"); inUl = 0; }
            if (inOl) { h_app(&o, "</ol>\n"); inOl = 0; }
            if (!inQ) { h_app(&o, "<blockquote>\n"); inQ = 1; }
            h_app(&o, "<p>");
            h_inline(&o, qs, ql, 0);
            h_app(&o, "</p>\n");
            pos += adv;
            if (pos > srcLen) break;
            continue;
        }

        if ((s[0] == L'-' || s[0] == L'*' || s[0] == L'+')
            && sl > 1 && s[1] == L' ') {
            if (inP) { h_app(&o, "</p>\n"); inP = 0; }
            if (inOl) { h_app(&o, "</ol>\n"); inOl = 0; }
            if (inQ) { h_app(&o, "</blockquote>\n"); inQ = 0; }
            if (!inUl) { h_app(&o, "<ul>\n"); inUl = 1; }
            h_app(&o, "<li>");
            h_inline(&o, s + 2, sl - 2, 0);
            h_app(&o, "</li>\n");
            pos += adv;
            if (pos > srcLen) break;
            continue;
        }

        if (s[0] >= L'0' && s[0] <= L'9') {
            int d = 0;
            while (d < sl && s[d] >= L'0' && s[d] <= L'9') d++;
            if (d < sl && s[d] == L'.' && d + 1 < sl && s[d+1] == L' ') {
                if (inP) { h_app(&o, "</p>\n"); inP = 0; }
                if (inUl) { h_app(&o, "</ul>\n"); inUl = 0; }
                if (inQ) { h_app(&o, "</blockquote>\n"); inQ = 0; }
                if (!inOl) { h_app(&o, "<ol>\n"); inOl = 1; }
                h_app(&o, "<li>");
                h_inline(&o, s + d + 2, sl - d - 2, 0);
                h_app(&o, "</li>\n");
                pos += adv;
                if (pos > srcLen) break;
                continue;
            }
        }

        /* paragraph line: merge consecutive lines with <br> */
        if (inUl) { h_app(&o, "</ul>\n"); inUl = 0; }
        if (inOl) { h_app(&o, "</ol>\n"); inOl = 0; }
        if (inQ) { h_app(&o, "</blockquote>\n"); inQ = 0; }
        if (!inP) { h_app(&o, "<p>"); inP = 1; }
        else h_app(&o, "<br>\n");
        h_inline(&o, s, sl, 0);
        pos += adv;
        if (pos > srcLen) break;
    }
    if (inCode) h_app(&o, "</code></pre>\n");
    if (inP) h_app(&o, "</p>\n");
    if (inUl) h_app(&o, "</ul>\n");
    if (inOl) h_app(&o, "</ol>\n");
    if (inQ) h_app(&o, "</blockquote>\n");
    h_app(&o, "</body>\n</html>\n");

    if (!o.buf) return 0;
    *out = o.buf;
    return o.len;
}
