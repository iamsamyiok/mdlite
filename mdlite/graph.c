/* MDLite - workspace graph view in 3D space:
 * nodes = note file names, edges = wiki-links, nothing else.
 * Orbit with mouse drag, zoom with the wheel, double-click opens. */
#include <stdio.h>
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define _ISOC99_SOURCE
#include "graph.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <math.h>
#include "mdlite.h"

#define GRAPH_FOCAL    620.0f   /* perspective focal length */
#define GRAPH_NEAR     120.0f   /* min view depth           */
#define GRAPH_RADIUS   13.0f    /* node radius at scale 1.0 */

/* ------------------------------------------------------------------ */
/* data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t name[80];
    wchar_t path[MAX_PATH];
    float   x, y, z;
    float   vx, vy, vz;
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

static float g_yaw   = 0.55f;     /* orbit around the Y axis  */
static float g_pitch = 0.32f;     /* orbit around the X axis  */
static float g_dist  = 1500.0f;   /* camera distance          */
static float g_tgtX, g_tgtY, g_tgtZ;   /* look-at target      */
static int   g_hover = -1;
static int   g_dragging = -1;
static POINT g_dragOrigin;
static int   g_selected = -1;     /* focus node               */
static BOOL  g_orbiting = FALSE;  /* empty-space drag = orbit */
static POINT g_orbitOrigin;

/* adjacency lists for focus dimming */
static int **g_adj    = NULL;
static int  *g_adjN   = NULL;
static int  *g_adjCap = NULL;

static void FreeAdj(void)
{
    if (!g_adj) return;
    for (int i = 0; i < g_nN; i++) free(g_adj[i]);
    free(g_adj); free(g_adjN); free(g_adjCap);
    g_adj = NULL; g_adjN = NULL; g_adjCap = NULL;
}

static void AdjAppend(int a, int b)
{
    for (int k = 0; k < g_adjN[a]; k++)
        if (g_adj[a][k] == b) return;
    if (g_adjN[a] == g_adjCap[a])
        g_adjCap[a] *= 2, g_adj[a] = (int *)realloc(g_adj[a], g_adjCap[a] * sizeof(int));
    g_adj[a][g_adjN[a]++] = b;
}

static void RebuildAdj(void)
{
    FreeAdj();
    if (g_nN <= 0) return;
    g_adj    = (int **)calloc(g_nN, sizeof(int *));
    g_adjN   = (int *)calloc(g_nN, sizeof(int));
    g_adjCap = (int *)calloc(g_nN, sizeof(int));
    for (int i = 0; i < g_nN; i++) {
        g_adjCap[i] = 4;
        g_adj[i] = (int *)malloc(g_adjCap[i] * sizeof(int));
    }
    for (int e = 0; e < g_nE; e++) {
        int a = g_edges[e].from, b = g_edges[e].to;
        if (a == b) continue;
        AdjAppend(a, b);
        AdjAppend(b, a);
    }
}

static BOOL IsNeighbor(int a, int b)
{
    if (a < 0 || b < 0 || a >= g_nN) return FALSE;
    for (int k = 0; k < g_adjN[a]; k++)
        if (g_adj[a][k] == b) return TRUE;
    return FALSE;
}

/* kg-style focus: unselected, unrelated nodes fade out */
static BOOL Dimmed(int idx)
{
    return g_selected >= 0 && idx != g_selected && !IsNeighbor(g_selected, idx);
}

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
    g_nodes[idx].x = 0; g_nodes[idx].y = 0; g_nodes[idx].z = 0;
    g_nodes[idx].vx = 0; g_nodes[idx].vy = 0; g_nodes[idx].vz = 0;
    g_nodes[idx].inDeg = 0; g_nodes[idx].outDeg = 0;
    g_nodes[idx].orphan = FALSE;
    return idx;
}

static void AddEdge(int from, int to)
{
    /* undirected: A→B and B→A are the same visible link */
    for (int i = 0; i < g_nE; i++)
        if ((g_edges[i].from == from && g_edges[i].to == to)
            || (g_edges[i].from == to && g_edges[i].to == from)) return;
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
    FreeAdj();
    free(g_nodes);
    free(g_edges);
    g_nodes = NULL;
    g_edges = NULL;
    g_nN = g_nE = g_nCap = g_nECap = 0;
    g_activeFileIdx = -1;
    g_selected = -1;
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
/* 3D force-directed layout                                            */
/* ------------------------------------------------------------------ */

static void GraphLayout(void)
{
    if (g_nN < 2) {
        if (g_nN == 1) { g_nodes[0].x = 0; g_nodes[0].y = 0; g_nodes[0].z = 0; }
        return;
    }
    /* init on a fibonacci sphere for an even 3D spread */
    const float R = g_nN <= 8 ? 320.0f : (g_nN <= 24 ? 480.0f : 640.0f);
    const float golden = 3.14159265f * (3.0f - sqrtf(5.0f));
    for (int i = 0; i < g_nN; i++) {
        float yy = (g_nN == 1) ? 0.0f : 1.0f - 2.0f * (float)i / (float)(g_nN - 1);
        float rr = sqrtf(1.0f - yy * yy);
        float th = golden * (float)i;
        g_nodes[i].x = R * rr * cosf(th);
        g_nodes[i].y = R * yy;
        g_nodes[i].z = R * rr * sinf(th);
    }
    /* Fruchterman-Reingold in 3D: pairwise repulsion, link springs,
     * weak gravity toward origin. */
    const float C2 = 600.0f, L = 260.0f, S = 3.5f, MAXV = 11.0f;
    for (int iter = 0; iter < 35; iter++) {
        float *fx = (float *)calloc(g_nN, sizeof(float));
        float *fy = (float *)calloc(g_nN, sizeof(float));
        float *fz = (float *)calloc(g_nN, sizeof(float));
        if (!fx || !fy || !fz) { free(fx); free(fy); free(fz); break; }
        for (int i = 0; i < g_nN; i++) {
            for (int j = i + 1; j < g_nN; j++) {
                float dx = g_nodes[j].x - g_nodes[i].x;
                float dy = g_nodes[j].y - g_nodes[i].y;
                float dz = g_nodes[j].z - g_nodes[i].z;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < 1.0f) d2 = 1.0f;
                float d = sqrtf(d2);
                float f = C2 / d;
                float invD = 1.0f / d;
                f *= invD;   /* f = C2 / d^2 along the axis */
                fx[i] -= f * dx; fy[i] -= f * dy; fz[i] -= f * dz;
                fx[j] += f * dx; fy[j] += f * dy; fz[j] += f * dz;
            }
        }
        for (int e = 0; e < g_nE; e++) {
            int a = g_edges[e].from, b = g_edges[e].to;
            float dx = g_nodes[b].x - g_nodes[a].x;
            float dy = g_nodes[b].y - g_nodes[a].y;
            float dz = g_nodes[b].z - g_nodes[a].z;
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d < 1.0f) d = 1.0f;
            if (d <= L) continue;   /* springs only pull, never push */
            float f = (d - L) / L * S;
            float ux = dx / d, uy = dy / d, uz = dz / d;
            fx[a] += f * ux; fy[a] += f * uy; fz[a] += f * uz;
            fx[b] -= f * ux; fy[b] -= f * uy; fz[b] -= f * uz;
        }
        for (int i = 0; i < g_nN; i++) {
            fx[i] -= g_nodes[i].x * 0.01f;
            fy[i] -= g_nodes[i].y * 0.01f;
            fz[i] -= g_nodes[i].z * 0.01f;
        }
        for (int i = 0; i < g_nN; i++) {
            float len = sqrtf(fx[i]*fx[i] + fy[i]*fy[i] + fz[i]*fz[i]);
            if (len > MAXV) {
                fx[i] = fx[i]/len*MAXV;
                fy[i] = fy[i]/len*MAXV;
                fz[i] = fz[i]/len*MAXV;
            }
            g_nodes[i].x += fx[i];
            g_nodes[i].y += fy[i];
            g_nodes[i].z += fz[i];
            if (g_nodes[i].x < -1600.0f) g_nodes[i].x = -1600.0f;
            if (g_nodes[i].x >  1600.0f) g_nodes[i].x =  1600.0f;
            if (g_nodes[i].y < -1600.0f) g_nodes[i].y = -1600.0f;
            if (g_nodes[i].y >  1600.0f) g_nodes[i].y =  1600.0f;
            if (g_nodes[i].z < -1600.0f) g_nodes[i].z = -1600.0f;
            if (g_nodes[i].z >  1600.0f) g_nodes[i].z =  1600.0f;
        }
        free(fx);
        free(fy);
        free(fz);
    }
    if (g_activeFileIdx >= 0) {
        g_tgtX = g_nodes[g_activeFileIdx].x;
        g_tgtY = g_nodes[g_activeFileIdx].y;
        g_tgtZ = g_nodes[g_activeFileIdx].z;
    } else {
        g_tgtX = g_tgtY = g_tgtZ = 0.0f;
    }
    RebuildAdj();
}

/* ------------------------------------------------------------------ */
/* projection                                                          */
/* ------------------------------------------------------------------ */

/* rotate world -> view (yaw then pitch), perspective-project to client
 * coordinates. Returns FALSE when the point is behind the camera. */
static BOOL ProjectPoint(float x, float y, float z, const RECT *rc,
                         float *sx, float *sy, float *depth, float *scale)
{
    float cy = cosf(g_yaw),  syw = sinf(g_yaw);
    float cp = cosf(g_pitch), spw = sinf(g_pitch);
    x -= g_tgtX; y -= g_tgtY; z -= g_tgtZ;
    float x1 =  x * cy - z * syw;
    float z1 =  x * syw + z * cy;
    float y1 =  y * cp - z1 * spw;
    float z2 =  y * spw + z1 * cp;
    float d = z2 + g_dist;
    if (d < GRAPH_NEAR) return FALSE;
    float s = GRAPH_FOCAL / d * 2.4f;
    float w = (float)(rc->right - rc->left);
    float h = (float)(rc->bottom - rc->top);
    *sx = w / 2.0f + x1 * s;
    *sy = h / 2.0f - y1 * s;
    *depth = d;
    *scale = s;
    return TRUE;
}

static void MixColor(COLORREF base, COLORREF farc, float t, COLORREF *out)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    *out = RGB(GetRValue(base)   + (GetRValue(farc)   - GetRValue(base))   * t,
               GetGValue(base)   + (GetGValue(farc)   - GetGValue(base))   * t,
               GetBValue(base)   + (GetBValue(farc)   - GetBValue(base))   * t);
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

/* per-frame projected node cache (built by GraphDraw, reused by
 * GraphHitTest so hit-testing matches what is on screen exactly) */
static float *g_prX, *g_prY, *g_prD, *g_prS;
static BOOL  *g_prVis;
static int    g_prN = 0, g_prCap = 0;
static RECT   g_prRect;

static void ProjectAll(const RECT *rc)
{
    if (g_nN > g_prCap) {
        int cap = g_nN + 32;
        g_prX = (float *)realloc(g_prX, cap * sizeof(float));
        g_prY = (float *)realloc(g_prY, cap * sizeof(float));
        g_prD = (float *)realloc(g_prD, cap * sizeof(float));
        g_prS = (float *)realloc(g_prS, cap * sizeof(float));
        g_prVis = (BOOL *)realloc(g_prVis, cap * sizeof(BOOL));
        g_prCap = cap;
    }
    g_prN = g_nN;
    g_prRect = *rc;
    for (int i = 0; i < g_nN; i++)
        g_prVis[i] = ProjectPoint(g_nodes[i].x, g_nodes[i].y, g_nodes[i].z,
                                  rc, &g_prX[i], &g_prY[i],
                                  &g_prD[i], &g_prS[i]);
}

static void GraphDrawNode(HDC dc, int idx)
{
    if (!g_prVis[idx]) return;
    const GraphNode *n = &g_nodes[idx];
    float sx = g_prX[idx], sy = g_prY[idx];
    float t = (g_prD[idx] - 300.0f) / 2400.0f;   /* 0 = near, 1 = far */
    BOOL dim = Dimmed(idx);
    BOOL sel = idx == g_selected;

    COLORREF fill, ring, name;
    if (dim) {
        MixColor(RGB(0xEE,0xEE,0xF2), RGB(0xF8,0xF8,0xFA), t, &fill);
        MixColor(RGB(0xDE,0xDE,0xE4), RGB(0xF0,0xF0,0xF4), t, &ring);
        MixColor(RGB(0xC8,0xC8,0xCE), RGB(0xE4,0xE4,0xE8), t, &name);
    } else if (idx == g_activeFileIdx || sel) {
        MixColor(RGB(0x1F,0x6F,0xEB), RGB(0x9E,0xC8,0xFF), t * 0.7f, &fill);
        MixColor(RGB(0x00,0x53,0xB8), RGB(0x7D,0xB8,0xFC), t * 0.7f, &ring);
        name = RGB(0x10,0x30,0x60);
    } else if (idx == g_hover) {
        MixColor(RGB(0xEB,0xF2,0xFF), RGB(0xF4,0xF8,0xFF), t, &fill);
        MixColor(RGB(0x90,0xB0,0xFF), RGB(0xC0,0xD4,0xFF), t, &ring);
        MixColor(RGB(0x22,0x22,0x28), RGB(0x8A,0x8A,0x92), t, &name);
    } else {
        MixColor(n->orphan ? RGB(0xF4,0xF4,0xF6) : RGB(0xE9,0xEC,0xF2),
                 RGB(0xF8,0xF8,0xFA), t, &fill);
        MixColor(n->orphan ? RGB(0xD8,0xD8,0xDE) : RGB(0xB8,0xC0,0xCE),
                 RGB(0xEC,0xEC,0xF0), t, &ring);
        MixColor(RGB(0x33,0x38,0x40), RGB(0xAE,0xAE,0xB6), t, &name);
    }

    float rad = GRAPH_RADIUS * g_prS[idx];
    if (rad < 3.0f) rad = 3.0f;
    if (rad > 22.0f) rad = 22.0f;
    RECT ball = { (int)(sx - rad), (int)(sy - rad),
                  (int)(sx + rad), (int)(sy + rad) };
    HBRUSH br = CreateSolidBrush(fill);
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    HPEN pn = CreatePen(PS_SOLID, sel ? 2 : 1, ring);
    HPEN op = (HPEN)SelectObject(dc, pn);
    Ellipse(dc, ball.left, ball.top, ball.right, ball.bottom);
    SelectObject(dc, ob);
    DeleteObject(br);
    SelectObject(dc, op);
    DeleteObject(pn);

    /* selection halo */
    if (sel) {
        HPEN pn2 = CreatePen(PS_SOLID, 1, RGB(0x7D,0xD3,0xFC));
        HPEN op2 = (HPEN)SelectObject(dc, pn2);
        HBRUSH ob2 = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, ball.left - 4, ball.top - 4,
                ball.right + 4, ball.bottom + 4);
        SelectObject(dc, ob2);
        SelectObject(dc, op2);
        DeleteObject(pn2);
    }

    /* the note file name under the node */
    HFONT oldFont = (HFONT)SelectObject(dc, g_fontHeader);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, name);
    RECT lr = { (int)(sx - 110.0f), (int)(sy + rad + 2.0f),
                (int)(sx + 110.0f), (int)(sy + rad + 22.0f) };
    DrawTextW(dc, n->name, -1, &lr,
              DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

static void GraphDrawEdge(HDC dc, int ei)
{
    const GraphEdge *e = &g_edges[ei];
    int a = e->from, b = e->to;
    if (!g_prVis[a] || !g_prVis[b]) return;
    BOOL fade = Dimmed(a) || Dimmed(b);
    float t = (g_prD[a] + g_prD[b]) / 2.0f;
    t = (t - 300.0f) / 2400.0f;
    COLORREF c;
    if (fade) MixColor(RGB(0xE8,0xE8,0xEC), RGB(0xF4,0xF4,0xF6), t, &c);
    else      MixColor(RGB(0xB6,0xC2,0xD6), RGB(0xE2,0xE8,0xF0), t, &c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, (int)g_prX[a], (int)g_prY[a], NULL);
    LineTo(dc,   (int)g_prX[b], (int)g_prY[b]);
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
    ProjectAll(rc);

    /* edges first, far to near, then nodes far to near (painter) */
    int *order = (int *)malloc((g_nN + g_nE) * sizeof(int));
    if (!order) return;
    for (int i = 0; i < g_nE; i++) order[i] = i;
    for (int i = 0; i + 1 < g_nE; i++)           /* insertion sort, small n */
        for (int j = i + 1; j < g_nE; j++) {
            float dj = (g_prD[g_edges[order[j]].from]
                        + g_prD[g_edges[order[j]].to]) / 2.0f;
            float di = (g_prD[g_edges[order[i]].from]
                        + g_prD[g_edges[order[i]].to]) / 2.0f;
            if (dj > di) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        }
    for (int i = 0; i < g_nE; i++) GraphDrawEdge(dc, order[i]);

    for (int i = 0; i < g_nN; i++) order[i] = i;
    for (int i = 0; i + 1 < g_nN; i++)
        for (int j = i + 1; j < g_nN; j++)
            if (g_prD[order[j]] > g_prD[order[i]]) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
    for (int i = 0; i < g_nN; i++) GraphDrawNode(dc, order[i]);
    free(order);

    HFONT oldFont = (HFONT)SelectObject(dc, g_fontHeader);
    SetBkMode(dc, TRANSPARENT);
    RECT hint = { rc->left + SC(10), rc->bottom - SC(24),
                  rc->left + SC(430), rc->bottom - SC(8) };
    DrawTextW(dc, L"拖空白旋转 · 滚轮缩放 · 单击聚焦 · 双击打开 · ESC 返回",
              -1, &hint, DT_LEFT | DT_VCENTER | DT_NOCLIP);

    /* stats badge, top-right: notes / links / orphans */
    int orphans = 0;
    for (int i = 0; i < g_nN; i++)
        if (g_nodes[i].orphan) orphans++;
    wchar_t stats[80];
    wsprintfW(stats, L"%d 笔记 · %d 链接 · %d 孤儿", g_nN, g_nE, orphans);
    SIZE sz;
    GetTextExtentPoint32W(dc, stats, lstrlenW(stats), &sz);
    int bw = sz.cx + SC(20), bh = SC(24);
    int bx = rc->right - bw - SC(10), by = rc->top + SC(8);
    HBRUSH sbbr = CreateSolidBrush(RGB(0x22,0x28,0x33));
    HBRUSH frameBr = CreateSolidBrush(RGB(0x3A,0x44,0x52));
    RECT sb = { bx, by, bx + bw, by + bh };
    FillRect(dc, &sb, sbbr);
    FrameRect(dc, &sb, frameBr);
    DeleteObject(sbbr); DeleteObject(frameBr);
    SetTextColor(dc, RGB(0xE8,0xEC,0xF2));
    RECT st = { bx + SC(10), by, bx + bw - SC(10), by + bh };
    DrawTextW(dc, stats, -1, &st, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

/* ------------------------------------------------------------------ */
/* hit-test / interaction                                              */
/* ------------------------------------------------------------------ */

int GraphHitTest(int px, int py, const RECT *rc)
{
    if (g_nN == 0) return -1;
    if (g_prN != g_nN
        || memcmp(&g_prRect, rc, sizeof(RECT)) != 0)
        ProjectAll(rc);          /* stay valid when called before a paint */
    int best = -1;
    float bestD = 0.0f;
    for (int i = 0; i < g_nN; i++) {
        if (!g_prVis[i]) continue;
        float rad = GRAPH_RADIUS * g_prS[i] + 4.0f;
        if (rad < 8.0f) rad = 8.0f;
        float dx = (float)px - g_prX[i];
        float dy = (float)py - g_prY[i];
        if (dx * dx + dy * dy <= rad * rad
            && (best < 0 || g_prD[i] < bestD)) {
            best = i; bestD = g_prD[i];
        }
    }
    return best;
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
    /* move the node in the camera plane (view axes mapped back to world) */
    float s = (g_dragging >= 0 && g_dragging < g_prN && g_prS[g_dragging] > 0.001f)
              ? g_prS[g_dragging] : 1.0f;
    float dx = (float)(px - g_dragOrigin.x) / s;
    float dy = (float)(py - g_dragOrigin.y) / s;
    float cy = cosf(g_yaw), sy = sinf(g_yaw);
    g_nodes[g_dragging].x += dx * cy;
    g_nodes[g_dragging].z += -dx * sy;
    g_nodes[g_dragging].y += -dy;
    g_dragOrigin.x = px;
    g_dragOrigin.y = py;
}

void GraphDragEnd(void)
{
    g_dragging = -1;
}

void GraphZoomBy(int deltaUnits)
{
    g_dist *= 1.0f - deltaUnits * 0.10f;
    if (g_dist < 320.0f) g_dist = 320.0f;
    if (g_dist > 9000.0f) g_dist = 9000.0f;
}

/* zoom keeping the point under the cursor pinned in place */
void GraphZoomByAt(int deltaUnits, int px, int py, const RECT *rc)
{
    (void)px; (void)py; (void)rc;
    GraphZoomBy(deltaUnits);   /* 3D orbit zoom is always around the target */
}

/* empty-space drag = orbit the camera */
void GraphPanStart(int px, int py)
{
    g_orbiting = TRUE;
    g_orbitOrigin.x = px;
    g_orbitOrigin.y = py;
}

void GraphPanMove(int px, int py)
{
    if (!g_orbiting) return;
    g_yaw += (float)(px - g_orbitOrigin.x) * 0.008f;
    g_pitch += (float)(py - g_orbitOrigin.y) * 0.006f;
    if (g_pitch > 1.45f) g_pitch = 1.45f;
    if (g_pitch < -1.45f) g_pitch = -1.45f;
    g_orbitOrigin.x = px;
    g_orbitOrigin.y = py;
}

void GraphPanEnd(void)
{
    g_orbiting = FALSE;
}

/* kg-style focus selection */
void GraphSelect(int idx)
{
    if (idx < -1 || idx >= g_nN) idx = -1;
    g_selected = idx;
}

int GraphSelected(void)
{
    return g_selected;
}

void GraphSetHover(int idx)
{
    g_hover = (idx >= -1 && idx < g_nN) ? idx : -1;
}

void GraphResetView(void)
{
    g_yaw = 0.55f;
    g_pitch = 0.32f;
    g_dist = 1500.0f;
    if (g_activeFileIdx >= 0) {
        g_tgtX = g_nodes[g_activeFileIdx].x;
        g_tgtY = g_nodes[g_activeFileIdx].y;
        g_tgtZ = g_nodes[g_activeFileIdx].z;
    } else {
        g_tgtX = g_tgtY = g_tgtZ = 0.0f;
    }
}

float GraphGetZoom(void)
{
    return 1400.0f / g_dist;   /* 1.0 at the default distance */
}

void  GraphGetOffset(float *ox, float *oy)
{
    if (ox) *ox = g_tgtX;
    if (oy) *oy = g_tgtY;
}

BOOL GraphNodePath(int idx, wchar_t *out, int outCap)
{
    if (idx < 0 || idx >= g_nN || !out || outCap <= 0) return FALSE;
    lstrcpynW(out, g_nodes[idx].path, outCap);
    return TRUE;
}
