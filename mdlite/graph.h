/* MDLite - workspace graph view */
#ifndef GRAPH_H
#define GRAPH_H

#include "tree.h"   /* for TreeNode traversal inside graph.c */

void GraphBuild(void);
void GraphDraw(HDC dc, const RECT *rc);
int  GraphHitTest(int px, int py, const RECT *rc);
BOOL GraphDragStart(int px, int py, const RECT *rc);
void GraphDragMove(int px, int py);
void GraphDragEnd(void);
void GraphZoomBy(int deltaUnits);
void GraphZoomByAt(int deltaUnits, int px, int py, const RECT *rc);
void GraphPanStart(int px, int py);
void GraphPanMove(int px, int py);
void GraphPanEnd(void);
void GraphSelect(int idx);
int  GraphSelected(void);
void GraphSetHover(int idx);
void GraphResetView(void);
float GraphGetZoom(void);
void  GraphGetOffset(float *ox, float *oy);
/* return the path of node `idx` (caller must provide out buffer);
 * returns FALSE if idx is out of range */
BOOL GraphNodePath(int idx, wchar_t *out, int outCap);

#endif
