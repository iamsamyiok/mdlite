/* MDLite - minimal markdown viewer/renderer (GDI based, low memory) */
#ifndef MDLITE_MARKDOWN_H
#define MDLITE_MARKDOWN_H

#include <windows.h>

/* inline run style flags */
enum {
    RF_BOLD   = 1,
    RF_ITALIC = 2,
    RF_CODE   = 4,   /* inline code, monospace */
    RF_STRIKE = 8,
    RF_LINK   = 16,  /* link text, colored + underlined */
    RF_IMAGE  = 32   /* image alt text, italic gray */
};

/* block line types */
enum {
    LT_BLANK = 0,
    LT_TEXT,          /* normal paragraph line */
    LT_H1, LT_H2, LT_H3, LT_H4, LT_H5, LT_H6,
    LT_CODE,          /* line inside fenced code block */
    LT_CODEPAD,       /* thin padding line at code block start/end */
    LT_QUOTE,
    LT_ITEM,          /* unordered list item */
    LT_OLITEM,        /* ordered list item */
    LT_HR
};

typedef struct MDRun {
    const wchar_t *ptr;  /* points into MDDoc.text (not NUL terminated) */
    int len;
    int flags;
    const wchar_t *url;  /* link target (points into text), NULL if none */
    int urlLen;
} MDRun;

/* one visual sub-line produced by word wrapping */
typedef struct MDSub {
    MDRun *runs;
    int nruns;
    int height;      /* pixel height of this sub-line */
} MDSub;

typedef struct MDLine {
    int type;
    int depth;       /* list nesting depth */
    int itemNum;     /* ordered list number */
    int code;        /* code block id (for background rect), 0 = none */
    int padTop;      /* extra spacing above text (headings) */
    MDSub *subs;
    int nsubs;
    int y;           /* absolute top offset in document */
    int height;      /* total pixel height (subs + spacing) */
} MDLine;

/* font set, rebuilt on dpi change */
typedef struct MDFonts {
    HFONT body, bold, ital, boldital;
    HFONT mono;
    HFONT h[6];
    int bodyH, monoH, hH[6];  /* character cell heights */
} MDFonts;

typedef struct MDDoc {
    wchar_t *text;          /* owned buffer, runs point into it */
    MDLine *lines;
    int nlines, caplines;
    int height;             /* total document height in px */
    int width;              /* width the doc was laid out for */
} MDDoc;

void md_init_fonts(MDFonts *f, HDC hdc, int dpi);
void md_free_fonts(MDFonts *f);

/* parse + layout. src is copied into doc->text. */
void md_build(MDDoc *doc, const wchar_t *src, int srcLen,
              const MDFonts *f, HDC hdc, int width);
void md_free(MDDoc *doc);

/* convert markdown source to a standalone UTF-8 HTML document.
 * returns byte length; *out is malloc'd, caller frees. 0 on error. */
int md_to_html(const wchar_t *src, int srcLen, char **out);

/* paint visible portion; rc is the target client area */
void md_paint(const MDDoc *doc, HDC hdc, const RECT *rc, int scrollY,
              const MDFonts *f);

/* link hit-rect sink: when set, md_paint reports every visible link's
 * client rect + target. Used for clickable links in preview mode. */
typedef void (*MdLinkSink)(void *ctx, RECT rc, const wchar_t *url,
                           int urlLen);
void md_set_link_sink(MdLinkSink cb, void *ctx);

/* theme colors (set by main.c, kept here as externs for simplicity) */
extern COLORREF g_colText, g_colHeading, g_colQuote, g_colCodeBg,
                g_colCodeInlineBg, g_colLink, g_colHr, g_colQuoteBar,
                g_colHeadingRule;

#endif
