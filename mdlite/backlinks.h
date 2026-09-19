/* MDLite - materialized wiki backlinks (双向链接)
 *
 * BacklinksSyncDir() rebuilds the auto-managed backlink block at the end
 * of every .md note under `dir` so it lists exactly the notes whose typed
 * content links to it:
 *
 *     <!-- mdlite:backlinks -->
 *     - [[A]]
 *
 * Call it after a note is saved. */
#ifndef BACKLINKS_H
#define BACKLINKS_H

void BacklinksSyncDir(const wchar_t *dir);

#endif
