/* MDLite - workspace graph view: nodes = .md files, edges = wiki-links */
#include <stdio.h>
#define _WIN32_WINNT 0x0601
#define _ISOC99_SOURCE
#include "graph.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <math.h>
#include "mdlite.h"

#define GRAPH_NODE_W  200.0f
#define GRAPH_NODE_H  28.0f
#define GRAPH_NODE_WD 200.0f
#define GRAPH_NODE_HT 28.0f

/* ------------------------------------------------------------------ */
/* data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t name[80];
    wchar_t path[MAX_PATH];
    float   x, y;
    int     inDeg, outDeg;
    BOOL    orphan;
} GraphNode;

typedef struct {
    int from;
    int to;
} GraphEdge;

typedef struct { wchar_t path[MAX_PATH]; } FileInfo;
typedef struct { FileInfo **fp; int *n; int *cap; } FCB;

static GraphNode *g_nodes;
static int g_nN, g_nCap;
static GraphEdge *g_edges;
static int g_nE, g_nECap;
static int  g_activeFileIdx = -1;

static float g_zoom = 1.0f;
static float g_offX, g_offY;
static int   g_hover = -1;
static int   g_dragging = -1;
static POINT g_dragOrigin;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int FindOrAddNode(const wchar_t *path, const wchar_t *name)
{
    for (int i = 0; i < g_nN; i++)
        if (lstrcmpiW(g_nodes[i].path, path) == 0) return i;
    if (g_nN == g_nCap) {
        g_nCap = g_nCap ? g_nCap * 2 : 16;
        g_nodes = (GraphNode *)realloc(g_nodes, g_nCap * sizeof(GraphNode));
    }
    int idx = g_nN++;
    lstrcpynW(g_nodes[idx].path, path, MAX_PATH);
    lstrcpynW(g_nodes[idx].name, name, 80);
    g_nodes[idx].x = 0; g_nodes[idx].y = 0;
    g_nodes[idx].inDeg = 0; g_nodes[idx].outDeg = 0;
    g_nodes[idx].orphan = FALSE;
    return idx;
}

static void AddEdge(int from, int to)
{
    for (int i = 0; i < g_nE; i++)
        if (g_edges[i].from == from && g_edges[i].to == to) return;
    if (g_nE == g_nECap) {
        g_nECap = g_nECap ? g_nECap * 2 : 16;
        g_edges = (GraphEdge *)realloc(g_edges, g_nECap * sizeof(GraphEdge));
    }
    g_edges[g_nE].from = from;
    g_edges[g_nE].to = to;
    g_nE++;
}

static const wchar_t *BaseNameOf(const wchar_t *path)
{
    const wchar_t *b = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') b = p + 1;
    return b;
}

static void LowerInPlace(wchar_t *s)
{
    for (; *s; s++) *s = (wchar_t)towlower(*s);
}

static void NormTarget(const wchar_t *src, wchar_t *dst, int dstCap)
{
    lstrcpynW(dst, src, dstCap);
    wchar_t *dot = wcsrchr(dst, L'.');
    if (dot && lstrcmpiW(dot, L".md") == 0) *dot = 0;
    LowerInPlace(dst);
}

/* ------------------------------------------------------------------ */
/* forward declarations                                                */
/* ------------------------------------------------------------------ */

static void GraphLayout(void);

/* ------------------------------------------------------------------ */
/* build                                                               */
/* ------------------------------------------------------------------ */

static void ScanFileForLinks(const wchar_t *path)
{
    char *u8 = NULL;
    int u8len = 0;
    if (!ReadAllBytes(path, &u8, &u8len) || !u8) return;
    int skip = 0;
    if (u8len >= 3 && (unsigned char)u8[0] == 0xEF
        && (unsigned char)u8[1] == 0xBB
        && (unsigned char)u8[2] == 0xBF)
        skip = 3;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, u8 + skip, u8len - skip, NULL, 0);
    if (wlen <= 0) { free(u8); return; }
    wchar_t *w = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
    if (!w) { free(u8); return; }
    MultiByteToWideChar(CP_UTF8, 0, u8 + skip, u8len - skip, w, wlen);
    w[wlen] = 0;
    free(u8);

    int inFence = 0;
    for (int i = 0; w[i]; i++) {
        if (w[i] == L'`' && (i == 0 || w[i-1] == L'\n')) {
            int j = i;
            while (w[j] == L'`') j++;
            if (j - i >= 3) { inFence = !inFence; i = j - 1; continue; }
        }
        if (inFence) continue;
        if (w[i] == L'[' && w[i+1] == L'[') {
            /* wiki-link [[target]] */
            int j = i + 2;
            while (w[j] && !(w[j] == L']' && w[j+1] == L']')) j++;
            if (w[j] == L']' && j > i + 2) {
                wchar_t save = w[j];
                w[j] = 0;   /* cut the target at "]]" for NormTarget */
                int si = -1;
                for (int k = 0; k < g_nN; k++)
                    if (lstrcmpiW(g_nodes[k].path, path) == 0) { si = k; break; }
                if (si < 0) { w[j] = save; i = j + 1; continue; }
                wchar_t tgtNorm[260];
                NormTarget(w + i + 2, tgtNorm, 260);
                w[j] = save;
                int di = -1;
                for (int k = 0; k < g_nN; k++) {
                    wchar_t nk[260];
                    NormTarget(g_nodes[k].name, nk, 260);
                    if (lstrcmpW(nk, tgtNorm) == 0) { di = k; break; }
                }
                if (di >= 0) {
                    AddEdge(si, di);
                    g_nodes[si].outDeg++;
                    g_nodes[di].inDeg++;
                }
                i = j + 1;
            }
        } else if (w[i] == L'[' && w[i+1] != L'[') {
            /* standard markdown link [text](target) */
            int j = i + 1;
            while (w[j] && w[j] != L']') j++;
            if (w[j] == L']' && w[j+1] == L'(') {
                int k = j + 2;
                while (w[k] && w[k] != L')') k++;
                if (w[k] == L')' && k > j + 1) {
                    wchar_t save = w[k];
                    w[k] = 0;   /* cut the target at ")" for NormTarget */
                    int si = -1;
                    for (int m = 0; m < g_nN; m++)
                        if (lstrcmpiW(g_nodes[m].path, path) == 0) { si = m; break; }
                    if (si >= 0) {
                        const wchar_t *tgt = w + j + 2;
                        if (tgt[0] == L'.' && (tgt[1] == L'/' || tgt[1] == L'\\'))
                            tgt += 2;
                        wchar_t tgtNorm[260];
                        NormTarget(tgt, tgtNorm, 260);
                        int di = -1;
                        for (int m = 0; m < g_nN; m++) {
                            wchar_t nm[260];
                            NormTarget(g_nodes[m].name, nm, 260);
                            if (lstrcmpW(nm, tgtNorm) == 0) { di = m; break; }
                        }
                        if (di >= 0 && di != si) {
                            AddEdge(si, di);
                            g_nodes[si].outDeg++;
                            g_nodes[di].inDeg++;
                        }
                    }
                    w[k] = save;
                }
            }
        }
    }
    free(w);
}

/* callback: append a collected file path to the FCB buffer */
static void FCBAdd(const wchar_t *p, void *ctx)
{
    FCB *fb = (FCB *)ctx;
    if (*fb->n == *fb->cap) {
        *fb->cap = *fb->cap ? *fb->cap * 2 : 32;
        *fb->fp = (FileInfo *)realloc(*fb->fp, *fb->cap * sizeof(FileInfo));
    }
    lstrcpynW((*fb->fp)[(*fb->n)++].path, p, MAX_PATH);
}

void GraphBuild(void)
{
    free(g_nodes);
    free(g_edges);
    g_nodes = NULL;
    g_edges = NULL;
    g_nN = g_nE = g_nCap = g_nECap = 0;
    g_activeFileIdx = -1;
    if (!g_path[0]) return;

    FileInfo *files = NULL;
    int fileN = 0, fileCap = 0;

    /* collect all .md file paths from the workspace tree */
    FCB fcb;
    fcb.fp = &files; fcb.n = &fileN; fcb.cap = &fileCap;
    TreeForEachFile(FCBAdd, &fcb);

    if (fileN == 0) { free(files); return; }

    for (int f = 0; f < fileN; f++) {
        const wchar_t *b = BaseNameOf(files[f].path);
        wchar_t base[80];
        lstrcpynW(base, b, 80);
        wchar_t *dot = wcsrchr(base, L'.');
        if (dot) *dot = 0;
        int idx = FindOrAddNode(files[f].path, base);
        if (lstrcmpiW(files[f].path, g_path) == 0)
            g_activeFileIdx = idx;
    }

    for (int f = 0; f < fileN; f++)
        ScanFileForLinks(files[f].path);

    for (int i = 0; i < g_nN; i++)
        if (g_nodes[i].inDeg == 0 && g_nodes[i].outDeg == 0)
            g_nodes[i].orphan = TRUE;

    free(files);
    GraphLayout();
}

/* ------------------------------------------------------------------ */
/* layout                                                              */
/* ------------------------------------------------------------------ */

static void GraphLayout(void)
{
    if (g_nN < 2) {
        if (g_nN == 1) { g_nodes[0].x = 0; g_nodes[0].y = 0; }
        return;
    }
    const float R = g_nN <= 8 ? 200.0f : (g_nN <= 24 ? 300.0f : 420.0f);
    for (int i = 0; i < g_nN; i++) {
        float a = (2.0f * (float)i * 3.14159265f) / (float)g_nN;
        g_nodes[i].x = R * cosf(a);
        g_nodes[i].y = R * sinf(a);
    }
    /* Fruchterman-Reingold: repulsion C2/d pushes nodes apart, springs
     * pull linked nodes toward rest length L. With C2=400 the force
     * balance sits near 1.4*L so the graph stays inside the viewport. */
    const float C2 = 400.0f, L = 200.0f, S = 3.5f, MAXV = 8.0f;
    for (int iter = 0; iter < 25; iter++) {
        float *fx = (float *)calloc(g_nN, sizeof(float));
        float *fy = (float *)calloc(g_nN, sizeof(float));
        if (!fx || !fy) { free(fx); free(fy); break; }
        for (int i = 0; i < g_nN; i++) {
            for (int j = i + 1; j < g_nN; j++) {
                float dx = g_nodes[j].x - g_nodes[i].x;
                float dy = g_nodes[j].y - g_nodes[i].y;
                float d2 = dx * dx + dy * dy;
                if (d2 < 1.0f) d2 = 1.0f;
                float d = sqrtf(d2);
                float f = C2 / d;
                float invD = 1.0f / d;
                float fxi = -f * dx * invD;
                float fyi = -f * dy * invD;
                fx[i] += fxi; fy[i] += fyi;
                fx[j] -= fxi; fy[j] -= fyi;
            }
        }
        for (int e = 0; e < g_nE; e++) {
            int a = g_edges[e].from, b = g_edges[e].to;
            float dx = g_nodes[b].x - g_nodes[a].x;
            float dy = g_nodes[b].y - g_nodes[a].y;
            float d = sqrtf(dx * dx + dy * dy);
            if (d < 1.0f) d = 1.0f;
            if (d <= L) continue;   /* springs only pull, never push */
            float f = (d - L) / L * S;
            float ux = dx / d, uy = dy / d;
            fx[a] += f * ux; fy[a] += f * uy;
            fx[b] -= f * ux; fy[b] -= f * uy;
        }
        for (int i = 0; i < g_nN; i++) {
            fx[i] -= g_nodes[i].x * 0.01f;
            fy[i] -= g_nodes[i].y * 0.01f;
        }
        for (int i = 0; i < g_nN; i++) {
            float len = sqrtf(fx[i]*fx[i] + fy[i]*fy[i]);
            if (len > MAXV) { fx[i] = fx[i]/len*MAXV; fy[i] = fy[i]/len*MAXV; }
            g_nodes[i].x += fx[i];
            g_nodes[i].y += fy[i];
            if (g_nodes[i].x < -1500.0f) g_nodes[i].x = -1500.0f;
            if (g_nodes[i].x >  1500.0f) g_nodes[i].x =  1500.0f;
            if (g_nodes[i].y < -1500.0f) g_nodes[i].y = -1500.0f;
            if (g_nodes[i].y >  1500.0f) g_nodes[i].y =  1500.0f;
        }
        free(fx);
        free(fy);
    }
    if (g_activeFileIdx >= 0) {
        g_offX = -g_nodes[g_activeFileIdx].x;
        g_offY = -g_nodes[g_activeFileIdx].y;
    }
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static void GraphDrawNode(HDC dc, int idx)
{
    const GraphNode *n = &g_nodes[idx];
    float nx = n->x - GRAPH_NODE_WD / 2.0f;
    float ny = n->y - GRAPH_NODE_HT / 2.0f;
    float nw = GRAPH_NODE_WD, nh = GRAPH_NODE_HT;
    HRGN rgn = CreateRoundRectRgn(
        (int)floorf(nx), (int)floorf(ny),
        (int)ceilf(nx + nw), (int)ceilf(ny + nh),
        8, 8);
    HBRUSH br = CreateSolidBrush(
        n->orphan ? RGB(0xFA,0xFA,0xFC)
                  : (idx == g_hover || idx == g_activeFileIdx
                     ? RGB(0xEB,0xF2,0xFF)
                     : RGB(0xF7,0xF7,0xF9)));
    FillRgn(dc, rgn, br);
    DeleteObject(br);
    COLORREF bc = idx == g_activeFileIdx ? RGB(0x00,0x7A,0xFF)
           : idx == g_hover            ? RGB(0x90,0xB0,0xFF)
                                         : RGB(0xCC,0xCC,0xD0);
    HPEN pen = CreatePen(PS_SOLID, 1, bc);
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, (int)floorf(nx), (int)floorf(ny),
              (int)ceilf(nx + nw), (int)ceilf(ny + nh));
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    DeleteObject(rgn);

    HFONT oldFont = (HFONT)SelectObject(dc, g_fontHeader);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x22,0x22,0x28));
    RECT lr;
    lr.left   = (int)floorf(nx + 8.0f);
    lr.top    = (int)floorf(ny);
    lr.right  = (int)ceilf(nx + nw - 8.0f);
    lr.bottom = (int)ceilf(ny + nh);
    DrawTextW(dc, n->name, -1, &lr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);

    /* draw connection count badge */
    int totalDeg = n->inDeg + n->outDeg;
    if (totalDeg > 0) {
        wchar_t badge[16];
        wsprintfW(badge, L"%d", totalDeg);
        int tw = (int)lstrlenW(badge);
        int bw = tw * 7 + 8;
        int bh = 16;
        int bx = (int)ceilf(nx + nw) - bw - 4;
        int by = (int)floorf(ny) - bh - 2;
        if (by < (int)ny) by = (int)ny;
        HBRUSH bbr = CreateSolidBrush(idx == g_activeFileIdx
                                      ? RGB(0x00,0x7A,0xFF)
                                      : RGB(0x6E,0x6E,0x73));
        FillRect(dc, &(RECT){bx, by, bx+bw, by+bh}, bbr);
        DeleteObject(bbr);
        HFONT of2 = (HFONT)SelectObject(dc, g_fontHeader);
        SetTextColor(dc, RGB(255,255,255));
        RECT tr = {bx + 4, by + 2, bx + bw - 4, by + bh - 2};
        DrawTextW(dc, badge, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of2);
    }
}

static void GraphDrawEdge(HDC dc, int ei)
{
    const GraphEdge *e = &g_edges[ei];
    const GraphNode *a = &g_nodes[e->from], *b = &g_nodes[e->to];
    float dx = b->x - a->x, dy = b->y - a->y;
    float d = sqrtf(dx*dx + dy*dy);
    if (d < 1.0f) return;
    float ux = dx/d, uy = dy/d;
    float hw = GRAPH_NODE_WD/2.0f, hh = GRAPH_NODE_HT/2.0f;
    float t1 = 1.0f;
    if (fabsf(ux) > 0.001f) t1 = fminf(t1, hw/fabsf(ux));
    if (fabsf(uy)  > 0.001f) t1 = fminf(t1, hh/fabsf(uy));
    float x1 = a->x + ux * t1, y1 = a->y + uy * t1;
    float t2 = 1.0f;
    if (fabsf(-ux) > 0.001f) t2 = fminf(t2, hw/fabsf(-ux));
    if (fabsf(-uy) > 0.001f) t2 = fminf(t2, hh/fabsf(-uy));
    float x2 = b->x - ux * t2, y2 = b->y - uy * t2;

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xCC,0xCC,0xD0));
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, (int)floorf(x1), (int)floorf(y1), NULL);
    LineTo(dc,   (int)floorf(x2), (int)floorf(y2));
    DeleteObject(pen);
    SelectObject(dc, oldPen);
}

void GraphDraw(HDC dc, const RECT *rc)
{
    if (g_nN == 0) {
        HFONT oldFont = (HFONT)SelectObject(dc, g_fontHeader);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x99,0x99,0xA0));
        RECT r = { rc->left, rc->top, rc->right, rc->bottom };
        const wchar_t *msg = g_path[0]
            ? L"当前工作区暂无 wiki 链接。\n打开或新建 .md 文件后回车即可看到图谱。"
            : L"未打开文档。\n请先打开或新建一个 .md 文件。";
        DrawTextW(dc, msg, -1, &r, DT_CENTER | DT_VCENTER | DT_NOCLIP);
        SelectObject(dc, oldFont);
        return;
    }
    XFORM xf;
    xf.eM11 = g_zoom;   xf.eM12 = 0.0f;
    xf.eM21 = 0.0f;     xf.eM22 = g_zoom;
    xf.eDx  = (FLOAT)(rc->right  / 2.0 + g_offX);
    xf.eDy  = (FLOAT)(rc->bottom / 2.0 + g_offY);
    /* world transform only works in GM_ADVANCED; in the default
     * compatible mode SetWorldTransform fails silently and the nodes
     * end up drawn at raw world coordinates (mostly off-window) */
    int prevGM = SetGraphicsMode(dc, GM_ADVANCED);
    SetWorldTransform(dc, &xf);
    for (int i = 0; i < g_nE; i++) GraphDrawEdge(dc, i);
    for (int i = 0; i < g_nN; i++) GraphDrawNode(dc, i);
    XFORM xfId = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    SetWorldTransform(dc, &xfId);
    SetGraphicsMode(dc, prevGM);

    HFONT oldFont = (HFONT)SelectObject(dc, g_fontHeader);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xAA,0xAA,0xB0));
    wchar_t zn[32];
    wsprintfW(zn, L"%d%%", (int)(g_zoom * 100.0f));
    SIZE sz;
    GetTextExtentPoint32W(dc, zn, lstrlenW(zn), &sz);
    RECT zr = { rc->right - sz.cx - SC(10),
                rc->bottom - sz.cy - SC(8),
                rc->right - SC(10),
                rc->bottom - SC(8) };
    DrawTextW(dc, zn, -1, &zr, DT_RIGHT | DT_TOP | DT_NOCLIP);
    RECT hint = { rc->left + SC(10), rc->bottom - SC(24),
                  rc->left + SC(200), rc->bottom - SC(8) };
    DrawTextW(dc, L"滚轮缩放  ESC 退出", -1, &hint,
              DT_LEFT | DT_VCENTER | DT_NOCLIP);
    SelectObject(dc, oldFont);
}

/* ------------------------------------------------------------------ */
/* hit-test / interaction                                              */
/* ------------------------------------------------------------------ */

static void ScreenToModel(int sx, int sy, const RECT *rc, float *mx, float *my)
{
    *mx = (float)(sx - rc->right / 2 - g_offX) / g_zoom;
    *my = (float)(sy - rc->bottom / 2 - g_offY) / g_zoom;
}

static BOOL PtInNode(int idx, float mx, float my)
{
    const GraphNode *n = &g_nodes[idx];
    return mx >= n->x - GRAPH_NODE_WD/2.0f && mx <= n->x + GRAPH_NODE_WD/2.0f
        && my >= n->y - GRAPH_NODE_HT/2.0f && my <= n->y + GRAPH_NODE_HT/2.0f;
}

int GraphHitTest(int px, int py, const RECT *rc)
{
    if (g_nN == 0) return -1;
    float mx, my;
    ScreenToModel(px, py, rc, &mx, &my);
    for (int i = g_nN - 1; i >= 0; i--)
        if (PtInNode(i, mx, my)) return i;
    return -1;
}

BOOL GraphDragStart(int px, int py, const RECT *rc)
{
    int h = GraphHitTest(px, py, rc);
    if (h < 0) return FALSE;
    g_dragging = h;
    g_dragOrigin.x = px;
    g_dragOrigin.y = py;
    return TRUE;
}

void GraphDragMove(int px, int py)
{
    if (g_dragging < 0) return;
    int dx = px - g_dragOrigin.x;
    int dy = py - g_dragOrigin.y;
    g_nodes[g_dragging].x += (float)dx / g_zoom;
    g_nodes[g_dragging].y += (float)dy / g_zoom;
    g_dragOrigin.x = px;
    g_dragOrigin.y = py;
}

void GraphDragEnd(void)
{
    g_dragging = -1;
}

void GraphZoomBy(int deltaUnits)
{
    g_zoom *= 1.0f + deltaUnits * 0.10f;
    if (g_zoom < 0.2f) g_zoom = 0.2f;
    if (g_zoom > 5.0f) g_zoom = 5.0f;
}

void GraphResetView(void)
{
    g_zoom = 1.0f;
    g_offX = 0.0f;
    g_offY = 0.0f;
    if (g_activeFileIdx >= 0) {
        g_offX = -g_nodes[g_activeFileIdx].x;
        g_offY = -g_nodes[g_activeFileIdx].y;
    }
}

float GraphGetZoom(void) { return g_zoom; }
void  GraphGetOffset(float *ox, float *oy)
{
    if (ox) *ox = g_offX;
    if (oy) *oy = g_offY;
}

BOOL GraphNodePath(int idx, wchar_t *out, int outCap)
{
    if (idx < 0 || idx >= g_nN || !out || outCap <= 0) return FALSE;
    lstrcpynW(out, g_nodes[idx].path, outCap);
    return TRUE;
}
