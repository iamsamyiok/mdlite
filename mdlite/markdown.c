/* MDLite - markdown parser + GDI renderer */
#define WIDL_C_INLINE_WRAPPERS 0
#include "markdown.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <wchar.h>
#include <wincodec.h>

/* single-thread WIC factory, initialized on first use */
static IWICImagingFactory *g_wicFactory = NULL;
static BOOL g_wicOk = FALSE;

static BOOL InitWic(void)
{
    if (g_wicOk) return TRUE;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (hr != S_OK && hr != S_FALSE) return FALSE;
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void**)&g_wicFactory);
    if (FAILED(hr) || !g_wicFactory) return FALSE;
    g_wicOk = TRUE;
    return TRUE;
}

/* load a local image to an HBITMAP via WIC; outHbm must be DeleteObject'd.
 * maxPx > 0 caps the rendered width; pass 0 for no cap. */
static BOOL LoadWicBitmap(HDC hdc, const wchar_t *path,
                          HBITMAP *outHbm, int *outW, int *outH, int maxPx)
{
    if (!InitWic() || !g_wicFactory) return FALSE;
    IWICBitmapDecoder *dec = NULL;
    HRESULT hr = g_wicFactory->lpVtbl->CreateDecoderFromFilename(
        g_wicFactory, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec);
    if (FAILED(hr) || !dec) return FALSE;
    IWICBitmapFrameDecode *frame = NULL;
    hr = dec->lpVtbl->GetFrame(dec, 0, &frame);
    if (FAILED(hr) || !frame) { dec->lpVtbl->Release(dec); return FALSE; }
    UINT pw = 0, ph = 0;
    hr = frame->lpVtbl->GetSize(frame, &pw, &ph);
    if (FAILED(hr)) { frame->lpVtbl->Release(frame); dec->lpVtbl->Release(dec); return FALSE; }
    int w = (int)pw, h = (int)ph;
    if (maxPx > 0 && w > maxPx) { h = h * maxPx / w; w = maxPx; }
    IWICFormatConverter *fc = NULL;
    hr = g_wicFactory->lpVtbl->CreateFormatConverter(g_wicFactory, &fc);
    if (FAILED(hr) || !fc) { frame->lpVtbl->Release(frame); dec->lpVtbl->Release(dec); return FALSE; }
    hr = fc->lpVtbl->Initialize(fc, (IWICBitmapSource*)frame, &GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) { fc->lpVtbl->Release(fc); frame->lpVtbl->Release(frame); dec->lpVtbl->Release(dec); return FALSE; }
    HDC hdcMem = CreateCompatibleDC(hdc);
    if (!hdcMem) { fc->lpVtbl->Release(fc); frame->lpVtbl->Release(frame); dec->lpVtbl->Release(dec); return FALSE; }
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(hdcMem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm) { DeleteDC(hdcMem); fc->lpVtbl->Release(fc); frame->lpVtbl->Release(frame); dec->lpVtbl->Release(dec); return FALSE; }
    HBITMAP old = (HBITMAP)SelectObject(hdcMem, hbm);
    UINT stride = (UINT)w * 4u;
    UINT size   = stride * (UINT)h;
    fc->lpVtbl->CopyPixels(fc, NULL, stride, size, (BYTE*)bits);
    SelectObject(hdcMem, old);
    DeleteDC(hdcMem);
    fc->lpVtbl->Release(fc);
    frame->lpVtbl->Release(frame);
    dec->lpVtbl->Release(dec);
    if (!bits) return FALSE;
    *outHbm = hbm; *outW = w; *outH = h;
    return TRUE;
}


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
    f->body     = make_font(dpi, UiFaceName(), 105, 0, 0, 0); /* 10.5pt */
    f->bold     = make_font(dpi, UiFaceName(), 105, 1, 0, 0);
    f->ital     = make_font(dpi, UiFaceName(), 105, 0, 1, 0);
    f->boldital = make_font(dpi, UiFaceName(), 105, 1, 1, 0);
    f->mono     = make_font(dpi, MonoFaceName(), 95, 0, 0, 0);
    f->sup      = make_font(dpi, UiFaceName(), 75, 0, 0, 0);  /* 7.5pt */
    /* heading ladder: each level clearly larger than the next */
    f->h[0]     = make_font(dpi, UiFaceName(), 260, 1, 0, 0); /* 26pt */
    f->h[1]     = make_font(dpi, UiFaceName(), 200, 1, 0, 0); /* 20pt */
    f->h[2]     = make_font(dpi, UiFaceName(), 160, 1, 0, 0); /* 16pt */
    f->h[3]     = make_font(dpi, UiFaceName(), 135, 1, 0, 0); /* 13.5pt */
    f->h[4]     = make_font(dpi, UiFaceName(), 118, 1, 0, 0); /* 11.8pt */
    f->h[5]     = make_font(dpi, UiFaceName(), 108, 1, 0, 0); /* 10.8pt */
    f->bodyH = font_cellheight(hdc, f->body);
    f->monoH = font_cellheight(hdc, f->mono);
    f->supH  = font_cellheight(hdc, f->sup);
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
    if (f->sup)      DeleteObject(f->sup);
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

static void push_run(RunBuf *b, const wchar_t *p, int len, int flags,
                    const wchar_t *url, int urlLen)
{
    if (len <= 0) return;
    if (b->n == b->cap) {
        b->cap = round_up(b->cap);
        b->v = (MDRun *)realloc(b->v, b->cap * sizeof(MDRun));
    }
    b->v[b->n].ptr = p;
    b->v[b->n].len = len;
    b->v[b->n].flags = flags;
    b->v[b->n].url = url;
    b->v[b->n].urlLen = urlLen;
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
                      int *contentLen, int *urlStart, int *urlLen)
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
    *urlStart = closeEnd + 1;
    *urlLen = j - closeEnd - 1;
    return j + 1;
}

/* CommonMark escapable punctuation after a backslash */
static int is_escapable(wchar_t c)
{
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40)
        || (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
}

/* `<https://...>` autolink: scheme + no spaces inside until '>' */
static int match_autolink(const wchar_t *s, int len)
{
    if (s[0] != L'<') return 0;
    static const wchar_t *schemes[] = { L"http://", L"https://",
                                        L"ftp://", L"mailto:" };
    int i = 1, sch = 0;
    for (int k = 0; k < 4; k++) {
        int sl = lstrlenW(schemes[k]);
        if (len >= 1 + sl + 1 && !wcsncmp(s + 1, schemes[k], sl)) {
            sch = sl;
            break;
        }
    }
    if (!sch) return 0;
    i = 1 + sch;
    while (i < len && s[i] != L'>') {
        if (s[i] == L'<' || iswspace(s[i])) return 0;
        i++;
    }
    if (i >= len || i == 1 + sch) return 0;   /* empty or no '>' */
    return i + 1;
}

/* `[^label]: text` footnote definition line? */
static int is_footnote_def(const wchar_t *s, int len,
                           const wchar_t **label, int *labelLen,
                           const wchar_t **text, int *textLen)
{
    if (len < 5 || s[0] != L'[' || s[1] != L'^') return 0;
    int i = 2;
    while (i < len && s[i] != L']') {
        if (s[i] == L'[' || iswspace(s[i])) return 0;
        i++;
    }
    if (i >= len || i - 2 < 1 || i - 2 > 16) return 0;
    if (i + 1 >= len || s[i + 1] != L':') return 0;
    *label = s + 2;
    *labelLen = i - 2;
    int t = i + 2;
    if (t < len && s[t] == L' ') t++;
    *text = s + t;
    *textLen = len - t;
    return 1;
}

static void parse_inline(const wchar_t *s, int len, int flags, int depth,
                         RunBuf *out, const wchar_t *url, int urlLen)
{
    int i = 0;
    while (i < len) {
        wchar_t c = s[i];
        int matched = 0;

        if (c == L'\\' && i + 1 < len && is_escapable(s[i + 1])) {
            if (i > 0) push_run(out, s, i, flags, url, urlLen);
            push_run(out, s + i + 1, 1, flags, NULL, 0);
            i += 2; s += i; len -= i; i = 0;
            matched = 1;
        }
        else if (c == L'<') {
            int m = match_autolink(s + i, len - i);
            if (m > 0) {
                if (i > 0) push_run(out, s, i, flags, url, urlLen);
                const wchar_t *u = s + i + 1;
                int ul = m - 2;
                push_run(out, u, ul, flags | RF_LINK, u, ul);
                i += m; s += i; len -= i; i = 0;
                matched = 1;
            }
        }
        else if (depth < 3 && c == L'=' && i + 1 < len && s[i+1] == L'=') {
            int m = match_marker(s + i, len - i, L"==");
            if (m > 4) {
                if (i > 0) push_run(out, s, i, flags, url, urlLen);
                parse_inline(s + i + 2, m - 4, flags | RF_HL, depth + 1,
                             out, url, urlLen);
                i += m; s += i; len -= i; i = 0;
                matched = 1;
            }
        }
        else if (c == L'[' && i + 1 < len && s[i+1] == L'[') {
            /* wiki link [[target]] or [[target|label]] */
            int j = i + 2;
            while (j + 1 < len && !(s[j] == L']' && s[j+1] == L']')) j++;
            if (j + 1 < len && j > i + 2) {
                const wchar_t *tgt = s + i + 2;
                int tl = j - i - 2;
                const wchar_t *txt = tgt;
                int txtLen = tl;
                /* split on the first | : [[target|label]] */
                for (int k = 0; k < tl; k++) {
                    if (tgt[k] == L'|') {
                        txtLen = tl - k - 1;
                        txt = tgt + k + 1;
                        tl = k;
                        break;
                    }
                }
                if (tl >= 1 && tl < 260 && txtLen >= 1) {
                    if (i > 0) push_run(out, s, i, flags, url, urlLen);
                    push_run(out, txt, txtLen,
                             flags | RF_LINK | RF_WIKILINK, tgt, tl);
                    j += 2;
                    s += j; len -= j; i = 0;
                    matched = 1;
                }
            }
        }
        else if (c == L'[' && i + 2 < len && s[i+1] == L'^') {
            /* inline footnote reference [^label] */
            int j = i + 2;
            while (j < len && s[j] != L']') {
                if (s[j] == L'[' || iswspace(s[j])) break;
                j++;
            }
            if (j < len && s[j] == L']' && j - i - 2 >= 1
                && j - i - 2 <= 16) {
                if (i > 0) push_run(out, s, i, flags, url, urlLen);
                push_run(out, s + i + 1, j - i - 1, flags | RF_SUP,
                         NULL, 0);
                j++;
                s += j; len -= j; i = 0;
                matched = 1;
            }
        }
        else if (c == L'`') {
            int j = i + 1;
            while (j < len && s[j] != L'`') j++;
            if (j < len && j > i + 1) {
                if (i > 0) push_run(out, s, i, flags, url, urlLen);
                push_run(out, s + i + 1, j - i - 1, flags | RF_CODE,
                         NULL, 0);
                s += j + 1; len -= j + 1; i = 0;
                matched = 1;
            }
        }
        else if (depth < 3 && c == L'*' && i + 1 < len && s[i+1] == L'*') {
            int m = match_marker(s + i, len - i, L"**");
            if (m > 4) {
                if (i > 0) push_run(out, s, i, flags, url, urlLen);
                parse_inline(s + i + 2, m - 4, flags | RF_BOLD, depth + 1,
                             out, url, urlLen);
                i += m; s += i; len -= i; i = 0;
                matched = 1;
            }
        }
        else if (depth < 3 && c == L'*' && i + 1 < len && s[i+1] != L'*'
                 && !iswspace(s[i+1])) {
            int m = match_marker(s + i, len - i, L"*");
            if (m > 2) {
                if (i > 0) push_run(out, s, i, flags, url, urlLen);
                parse_inline(s + i + 1, m - 2, flags | RF_ITALIC, depth + 1,
                             out, url, urlLen);
                i += m; s += i; len -= i; i = 0;
                matched = 1;
            }
        }
        else if (depth < 3 && c == L'~' && i + 1 < len && s[i+1] == L'~') {
            int m = match_marker(s + i, len - i, L"~~");
            if (m > 4) {
                if (i > 0) push_run(out, s, i, flags, url, urlLen);
                parse_inline(s + i + 2, m - 4, flags | RF_STRIKE, depth + 1,
                             out, url, urlLen);
                i += m; s += i; len -= i; i = 0;
                matched = 1;
            }
        }
        else if (c == L'[' || (c == L'!' && i + 1 < len && s[i+1] == L'[')) {
            int off = (c == L'!') ? 1 : 0;
            int cs = 0, cl = 0, us = 0, ul = 0;
            const wchar_t *base = s + i + off;
            int m = match_link(base, len - i - off, &cs, &cl, &us, &ul);
            if (m > 0) {
                if (i > 0) push_run(out, s, i, flags, url, urlLen);
                int lf = flags | (off ? RF_IMAGE : RF_LINK);
                parse_inline(s + i + off + cs, cl, lf, depth + 1, out,
                             off ? NULL : base + us, off ? 0 : ul);
                i += off + m; s += i; len -= i; i = 0;
                matched = 1;
            }
        }

        if (!matched) i++;
    }
    if (len > 0) push_run(out, s, len, flags, url, urlLen);
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
/* tables                                                              */
/* ------------------------------------------------------------------ */

#define TB_PADX 8
#define TB_PADY 5

/* delimiter row: only | - : and spaces, at least one dash */
static int is_delim_row(const wchar_t *s, int len)
{
    int dashes = 0, bars = 0;
    for (int i = 0; i < len; i++) {
        wchar_t c = s[i];
        if (c == L'-') dashes++;
        else if (c == L'|') bars++;
        else if (c == L':' || c == L' ') continue;
        else return 0;
    }
    return dashes >= 1 && bars >= 1;
}

/* parse alignment out of a delimiter row cell text */
static char parse_align(const wchar_t *s, int len)
{
    int first = -1, last = -1;
    for (int i = 0; i < len; i++)
        if (s[i] == L':') { if (first < 0) first = i; last = i; }
    if (first == 0 && last == len - 1 && len > 0) return 1; /* :---: */
    if (last == len - 1 && len > 0) return 2;               /* ---:   */
    return 0;                                               /* :--- --- */
}

/* split "|a|b|c" into cell ranges (ptr/len, trimmed); returns count.
 * cells points at a caller-provided MD_MAX_COLS array. */
static int split_cells(const wchar_t *s, int len, MDCell *cells)
{
    int n = 0;
    int i = 0;
    if (i < len && s[i] == L'|') i++;       /* leading bar */
    while (i < len && n < MD_MAX_COLS) {
        int j = i;
        while (j < len && s[j] != L'|') j++;
        int a = i, b = j;
        while (a < b && iswspace(s[a])) a++;
        while (b > a && iswspace(s[b - 1])) b--;
        cells[n].ptr = s + a;
        cells[n].len = b - a;
        cells[n].subs = NULL;
        cells[n].nsubs = 0;
        n++;
        i = j + 1;
    }
    return n;
}

/* cache code block extents for fast background painting */
static void push_code_block(MDDoc *d, int id, int top, int bottom)
{
    if (d->ncodeBlocks == d->capCodeBlocks) {
        d->capCodeBlocks = round_up(d->capCodeBlocks);
        d->codeBlocks = (MDCodeBlock *)realloc(d->codeBlocks,
                           d->capCodeBlocks * sizeof(MDCodeBlock));
    }
    d->codeBlocks[d->ncodeBlocks].id = id;
    d->codeBlocks[d->ncodeBlocks].top = top;
    d->codeBlocks[d->ncodeBlocks].bottom = bottom;
    d->ncodeBlocks++;
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
    typedef struct Tok { const wchar_t *ptr; int len; int flags; int isSpace; int wid;
                          const wchar_t *url; int urlLen; } Tok;
    Tok *t = NULL;
    int nt = 0, tcap = 0;
    for (int r = 0; r < rb->n; r++) {
        const wchar_t *p = rb->v[r].ptr;
        int len = rb->v[r].len, flags = rb->v[r].flags;
        const wchar_t *url = rb->v[r].url; int urlLen = rb->v[r].urlLen;
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
            t[nt].url = url;
            t[nt].urlLen = urlLen;
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
                push_run(&cur, t[i].ptr, t[i].len, t[i].flags,
                             t[i].url, t[i].urlLen);
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
            /* never split a UTF-16 surrogate pair across lines */
            if (lo < t[i].len
                && (t[i].ptr[lo - 1] & 0xFC00) == 0xD800
                && (t[i].ptr[lo] & 0xFC00) == 0xDC00)
                lo++;
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
        push_run(&cur, t[i].ptr, t[i].len, t[i].flags,
                             t[i].url, t[i].urlLen);
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

    int inCode = 0, codeId = 0, codeStartY = 0;
    int y = 0, pos = 0;
    int pendingBlank = 0;
    int headingLevel = 0;
    int inAlert = 0;        /* inside a "> [!TYPE]" callout card */
    int total = srcLen;
    const wchar_t *p = doc->text;

    if (total == 0) {                   /* empty doc still renders one line */
        MDLine *L = push_line(doc, LT_BLANK);
        L->height = f->bodyH;
        L->y = 0;
        doc->height = L->height;
        return;
    }

    while (pos < total) {
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

        if (s[0] != L'>') inAlert = 0;   /* left the callout card */

        if (inCode) {
            if (len >= 3 && s[0] == L'`' && s[1] == L'`' && s[2] == L'`') {
                L = push_line(doc, LT_CODEPAD);
                L->code = codeId;
                L->height = 10;
                L->y = y; y += L->height;
                inCode = 0;
                push_code_block(doc, codeId, codeStartY, y);
                pos += advance;
                if (pos > total) break;
                continue;
            } else {
                L = push_line(doc, LT_CODE);
                L->code = codeId;
                push_run(&rb, s, len, RF_CODE, NULL, 0);
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
            codeStartY = L->y;
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

        if (s[0] == L'|' || wmemchr(s, L'|', len) != NULL) {
            /* peek next physical line for a GFM delimiter row */
            int npos = pos + advance;
            const wchar_t *ns = p + npos;
            int nlenRaw = 0;
            while (npos + nlenRaw < total && p[npos + nlenRaw] != L'\n')
                nlenRaw++;
            int nlen = nlenRaw;
            while (nlen > 0 && ns[nlen - 1] == L'\r') nlen--;
            int nlead = lead_spaces(ns, nlen);
            if (npos <= total && nlen - nlead > 0
                && is_delim_row(ns + nlead, nlen - nlead)) {
                MDCell hc[MD_MAX_COLS];
                int ncols = split_cells(s, len, hc);
                if (ncols > 0) {
                    char aligns[MD_MAX_COLS] = { 0 };
                    {
                        MDCell dc[MD_MAX_COLS];
                        int nd = split_cells(ns + nlead, nlen - nlead, dc);
                        for (int c = 0; c < ncols && c < nd; c++)
                            aligns[c] = parse_align(dc[c].ptr, dc[c].len);
                    }

                    /* gather body rows while lines contain a pipe
                     * (start past the delimiter row itself) */
                    typedef struct { const wchar_t *s; int len; } RowRef;
                    int capR = 16, nR = 0;
                    RowRef *rows = (RowRef *)malloc(capR * sizeof(RowRef));
                    int tpos = pos + advance + nlenRaw + 1;
                    while (tpos < total) {
                        const wchar_t *rs = p + tpos;
                        int rlen = 0;
                        while (tpos + rlen < total && p[tpos + rlen] != L'\n')
                            rlen++;
                        int radv = rlen + 1;
                        while (rlen > 0 && rs[rlen - 1] == L'\r') rlen--;
                        int rlead = lead_spaces(rs, rlen);
                        if (rlen - rlead < 1
                            || !wmemchr(rs + rlead, L'|', rlen - rlead))
                            break;
                        if (nR == capR) {
                            capR *= 2;
                            rows = (RowRef *)realloc(rows,
                                                    capR * sizeof(RowRef));
                        }
                        rows[nR].s = rs + rlead;
                        rows[nR].len = rlen - rlead;
                        nR++;
                        tpos += radv;
                    }

                    /* natural column widths (raw text, capped) */
                    int natural[MD_MAX_COLS] = { 0 };
                    int lineH2 = f->bodyH + (f->bodyH >> 1);
                    for (int c = 0; c < ncols; c++) {
                        int w = text_w(hdc, f->bold, hc[c].ptr, hc[c].len)
                                + 2 * TB_PADX;
                        natural[c] = w < 24 ? 24 : w;
                    }
                    MDCell (*rowCells)[MD_MAX_COLS] = NULL;
                    if (nR) rowCells = (MDCell(*)[MD_MAX_COLS])
                        calloc(nR, sizeof(MDCell) * MD_MAX_COLS);
                    for (int r = 0; r < nR; r++) {
                        int nc2 = split_cells(rows[r].s, rows[r].len,
                                              rowCells[r]);
                        for (int c = 0; c < nc2 && c < ncols; c++) {
                            int w = text_w(hdc, f->body,
                                           rowCells[r][c].ptr,
                                           rowCells[r][c].len)
                                    + 2 * TB_PADX;
                            if (w > natural[c]) natural[c] = w;
                        }
                    }
                    for (int c = 0; c < ncols; c++)
                        if (natural[c] > 280) natural[c] = 280;

                    /* distribute available width */
                    int colW[MD_MAX_COLS];
                    int availW = contentW;
                    int sum = 0;
                    for (int c = 0; c < ncols; c++) sum += natural[c];
                    if (sum <= availW) {
                        int extra = (availW - sum) / ncols;
                        for (int c = 0; c < ncols; c++)
                            colW[c] = natural[c] + extra;
                    } else {
                        for (int c = 0; c < ncols; c++) {
                            int w = (int)((long long)natural[c] * availW
                                          / sum);
                            colW[c] = w < 48 ? 48 : w;
                        }
                        int over = 0;
                        for (int c = 0; c < ncols; c++) over += colW[c];
                        while (over > availW) {
                            int cut = 0;
                            for (int c = 0; c < ncols; c++) {
                                if (colW[c] > 48) { colW[c]--; over--; cut = 1; }
                                if (over <= availW) break;
                            }
                            if (!cut) break;
                        }
                    }

                    /* emit header row */
                    MDLine *H = push_line(doc, LT_TABLEROW);
                    H->isHeader = 1;
                    H->ncells = ncols;
                    H->cells = (MDCell *)malloc(ncols * sizeof(MDCell));
                    int hMaxH = 0;
                    for (int c = 0; c < ncols; c++) {
                        H->cells[c] = hc[c];
                        RunBuf crb = { 0 };
                        parse_inline(hc[c].ptr, hc[c].len, RF_BOLD, 0,
                                     &crb, NULL, 0);
                        MDLine tmp = { 0 };
                        wrap_line(&tmp, &crb, f, hdc,
                                  colW[c] - 2 * TB_PADX, lineH2, 0);
                        free_runs(&crb);
                        H->cells[c].subs = tmp.subs;
                        H->cells[c].nsubs = tmp.nsubs;
                        int hh = 0;
                        for (int k = 0; k < tmp.nsubs; k++)
                            hh += tmp.subs[k].height;
                        if (hh > hMaxH) hMaxH = hh;
                    }
                    H->height = hMaxH + 2 * TB_PADY;
                    H->y = y;
                    y += H->height;
                    memcpy(H->aligns, aligns, MD_MAX_COLS);
                    memcpy(H->colW, colW, sizeof(int) * MD_MAX_COLS);

                    /* emit body rows */
                    for (int r = 0; r < nR; r++) {
                        MDLine *B = push_line(doc, LT_TABLEROW);
                        B->ncells = ncols;
                        B->cells = (MDCell *)malloc(ncols * sizeof(MDCell));
                        int rMaxH = 0;
                        for (int c = 0; c < ncols; c++) {
                            if (c < MD_MAX_COLS && rowCells[r][c].ptr)
                                B->cells[c] = rowCells[r][c];
                            else {
                                B->cells[c].ptr = rows[r].s;
                                B->cells[c].len = 0;
                                B->cells[c].subs = NULL;
                                B->cells[c].nsubs = 0;
                            }
                            if (B->cells[c].len == 0) continue;
                            RunBuf crb = { 0 };
                            parse_inline(B->cells[c].ptr, B->cells[c].len,
                                         0, 0, &crb, NULL, 0);
                            MDLine tmp = { 0 };
                            wrap_line(&tmp, &crb, f, hdc,
                                      colW[c] - 2 * TB_PADX, lineH2, 0);
                            free_runs(&crb);
                            B->cells[c].subs = tmp.subs;
                            B->cells[c].nsubs = tmp.nsubs;
                            int hh = 0;
                            for (int k = 0; k < tmp.nsubs; k++)
                                hh += tmp.subs[k].height;
                            if (hh > rMaxH) rMaxH = hh;
                        }
                        B->height = rMaxH + 2 * TB_PADY;
                        B->y = y;
                        y += B->height;
                        memcpy(B->aligns, aligns, MD_MAX_COLS);
                        memcpy(B->colW, colW, sizeof(int) * MD_MAX_COLS);
                    }
                    free(rows);
                    free(rowCells);
                    pos = tpos;
                    if (pos > total) break;
                    continue;
                }
            }
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

        /* footnote definition line: stash, don't emit yet */
        {
            const wchar_t *fl = NULL, *ft = NULL;
            int fll = 0, ftl = 0;
            if (is_footnote_def(s, len, &fl, &fll, &ft, &ftl)
                && doc->nfootnotes < 64) {
                MDFootnote *fn = &doc->footnotes[doc->nfootnotes++];
                fn->label = fl; fn->labelLen = fll;
                fn->text = ft;  fn->textLen = ftl;
                pos += advance;
                if (pos >= total) break;
                continue;
            }
        }

        if (s[0] == L'>') {
            int qlevel = 0;
            while (qlevel < 3 && qlevel < len && s[qlevel] == L'>') qlevel++;
            s += qlevel; len -= qlevel;
            if (len > 0 && s[0] == L' ') { s++; len--; }
            L = push_line(doc, LT_QUOTE);
            L->depth = qlevel;
            /* GitHub-style alert: "> [!NOTE]" opens a callout card.
             * 1=NOTE 2=TIP 3=IMPORTANT 4=WARNING 5=CAUTION, 6=body */
            if (qlevel == 1 && len >= 6 && s[0] == L'[' && s[1] == L'!') {
                static const wchar_t *const anames[5] = {
                    L"NOTE]", L"TIP]", L"IMPORTANT]", L"WARNING]",
                    L"CAUTION]" };
                for (int a = 0; a < 5; a++) {
                    int al = 0;
                    while (al < 11 && anames[a][al] && s[2 + al] == anames[a][al])
                        al++;
                    if (anames[a][al] == 0) {
                        L->alert = (char)(a + 1);
                        s += 2 + al; len -= 2 + al;
                        if (len > 0 && s[0] == L' ') { s++; len--; }
                        inAlert = 1;
                        goto have_line;
                    }
                }
            }
            if (inAlert && qlevel == 1) L->alert = 6;
            goto have_line;
        }

        if ((s[0] == L'-' || s[0] == L'*' || s[0] == L'+') &&
            len > 1 && s[1] == L' ') {
            L = push_line(doc, LT_ITEM);
            L->depth = ls_spaces / 2;
            s += 2; len -= 2;
            if (len >= 3 && s[0] == L'[' && s[2] == L']'
                && (s[1] == L' ' || s[1] == L'x' || s[1] == L'X')) {
                L->task = (s[1] == L' ') ? 1 : 2;
                s += 3; len -= 3;
                if (len > 0 && s[0] == L' ') { s++; len--; }
            }
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
        /* "==full line==" renders as a highlighted bar (Notion style) */
        if (len >= 5 && s[0] == L'=' && s[1] == L'='
            && s[len-1] == L'=' && s[len-2] == L'=') {
            L->hlbar = 1;
            s += 2; len -= 4;
        }

    have_line:
        parse_inline(s, len, 0, 0, &rb, NULL, 0);
        wrap_line(L, &rb, f, hdc,
                  contentW - (L->type == LT_QUOTE
                              ? (L->alert ? 56 : 20) : 0), lineH,
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

    if (inCode) push_code_block(doc, codeId, codeStartY, y);

    /* footnote section: separator + one indented line per definition */
    if (doc->nfootnotes > 0) {
        int lineH = f->bodyH + (f->bodyH >> 1);
        MDLine *sep = push_line(doc, LT_HR);
        sep->height = f->bodyH + 12;
        sep->y = y;
        y += sep->height;
        for (int i = 0; i < doc->nfootnotes; i++) {
            MDFootnote *fn = &doc->footnotes[i];
            MDLine *FL = push_line(doc, LT_TEXT);
            RunBuf rb = {0};
            push_run(&rb, fn->label, fn->labelLen, RF_SUP, NULL, 0);
            static const wchar_t dot[3] = L". ";
            push_run(&rb, dot, 2, 0, NULL, 0);
            if (fn->textLen > 0)
                parse_inline(fn->text, fn->textLen, 0, 0, &rb, NULL, 0);
            wrap_line(FL, &rb, f, hdc, contentW - 24, lineH, 0);
            free_runs(&rb);
            FL->padTop = 0;
            FL->height = 0;
            for (int k = 0; k < FL->nsubs; k++)
                FL->height += FL->subs[k].height;
            FL->y = y;
            y += FL->height;
        }
    }

    doc->height = y;

    /* cache run pixel widths once so painting never re-measures */
    for (int li = 0; li < doc->nlines; li++) {
        MDLine *L = &doc->lines[li];
        int hl = (L->type >= LT_H1 && L->type <= LT_H6)
                 ? L->type - LT_H1 + 1 : 0;
        for (int k = 0; k < L->nsubs; k++)
            for (int ri = 0; ri < L->subs[k].nruns; ri++) {
                MDRun *r = &L->subs[k].runs[ri];
                HFONT cf = (r->flags & RF_SUP) ? f->sup
                           : font_for(f, r->flags, hl);
                r->w = text_w(hdc, cf, r->ptr, r->len);
            }
        for (int c = 0; c < L->ncells; c++)
            for (int k = 0; k < L->cells[c].nsubs; k++)
                for (int ri = 0; ri < L->cells[c].subs[k].nruns; ri++) {
                    MDRun *r = &L->cells[c].subs[k].runs[ri];
                    r->w = text_w(hdc, font_for(f, r->flags, 0),
                                  r->ptr, r->len);
                }
    }
}

void md_free(MDDoc *doc)
{
    if (!doc) return;
    for (int i = 0; i < doc->nlines; i++) {
        MDLine *L = &doc->lines[i];
        for (int k = 0; k < L->nsubs; k++)
            free(L->subs[k].runs);
        free(L->subs);
        for (int c = 0; c < L->ncells; c++) {
            for (int k = 0; k < L->cells[c].nsubs; k++)
                free(L->cells[c].subs[k].runs);
            free(L->cells[c].subs);
        }
        free(L->cells);
    }
    free(doc->lines);
    free(doc->codeBlocks);
    free(doc->text);
    ZeroMemory(doc, sizeof(*doc));
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */

/* link hit-rect reporting (set by host for clickable links) */
static MdLinkSink g_linkSink;
static void      *g_linkCtx;
/* current doc dir for resolving image paths */
static wchar_t    g_docPath[MAX_PATH];

void md_set_link_sink(MdLinkSink cb, void *ctx)
{
    g_linkSink = cb;
    g_linkCtx = ctx;
}

static void draw_runs(HDC hdc, const MDSub *sub, int x, int y,
                      const MDFonts *f, COLORREF defCol, int hl)
{
    SetBkMode(hdc, TRANSPARENT);
    for (int i = 0; i < sub->nruns; i++) {
        const MDRun *r = &sub->runs[i];
        int sup = (r->flags & RF_SUP) != 0;
        HFONT fo = sup ? f->sup : font_for(f, r->flags, hl);
        HFONT old = (HFONT)SelectObject(hdc, fo);
        int tmH = sup ? f->supH
                  : (r->flags & RF_CODE) ? f->monoH
                  : hl ? f->hH[hl - 1] : f->bodyH;
        int w = r->w;
        int ry = y + (sub->height - tmH) / 2 - (sup ? tmH : 0);

        /* report clickable link rect to the host */
        if ((r->flags & RF_LINK) && g_linkSink && r->url && r->urlLen > 0
            && r->urlLen < 4096) {
            RECT lrc = { x, ry, x + w, ry + tmH };
            g_linkSink(g_linkCtx, lrc, r->url, r->urlLen,
                       (r->flags & RF_WIKILINK) != 0);
        }

        if (r->flags & RF_CODE) {
            RECT rc = { x, ry, x + w + 4, ry + tmH };
            HRGN rg = CreateRoundRectRgn(rc.left, rc.top, rc.right, rc.bottom,
                                         6, 6);
            HBRUSH br = CreateSolidBrush(g_colCodeInlineBg);
            FillRgn(hdc, rg, br);
            DeleteObject(rg);
            DeleteObject(br);
        }
        if (r->flags & RF_HL) {
            RECT rc = { x - 1, ry, x + w + 1, ry + tmH };
            HBRUSH br = CreateSolidBrush(RGB(255, 244, 181));
            FillRect(hdc, &rc, br);
            DeleteObject(br);
        }

        COLORREF col = defCol;
        if (r->flags & RF_LINK) col = g_colLink;
        if (r->flags & RF_IMAGE) col = g_colQuote;

        if (r->flags & RF_STRIKE) {
            RECT rc = { x, ry + tmH / 2, x + w,
                        ry + tmH / 2 + 1 };
            HBRUSH br = CreateSolidBrush(col);
            FillRect(hdc, &rc, br);
            DeleteObject(br);
        }

        /* image: load and composite the bitmap */
        if (r->flags & RF_IMAGE && r->url && r->urlLen > 0
            && r->urlLen < 4096) {
            wchar_t imgPath[MAX_PATH];
            if (g_docPath[0])
                wsprintfW(imgPath, L"%s\\%.*S", g_docPath, r->urlLen, r->url);
            else
                lstrcpynW(imgPath, r->url, MAX_PATH);
            HBITMAP hbm = NULL; int iw = 0, ih = 0;
            HDC hdcMem = CreateCompatibleDC(hdc);
            if (hdcMem && LoadWicBitmap(hdc, imgPath, &hbm, &iw, &ih, 1280)) {
                RECT ir = { x, ry + 2, x + w, ry + tmH };
                if (iw > 0 && ih > 0)
                    StretchBlt(hdc, ir.left, ir.top, ir.right - ir.left,
                               ir.bottom - ir.top, hdcMem, 0, 0, iw, ih, SRCCOPY);
                DeleteObject(hbm);
            }
            if (hdcMem) DeleteDC(hdcMem);
            x += w + (r->flags & RF_CODE ? 4 : 0);
            continue;
        }

        SetTextColor(hdc, col);
        ExtTextOutW(hdc, x + (r->flags & RF_CODE ? 2 : 0), ry, 0, NULL,
                    r->ptr, r->len, NULL);

        if (r->flags & RF_LINK) {
            HPEN pen = CreatePen(PS_SOLID, 1, g_colLink);
            HPEN op = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, x, ry + tmH, NULL);
            LineTo(hdc, x + w, ry + tmH);
            SelectObject(hdc, op);
            DeleteObject(pen);
        }
        SelectObject(hdc, old);
        x += w + (r->flags & RF_CODE ? 4 : 0);
    }
}

void md_paint(const MDDoc *doc, HDC hdc, const RECT *rc, int scrollY,
              const MDFonts *f, const wchar_t *docPath)
{
    if (!doc || !doc->lines) return;
    /* cache the doc path so draw_runs can resolve relative image paths */
    if (docPath)
        lstrcpynW(g_docPath, docPath, MAX_PATH);
    else
        g_docPath[0] = 0;

    FillRect(hdc, rc, (HBRUSH)GetStockObject(WHITE_BRUSH));

    int margin = 24;
    int contentW = rc->right - rc->left - margin * 2;
    int yOff = rc->top;          /* document y=0 maps here */
    int viewTop = scrollY;
    int viewBottom = scrollY + (rc->bottom - rc->top);

    /* pass 1: code block backgrounds (extents cached at build) */
    for (int b = 0; b < doc->ncodeBlocks; b++) {
        int top = doc->codeBlocks[b].top;
        int bottom = doc->codeBlocks[b].bottom;
        if (bottom <= viewTop || top >= viewBottom) continue;
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

    /* pass 2: content -- binary-search the first visible line */
    int lo = 0, hi = doc->nlines;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (doc->lines[mid].y + doc->lines[mid].height < viewTop)
            lo = mid + 1;
        else hi = mid;
    }
    for (int i = lo; i < doc->nlines; i++) {
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

        case LT_TABLEROW: {
            /* header: light fill + heavier rule; rows: hairlines */
            if (L->isHeader) {
                RECT hb = { x, top, x + contentW, bottom };
                HBRUSH br = CreateSolidBrush(g_colCodeBg);
                FillRect(hdc, &hb, br);
                DeleteObject(br);
                HPEN pen = CreatePen(PS_SOLID, 2, g_colQuoteBar);
                HPEN op = (HPEN)SelectObject(hdc, pen);
                MoveToEx(hdc, x, bottom - 1, NULL);
                LineTo(hdc, x + contentW, bottom - 1);
                SelectObject(hdc, op);
                DeleteObject(pen);
            } else {
                HPEN pen = CreatePen(PS_SOLID, 1, g_colHr);
                HPEN op = (HPEN)SelectObject(hdc, pen);
                MoveToEx(hdc, x, bottom - 1, NULL);
                LineTo(hdc, x + contentW, bottom - 1);
                SelectObject(hdc, op);
                DeleteObject(pen);
            }
            /* vertical separators */
            {
                HPEN pen = CreatePen(PS_SOLID, 1, g_colHeadingRule);
                HPEN op = (HPEN)SelectObject(hdc, pen);
                int vx = x;
                for (int c = 0; c < L->ncells; c++) {
                    vx += L->colW[c];
                    if (c == L->ncells - 1) break;
                    MoveToEx(hdc, vx - 1, top, NULL);
                    LineTo(hdc, vx - 1, bottom);
                }
                SelectObject(hdc, op);
                DeleteObject(pen);
            }
            /* cells, honoring column alignment */
            {
                int vx = x;
                for (int c = 0; c < L->ncells; c++) {
                    MDCell *cell = &L->cells[c];
                    int cw = L->colW[c];
                    int cy = top + TB_PADY;
                    for (int k = 0; k < cell->nsubs; k++) {
                        MDSub *sub = &cell->subs[k];
                        int subW = 0;
                        for (int ri = 0; ri < sub->nruns; ri++)
                            subW += sub->runs[ri].w
                                    + (sub->runs[ri].flags & RF_CODE ? 4 : 0);
                        int tx = vx + TB_PADX;
                        if (L->aligns[c] == 1)
                            tx = vx + (cw - subW) / 2;
                        else if (L->aligns[c] == 2)
                            tx = vx + cw - subW - TB_PADX;
                        if (tx < vx + 2) tx = vx + 2;
                        draw_runs(hdc, sub, tx, cy, f, g_colText, 0);
                        cy += sub->height;
                    }
                    vx += cw;
                }
            }
            break;
        }

        case LT_QUOTE: {
            /* GitHub-style alert card: colored bg + left bar + tag */
            if (L->alert) {
                int at = L->alert;
                if (at == 6) {   /* body line: find the card's type */
                    for (int k = i - 1; k >= 0; k--) {
                        MDLine *Q = &doc->lines[k];
                        if (Q->type == LT_QUOTE && Q->alert >= 1
                            && Q->alert <= 5) { at = Q->alert; break; }
                        if (Q->type != LT_QUOTE || Q->depth != 1) break;
                    }
                    if (at == 6) at = 1;
                }
                static const COLORREF acol[6] = {
                    0, RGB(9,105,218), RGB(26,127,55), RGB(130,80,223),
                    RGB(154,103,0), RGB(207,34,46) };
                static const COLORREF abg[6] = {
                    0, RGB(221,244,255), RGB(218,251,225),
                    RGB(251,239,255), RGB(255,248,197), RGB(255,235,233) };
                static const wchar_t *const atag[6] = {
                    NULL, L"\x26A0 \x6CE8", L"\x26A0 \x63D0\x793A",
                    L"\x26A0 \x91CD\x8981", L"\x26A0 \x8B66\x544A",
                    L"\x26A0 \x5371\x9669" };
                int cw = doc->width - 24;
                RECT card = { x - 4, top, x - 4 + cw, bottom };
                HBRUSH bg = CreateSolidBrush(abg[at]);
                FillRect(hdc, &card, bg);
                DeleteObject(bg);
                RECT bar = { card.left, card.top, card.left + 4, card.bottom };
                HBRUSH bb = CreateSolidBrush(acol[at]);
                FillRect(hdc, &bar, bb);
                DeleteObject(bb);
                int tx = x + 14;
                if (L->alert >= 1 && L->alert <= 5) {
                    /* tag line: bold colored label, then the text runs */
                    HFONT old = (HFONT)SelectObject(hdc, f->bold);
                    SetTextColor(hdc, acol[at]);
                    SetBkMode(hdc, TRANSPARENT);
                    int tl = lstrlenW(atag[at]);
                    SIZE tsz; GetTextExtentPoint32W(hdc, atag[at], tl, &tsz);
                    ExtTextOutW(hdc, tx, top + L->padTop, 0, NULL,
                                atag[at], tl, NULL);
                    SelectObject(hdc, old);
                    for (int k = 0; k < L->nsubs; k++) {
                        draw_runs(hdc, &L->subs[k],
                                  tx + tsz.cx + 10, subY, f, g_colText, 0);
                        subY += L->subs[k].height;
                    }
                    break;
                }
                for (int k = 0; k < L->nsubs; k++) {
                    draw_runs(hdc, &L->subs[k], tx, subY, f, g_colText, 0);
                    subY += L->subs[k].height;
                }
                break;
            }
            HPEN pen = CreatePen(PS_SOLID | PS_ENDCAP_ROUND, 3, g_colQuoteBar);
            HPEN op = (HPEN)SelectObject(hdc, pen);
            for (int q = 0; q < L->depth; q++) {
                MoveToEx(hdc, x + 1 + q * 14, top, NULL);
                LineTo(hdc, x + 1 + q * 14, bottom);
            }
            SelectObject(hdc, op);
            DeleteObject(pen);
            int qi = L->depth * 14 + 4;
            for (int k = 0; k < L->nsubs; k++) {
                draw_runs(hdc, &L->subs[k], x + qi + 12, subY, f,
                          g_colQuote, 0);
                subY += L->subs[k].height;
            }
            break;
        }

        case LT_ITEM: {
            int bx = x + L->depth * 18;
            if (L->task) {
                /* 14x14 rounded checkbox, accent fill when checked */
                int sz = 14;
                int boxY = top + L->padTop + (f->bodyH - sz) / 2;
                RECT cb = { bx, boxY, bx + sz, boxY + sz };
                HRGN rg = CreateRoundRectRgn(cb.left, cb.top, cb.right,
                                             cb.bottom, 4, 4);
                HBRUSH br = CreateSolidBrush(
                    L->task == 2 ? g_colLink : RGB(255, 255, 255));
                FillRgn(hdc, rg, br);
                DeleteObject(br);
                FrameRgn(hdc, rg, CreateSolidBrush(
                    L->task == 2 ? g_colLink : g_colQuoteBar), 1, 1);
                DeleteObject(rg);
                if (L->task == 2) {
                    /* check mark */
                    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
                    HPEN op = (HPEN)SelectObject(hdc, pen);
                    MoveToEx(hdc, bx + 3, boxY + 7, NULL);
                    LineTo(hdc, bx + 6, boxY + 10);
                    LineTo(hdc, bx + 11, boxY + 3);
                    SelectObject(hdc, op);
                    DeleteObject(pen);
                }
                for (int k = 0; k < L->nsubs; k++) {
                    draw_runs(hdc, &L->subs[k], bx + 22, subY, f,
                              g_colText, 0);
                    subY += L->subs[k].height;
                }
                break;
            }
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
            if (L->hlbar) {   /* highlighted line: yellow bar behind */
                RECT hb = { x - 8, top + 1, x + contentW + 4, bottom - 1 };
                HBRUSH br = CreateSolidBrush(RGB(255, 244, 181));
                HRGN rg = CreateRoundRectRgn(hb.left, hb.top,
                                             hb.right, hb.bottom, 6, 6);
                FillRgn(hdc, rg, br);
                DeleteObject(rg);
                DeleteObject(br);
            }
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

/* ---- standalone share export: embed local images as base64 data URIs ----
 * enabled only while md_to_html_standalone() runs (reuses ReadAllBytes
 * from gitlite.c so no new file-io code is needed here). */
extern BOOL ReadAllBytes(const wchar_t *path, char **buf, int *len);

static BOOL g_htmlEmbed = FALSE;
static wchar_t g_htmlImgDir[MAX_PATH];

/* mime type from the url extension, or NULL when unknown */
static const char *img_mime(const wchar_t *url, int len)
{
    int dot = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (url[i] == L'.') { dot = i; break; }
        if (url[i] == L'/' || url[i] == L'\\') break;
    }
    if (dot < 0) return NULL;
    static const struct { const wchar_t *ext; const char *mime; } TBL[] = {
        { L"png",  "image/png"  },
        { L"jpg",  "image/jpeg" },
        { L"jpeg", "image/jpeg" },
        { L"gif",  "image/gif"  },
        { L"bmp",  "image/bmp"  },
        { L"webp", "image/webp" },
    };
    for (int k = 0; k < (int)(sizeof(TBL) / sizeof(TBL[0])); k++) {
        int ml = lstrlenW(TBL[k].ext);
        if (len - dot - 1 != ml) continue;
        BOOL same = TRUE;
        for (int i = 0; i < ml; i++)
            if ((wchar_t)towlower((wint_t)url[dot + 1 + i]) != TBL[k].ext[i]) {
                same = FALSE; break;
            }
        if (same) return TBL[k].mime;
    }
    return NULL;
}

/* resolve an image url against g_htmlImgDir; FALSE for remote/anchor/data */
static BOOL local_img_path(const wchar_t *url, int len, wchar_t *out, int cch)
{
    if (len <= 0 || len >= MAX_PATH) return FALSE;
    for (int i = 0; i + 2 < len; i++)
        if (url[i] == L':' && url[i + 1] == L'/' && url[i + 2] == L'/')
            return FALSE;                          /* http(s):// etc. */
    if (url[0] == L'#') return FALSE;
    if ((len >= 2 && url[1] == L':')
        || url[0] == L'\\' || url[0] == L'/') {    /* absolute path */
        for (int i = 0; i < len && i < cch - 1; i++) out[i] = url[i];
        out[len < cch - 1 ? len : cch - 1] = 0;
        return TRUE;
    }
    if (!g_htmlImgDir[0]) return FALSE;
    int dl = lstrlenW(g_htmlImgDir);
    if (dl + 1 + len >= cch) return FALSE;
    lstrcpynW(out, g_htmlImgDir, cch);
    out[dl] = L'\\';
    for (int i = 0; i < len; i++) out[dl + 1 + i] = url[i];
    out[dl + 1 + len] = 0;
    return TRUE;
}

static void h_app_b64(HtmlOut *o, const char *in, int n)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                            "abcdefghijklmnopqrstuvwxyz0123456789+/";
    char quad[5];
    for (int i = 0; i < n; i += 3) {
        unsigned v = ((unsigned)(unsigned char)in[i]) << 16;
        if (i + 1 < n) v |= ((unsigned)(unsigned char)in[i + 1]) << 8;
        if (i + 2 < n) v |= (unsigned)(unsigned char)in[i + 2];
        quad[0] = T[(v >> 18) & 63];
        quad[1] = T[(v >> 12) & 63];
        quad[2] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        quad[3] = (i + 2 < n) ? T[v & 63] : '=';
        quad[4] = 0;
        h_app(o, quad);
    }
}

/* on success the src="data:..." value is already appended */
static BOOL h_img_data_uri(HtmlOut *o, const wchar_t *url, int len)
{
    const char *mime = img_mime(url, len);
    if (!mime) return FALSE;
    wchar_t path[MAX_PATH];
    if (!local_img_path(url, len, path, MAX_PATH)) return FALSE;
    char *bytes = NULL;
    int blen = 0;
    if (!ReadAllBytes(path, &bytes, &blen) || !bytes || blen <= 0) {
        free(bytes);
        return FALSE;
    }
    if (blen > 16 * 1024 * 1024) {    /* keep the html usable */
        free(bytes);
        return FALSE;
    }
    char head[40];
    wsprintfA(head, "data:%s;base64,", mime);
    h_app(o, head);
    h_app_b64(o, bytes, blen);
    free(bytes);
    return TRUE;
}

static void h_inline(HtmlOut *o, const wchar_t *s, int len, int depth)
{
    for (int i = 0; i < len; i++) {
        wchar_t c = s[i];
        if (c == L'\\' && i + 1 < len && is_escapable(s[i + 1])) {
            h_appw(o, &s[i + 1], 1);   /* emit the escaped char verbatim */
            i++;
            continue;
        }
        if (c == L'<') {
            int m = match_autolink(s + i, len - i);
            if (m > 0) {
                h_app(o, "<a href=\"");
                h_appurl(o, s + i + 1, m - 2);
                h_app(o, "\">");
                h_appw(o, s + i + 1, m - 2);
                h_app(o, "</a>");
                i += m - 1;
                continue;
            }
        }
        if (depth < 4 && c == L'=' && i + 1 < len && s[i+1] == L'=') {
            int m = i + 2;
            while (m + 1 < len && !(s[m] == L'=' && s[m+1] == L'=')) m++;
            if (m + 1 < len) {
                h_app(o, "<mark>");
                h_inline(o, s + i + 2, m - i - 2, depth + 1);
                h_app(o, "</mark>");
                i = m + 1;
                continue;
            }
        }
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
                    if (!(g_htmlEmbed
                          && h_img_data_uri(o, s + rb + 2, rp - rb - 2)))
                        h_appurl(o, s + rb + 2, rp - rb - 2);
                    h_app(o, "\" alt=\"");
                    h_appw(o, s + i + 2, rb - i - 2);
                    h_app(o, "\">");
                    i = rp;
                    continue;
                }
            }
        }
        if (c == L'[' && i + 1 < len && s[i+1] == L'[') {
            int j = i + 2;
            while (j + 1 < len && !(s[j] == L']' && s[j+1] == L']')) j++;
            if (j + 1 < len && j > i + 2) {
                const wchar_t *tgt = s + i + 2;
                int tl = j - i - 2;
                int txtOff = 0, txtLen = tl;
                for (int k = 0; k < tl; k++) {
                    if (tgt[k] == L'|') {
                        txtOff = k + 1;
                        txtLen = tl - k - 1;
                        tl = k;
                        break;
                    }
                }
                if (tl >= 1 && tl < 260 && txtLen >= 1) {
                    h_app(o, "<a href=\"");
                    h_appurl(o, tgt, tl);
                    h_app(o, ".md\">");
                    h_inline(o, tgt + txtOff, txtLen, depth + 1);
                    h_app(o, "</a>");
                    i = j + 1;
                    continue;
                }
            }
        }
        if (c == L'[' && i + 2 < len && s[i+1] == L'^') {
            int rb = i + 2;
            while (rb < len && s[rb] != L']') {
                if (s[rb] == L'[' || iswspace(s[rb])) break;
                rb++;
            }
            if (rb < len && s[rb] == L']' && rb - i - 2 >= 1
                && rb - i - 2 <= 16) {
                h_app(o, "<sup>[");
                h_appw(o, s + i + 2, rb - i - 2);
                h_app(o, "]</sup>");
                i = rb;
                continue;
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

/* close any open blockquote levels */
static void h_close_q(HtmlOut *o, int *qLv)
{
    while (*qLv > 0) { h_app(o, "</blockquote>\n"); (*qLv)--; }
}

/* close an open GitHub-alert callout div at block boundaries */
static void h_close_alert(HtmlOut *o, int *inA)
{
    if (*inA) { h_app(o, "</div>\n"); *inA = 0; }
}

/* delimiter row validity for html export (mirrors is_delim_row) */
static int tbl_delim_ok(const wchar_t *s, int len)
{
    int dashes = 0, bars = 0;
    for (int i = 0; i < len; i++) {
        wchar_t c = s[i];
        if (c == L'-') dashes++;
        else if (c == L'|') bars++;
        else if (c == L':' || c == L' ') continue;
        else return 0;
    }
    return dashes >= 1 && bars >= 1;
}

/* emit one table row as <th>/<td> cells; when delim is given the
 * per-column alignment is read from it */
static void tbl_emit_row(HtmlOut *o, const wchar_t *s, int len, int head,
                         const wchar_t *delim, int dlen)
{
    const wchar_t *cellStart[MD_MAX_COLS];
    int cellLen[MD_MAX_COLS];
    int n = 0;
    int i = 0;
    if (i < len && s[i] == L'|') i++;
    while (i < len && n < MD_MAX_COLS) {
        int j = i;
        while (j < len && s[j] != L'|') j++;
        int a = i, b = j;
        while (a < b && iswspace(s[a])) a++;
        while (b > a && iswspace(s[b - 1])) b--;
        cellStart[n] = s + a;
        cellLen[n] = b - a;
        n++;
        i = j + 1;
    }
    for (int c = 0; c < n; c++) {
        char align = 0;
        if (delim) {
            /* walk the delimiter row to cell c */
            int k = 0, ci = 0;
            if (ci < dlen && delim[ci] == L'|') ci++;
            while (ci < dlen && k < MD_MAX_COLS) {
                int j = ci;
                while (j < dlen && delim[j] != L'|') j++;
                if (k == c) {
                    int first = -1, last = -1;
                    for (int t = ci; t < j; t++)
                        if (delim[t] == L':') {
                            if (first < 0) first = t;
                            last = t;
                        }
                    if (first == ci && last == j - 1 && j > ci) align = 1;
                    else if (last == j - 1 && j > ci) align = 2;
                    break;
                }
                k++;
                ci = j + 1;
            }
        }
        h_app(o, head ? "<th" : "<td");
        if (align == 1) h_app(o, " style=\"text-align:center\"");
        if (align == 2) h_app(o, " style=\"text-align:right\"");
        h_app(o, ">");
        h_inline(o, cellStart[c], cellLen[c], 0);
        h_app(o, head ? "</th>\n" : "</td>\n");
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
              ".alert{border-left:4px solid #0969da;background:#ddf4ff;"
              "margin:10px 0;padding:6px 14px;border-radius:4px}\n"
              ".alert b.tag{color:#0969da}\n"
              ".alert.t2{border-color:#1a7f37;background:#dafbe1}"
              ".alert.t2 b.tag{color:#1a7f37}\n"
              ".alert.t3{border-color:#8250df;background:#fbefff}"
              ".alert.t3 b.tag{color:#8250df}\n"
              ".alert.t4{border-color:#9a6700;background:#fff8c5}"
              ".alert.t4 b.tag{color:#9a6700}\n"
              ".alert.t5{border-color:#cf222e;background:#ffebe9}"
              ".alert.t5 b.tag{color:#cf222e}\n"
              "a{color:#007aff}\nimg{max-width:100%}\n"
              "mark{background:#fff4b5;padding:0 2px}\n"
              "p.hl{background:#fff4b5;border-radius:4px;"
              "padding:2px 10px;display:block}\n"
              "hr{border:none;border-top:1px solid #d2d2d7}\n"
              "</style>\n</head>\n<body>\n");

    int inCode = 0, inP = 0, inUl = 0, inOl = 0, qLv = 0;
    int inAlertH = 0;   /* inside a "> [!TYPE]" callout div */
    MDFootnote fns[64];
    int nfn = 0;
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
            h_close_q(&o, &qLv);
            h_close_alert(&o, &inAlertH);
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
            h_close_q(&o, &qLv);
            h_close_alert(&o, &inAlertH);
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

        if (s[0] == L'|' || wmemchr(s, L'|', sl) != NULL) {
            /* GFM table: header + delimiter row + body rows */
            int npos2 = pos + adv;
            const wchar_t *ns2 = src + npos2;
            int nlenRaw2 = 0;
            while (npos2 + nlenRaw2 < srcLen
                   && src[npos2 + nlenRaw2] != L'\n') nlenRaw2++;
            int nlen2 = nlenRaw2;
            while (nlen2 > 0 && ns2[nlen2 - 1] == L'\r') nlen2--;
            int nlead2 = 0;
            while (nlead2 < nlen2 && ns2[nlead2] == L' ') nlead2++;
            if (npos2 <= srcLen && nlen2 - nlead2 > 0
                && tbl_delim_ok(ns2 + nlead2, nlen2 - nlead2)) {
                if (inP) { h_app(&o, "</p>\n"); inP = 0; }
                if (inUl) { h_app(&o, "</ul>\n"); inUl = 0; }
                if (inOl) { h_app(&o, "</ol>\n"); inOl = 0; }
                h_close_q(&o, &qLv);
                h_close_alert(&o, &inAlertH);
                h_app(&o, "<table>\n<thead>\n<tr>\n");
                tbl_emit_row(&o, s, sl, 1, ns2 + nlead2, nlen2 - nlead2);
                h_app(&o, "</tr>\n</thead>\n<tbody>\n");
                pos = npos2 + nlenRaw2 + 1;   /* past the delimiter row */
                while (pos < srcLen) {
                    const wchar_t *rs2 = src + pos;
                    int rlen2 = 0;
                    while (pos + rlen2 < srcLen
                           && src[pos + rlen2] != L'\n') rlen2++;
                    int radv2 = rlen2 + 1;
                    while (rlen2 > 0 && rs2[rlen2 - 1] == L'\r') rlen2--;
                    int rlead2 = 0;
                    while (rlead2 < rlen2 && rs2[rlead2] == L' ') rlead2++;
                    if (rlen2 - rlead2 < 1
                        || !wmemchr(rs2 + rlead2, L'|', rlen2 - rlead2))
                        break;
                    h_app(&o, "<tr>\n");
                    tbl_emit_row(&o, rs2 + rlead2, rlen2 - rlead2, 0,
                                 NULL, 0);
                    h_app(&o, "</tr>\n");
                    pos += radv2;
                }
                h_app(&o, "</tbody>\n</table>\n");
                if (pos > srcLen) break;
                continue;
            }
        }

        if (s[0] == L'#') {
            int lv = 0;
            while (lv < sl && lv < 6 && s[lv] == L'#') lv++;
            if (lv < sl && s[lv] == L' ') {
                if (inP) { h_app(&o, "</p>\n"); inP = 0; }
                if (inUl) { h_app(&o, "</ul>\n"); inUl = 0; }
                if (inOl) { h_app(&o, "</ol>\n"); inOl = 0; }
                h_close_q(&o, &qLv);
                h_close_alert(&o, &inAlertH);
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

        /* footnote definition line: collect, emit at the end */
        {
            const wchar_t *fl = NULL, *ft = NULL;
            int fll = 0, ftl = 0;
            if (is_footnote_def(s, sl, &fl, &fll, &ft, &ftl)
                && nfn < 64) {
                fns[nfn].label = fl; fns[nfn].labelLen = fll;
                fns[nfn].text = ft;  fns[nfn].textLen = ftl;
                nfn++;
                pos += adv;
                if (pos > srcLen) break;
                continue;
            }
        }

        if (s[0] == L'>') {
            int qlevel = 0;
            while (qlevel < 3 && qlevel < sl && s[qlevel] == L'>') qlevel++;
            const wchar_t *qs = s + qlevel;
            int ql = sl - qlevel;
            if (ql > 0 && qs[0] == L' ') { qs++; ql--; }
            if (inP) { h_app(&o, "</p>\n"); inP = 0; }
            if (inUl) { h_app(&o, "</ul>\n"); inUl = 0; }
            if (inOl) { h_app(&o, "</ol>\n"); inOl = 0; }
            /* GitHub alert card: "> [!TYPE]" line opens a colored div */
            if (qlevel == 1 && ql >= 6 && qs[0] == L'[' && qs[1] == L'!') {
                static const char *const atags[5] = {
                    "\xE6\xB3\xA8", "\xE6\x8F\x90\xE7\xA4\xBA",
                    "\xE9\x87\x8D\xE8\xA6\x81",
                    "\xE8\xAD\xA6\xE5\x91\x8A",
                    "\xE5\x8D\xB1\xE9\x99\xA9" };
                static const wchar_t *const anames2[5] = {
                    L"NOTE]", L"TIP]", L"IMPORTANT]", L"WARNING]",
                    L"CAUTION]" };
                for (int a = 0; a < 5; a++) {
                    int al = 0;
                    while (al < 11 && anames2[a][al]
                           && qs[2 + al] == anames2[a][al]) al++;
                    if (anames2[a][al] == 0) {
                        qs += 2 + al; ql -= 2 + al;
                        if (ql > 0 && qs[0] == L' ') { qs++; ql--; }
                        if (a > 0) {
                            char cls[16];
                            wsprintfA(cls, "%d", a);
                            h_app(&o, "<div class=\"alert t");
                            h_app(&o, cls);
                        } else {
                            h_app(&o, "<div class=\"alert");
                        }
                        h_app(&o, "\"><p><b class=\"tag\">\xE2\x9A\xA0 ");
                        h_app(&o, atags[a]);
                        h_app(&o, "</b> ");
                        if (ql > 0) h_inline(&o, qs, ql, 0);
                        h_app(&o, "</p>\n");
                        inAlertH = 1;
                        goto alert_done;
                    }
                }
            }
            if (inAlertH && qlevel == 1 && qLv == 0) {
                /* body line of an open callout card */
                h_app(&o, "<p>");
                h_inline(&o, qs, ql, 0);
                h_app(&o, "</p>\n");
            alert_done: ;
                pos += adv;
                if (pos > srcLen) break;
                continue;
            }
            if (inAlertH) { h_app(&o, "</div>\n"); inAlertH = 0; }
            while (qLv < qlevel) { h_app(&o, "<blockquote>\n"); qLv++; }
            while (qLv > qlevel) { h_app(&o, "</blockquote>\n"); qLv--; }
            h_app(&o, "<p>");
            h_inline(&o, qs, ql, 0);
            h_app(&o, "</p>\n");
            pos += adv;
            if (pos > srcLen) break;
            continue;
        }
        if (inAlertH) { h_app(&o, "</div>\n"); inAlertH = 0; }

        if ((s[0] == L'-' || s[0] == L'*' || s[0] == L'+')
            && sl > 1 && s[1] == L' ') {
            const wchar_t *item = s + 2;
            int il = sl - 2;
            int task = 0;
            if (il >= 3 && item[0] == L'[' && item[2] == L']'
                && (item[1] == L' ' || item[1] == L'x'
                    || item[1] == L'X')) {
                task = (item[1] == L' ') ? 1 : 2;
                item += 3;
                il -= 3;
                if (il > 0 && item[0] == L' ') { item++; il--; }
            }
            if (inP) { h_app(&o, "</p>\n"); inP = 0; }
            if (inOl) { h_app(&o, "</ol>\n"); inOl = 0; }
            h_close_q(&o, &qLv);
            h_close_alert(&o, &inAlertH);
            if (!inUl) {
                h_app(&o, task ? "<ul class=\"task\">\n" : "<ul>\n");
                inUl = 1;
            }
            h_app(&o, "<li>");
            if (task)
                h_app(&o, task == 2
                           ? "<input type=\"checkbox\" disabled checked>"
                           : "<input type=\"checkbox\" disabled>");
            h_inline(&o, item, il, 0);
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
                h_close_q(&o, &qLv);
                h_close_alert(&o, &inAlertH);
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
        h_close_q(&o, &qLv);
        h_close_alert(&o, &inAlertH);
        /* "==full line==" renders as a highlighted bar */
        if (sl >= 5 && s[0] == L'=' && s[1] == L'='
            && s[sl-1] == L'=' && s[sl-2] == L'=') {
            if (inP) { h_app(&o, "</p>\n"); inP = 0; }
            h_app(&o, "<p class=\"hl\">");
            h_inline(&o, s + 2, sl - 4, 0);
            h_app(&o, "</p>\n");
            pos += adv;
            if (pos > srcLen) break;
            continue;
        }
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
    h_close_q(&o, &qLv);
    h_close_alert(&o, &inAlertH);
    if (nfn > 0) {
        h_app(&o, "<hr>\n<section class=\"footnotes\">\n<ol>\n");
        for (int i = 0; i < nfn; i++) {
            h_app(&o, "<li id=\"fn-");
            h_appurl(&o, fns[i].label, fns[i].labelLen);
            h_app(&o, "\">");
            h_inline(&o, fns[i].text, fns[i].textLen, 0);
            h_app(&o, "</li>\n");
        }
        h_app(&o, "</ol>\n</section>\n");
    }
    h_app(&o, "</body>\n</html>\n");

    if (!o.buf) return 0;
    *out = o.buf;
    return o.len;
}

/* standalone share export: like md_to_html, but local images referenced
 * relative to docPath (the document's directory) are inlined as base64
 * data URIs so the single .html file renders anywhere. */
int md_to_html_standalone(const wchar_t *src, int srcLen,
                          const wchar_t *docPath, char **out)
{
    g_htmlImgDir[0] = 0;
    if (docPath && docPath[0])
        lstrcpynW(g_htmlImgDir, docPath, MAX_PATH);
    g_htmlEmbed = TRUE;
    int r = md_to_html(src, srcLen, out);
    g_htmlEmbed = FALSE;
    g_htmlImgDir[0] = 0;
    return r;
}
