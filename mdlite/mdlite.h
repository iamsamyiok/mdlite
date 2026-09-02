/* MDLite - shared declarations across modules */
#ifndef MDLITE_H
#define MDLITE_H

#define _WIN32_WINNT 0x0601
#include <windows.h>

#define APP_NAME     L"MDLite"
#define UNTITLED     L"未命名"

/* cross-thread messages (bg task -> main window) */
#define WM_EDITCMD   (WM_USER + 1)
#define WM_AI_CHUNK  (WM_USER + 2)
#define WM_AI_DONE   (WM_USER + 3)
#define WM_AG_CHUNK  (WM_USER + 4)
#define WM_AG_DONE   (WM_USER + 5)
#define WM_TRAYICON  (WM_USER + 6)

/* commands posted/sent to WndProc via WM_EDITCMD */
#define IDM_OPEN     1001
#define IDM_SAVE     1002
#define IDM_SAVEAS   1003
#define IDM_NEW      1004
#define IDM_TOGGLE   1005
#define IDM_FIND     1006
#define IDM_FINDNEXT 1007
#define IDM_FINDPREV 1008
#define IDM_REPLACE  1021
#define IDM_ZOOM     1009
#define IDM_SETTINGS 1010
#define IDM_TRAY_SHOW  1011
#define IDM_TRAY_QUIT  1012
#define IDM_OUTLINE    1013
#define IDM_COPYHTML   1014
#define IDM_EXPORTHTML 1015
#define IDM_HELP       1016
#define IDM_TOPMOST    1017
#define IDM_PRINT      1018
#define IDM_EXPORTTXT  1019
#define IDM_AI_MENU    1020
#define IDM_INSERT_AT  1022
#define IDM_SHAREHTML  1023
#define IDM_TREEBAR    1024
#define IDM_LINKS      1025
#define IDM_MRU_BASE   2000   /* + index, up to 2009 */

/* view states */
#define VIEW_EDIT    0
#define VIEW_SPLIT   1
#define VIEW_PREVIEW 2
#define VIEW_GRAPH   3

/* timers / hotkey ids */
#define TIMER_AUTO   2
#define TIMER_KICK   3
#define TIMER_AICHUNK 4
#define TIMER_UITICK 5
#define HOTKEY_SHOW  1

/* ---- shared UI globals (defined in main.c) ---- */
extern HWND   g_hwnd;
extern HWND   g_edit;
extern WNDPROC g_editProc;     /* original edit subclass proc */
extern int    g_dpi;
extern wchar_t g_path[MAX_PATH];
extern wchar_t g_name[MAX_PATH];
extern HFONT  g_fontHeader;
extern int    text_w(HDC hdc, HFONT font, const wchar_t *s, int len);
extern int    HeaderH(void);   /* main.c: header strip height (px) */
extern int    StatusH(void);   /* main.c: status bar height (px) */
extern BOOL   FindBarActive(void); /* main.c: find bar visible */
BOOL LoadFile(const wchar_t *path);      /* main.c */
BOOL ConfirmDiscard(void);               /* main.c */

static inline int SC(int px) { return MulDiv(px, g_dpi, 96); }

extern char g_archivedTree[41];   /* tree already recorded (skip re-archiving) */
extern int  g_histMax;            /* history entries kept per file (main.c) */
void  PinLoad(const wchar_t *path); /* refresh pin set for this document */
BOOL  PinHas(const char *sha);      /* is commit hex pinned for g_path? */   /* tree already recorded (skip re-archiving) */

/* ---- ai.c : provider config (owned by ai.c, edited by settings dlg) ---- */
extern wchar_t g_aiBase[256];
extern wchar_t g_aiModel[128];
extern wchar_t g_aiKey[256];
extern wchar_t g_aiSys[1024];
extern int     g_aiTimeout;
extern int     g_agTimeout;    /* agent kill timeout, seconds */
extern int     g_aiRounds;     /* prior Q/A turns sent as context */
extern wchar_t g_agPath[512];  /* opencode.exe full path; "" = PATH */
extern wchar_t g_agSys[1024];  /* agent prompt prepended to question */
extern volatile BOOL g_aiBusy;
extern volatile BOOL g_agBusy;
extern DWORD  g_aiStartTick, g_agStartTick;
extern int    g_stBusySec;      /* status-bar busy-seconds cache (main.c) */
extern BOOL   g_indentRet;      /* Enter inherits leading blanks (main.c) */

/* ---- ai.c : entry points ---- */
void  StartAi(const wchar_t *question);
void  ShowSelAiMenu(void);
void  StartAgent(const wchar_t *task);
void  ShowInsertMenu(void);   /* main.c: @ snippet popup */
void  AiCancel(void);
void  AgCancel(void);
void  AiFlushPending(void);
void  AgFlushPending(void);
/* main-thread message handlers (invoked from WndProc; consume the ptr) */
void  AiOnChunk(const wchar_t *txt, int n);
void  AiOnDone(const wchar_t *err);
void  AgOnChunk(const wchar_t *txt, int n);
void  AgOnDone(const wchar_t *err);
LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

/* ---- gitlite.c : byte io + snapshot repository ---- */
BOOL  WriteAllBytes(const wchar_t *path, const char *buf, int len);
BOOL  ReadAllBytes(const wchar_t *path, char **buf, int *len);
void  PathHash(const wchar_t *path, wchar_t *out);
BOOL  ReadLoose(const wchar_t *repo, const char *hex,
                char **body, int *blen);
BOOL  RepoRefPath(const wchar_t *repo, wchar_t *path, int cch);
/* legacy per-document layout (kept for one-time migration) */
BOOL  BuildHistoryDir(wchar_t *dir, int cch);
BOOL  BuildRepoDir(wchar_t *repo, int cch);
BOOL  ReadMaster(const wchar_t *repo, char hex[41]);
BOOL  WriteMaster(const wchar_t *repo, const char *hex);
BOOL  CloneCommit(const wchar_t *repo, const char *origHex,
                  const char *newParent, char outHex[41]);
/* parse "<prefix><value>" line from a commit body -> ptr + len */
const char *CommitLine(const char *body, int blen,
                       const char *prefix, int plen, int *vlen);
/* full archive write: hash objects, update ref, honor g_histMax */
void  GitArchive(const char *u8, int u8len, BOOL isAuto);

/* ---- export.c ---- */
char *BuildHtml(int *outLen);          /* malloc'd UTF-8 html */
void  CopyHtml(void);
void  ExportHtml(void);
void  ExportShareHtml(void);           /* self-contained html, images inlined */
void  DoPlainExport(void);
void  DoPrint(void);

/* ---- graph.c ---- */
void GraphBuild(void);
void GraphDraw(HDC dc, const RECT *rc);
int  GraphHitTest(int px, int py, const RECT *rc);
BOOL GraphDragStart(int px, int py, const RECT *rc);
void GraphDragMove(int px, int py);
void GraphDragEnd(void);
void GraphZoomBy(int deltaUnits);
void GraphResetView(void);

#endif /* MDLITE_H */
