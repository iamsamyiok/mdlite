/* MDLite - workspace file tree sidebar */
#ifndef MDLITE_TREE_H
#define MDLITE_TREE_H

#include "mdlite.h"

typedef struct TreeNode TreeNode;    /* opaque to callers; graph.c uses it */

void TreeToggle(void);                 /* show / hide the sidebar */
BOOL TreeShown(void);
void TreeSetShown(BOOL on);            /* restore persisted state */
int  TreeWidth(void);                  /* px, 0 when hidden */
void TreeSync(const wchar_t *docPath); /* rebuild from the doc's folder */
void TreeDraw(HDC dc, const RECT *rcClient);
BOOL TreeClick(POINT pt);              /* TRUE when the click is consumed */
void TreeMouseMove(POINT pt);
void TreeWheel(int delta);
BOOL TreePtIn(POINT pt);
BOOL TreeContextMenu(LPARAM lp);       /* screen coords; TRUE = consumed */

/* call cb(path, ctx) for every leaf .md file in the workspace tree */
void TreeForEachFile(void (*cb)(const wchar_t *path, void *ctx), void *ctx);

/* pin the workspace to a fixed vault folder ("" = follow the current
 * document). When set, tree / graph / orphan scans all stay inside it. */
void TreeSetVault(const wchar_t *dir);
const wchar_t *TreeVaultDir(void);

#endif

/* ---- workspace link index (wiki-links) ---- */

/* rescan the workspace: collect .md files and all [[wiki targets]]
 * (fenced code blocks are skipped). Call before Backlinks/Orphans/
 * JumpResolve when content may have changed. */
void TreeScanLinks(void);

/* files that link to `path` (matched by base name, deduped).
 * Returns count; caller frees *out via free(). */
int  TreeBacklinks(const wchar_t *path, wchar_t (**out)[MAX_PATH]);

/* workspace .md files that nothing links to.
 * Returns count; caller frees *out via free(). */
int  TreeOrphans(wchar_t (**out)[MAX_PATH]);

/* resolve a [[target]] to a real file path: try relative to docPath
 * (with/without .md), then base-name match across the workspace.
 * Returns FALSE when no file matches. */
BOOL TreeJumpResolve(const wchar_t *docPath, const wchar_t *target,
                     wchar_t *out, int outCap);
