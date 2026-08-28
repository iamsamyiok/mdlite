/* MDLite - editor pure logic shared by app and tests */
#ifndef MDLITE_EDITLOGIC_H
#define MDLITE_EDITLOGIC_H
#include <windows.h>

/* read one line (li, 0-based) into buf; returns length, NUL-terminated */
int EditGetLine(HWND h, int li, wchar_t *buf, int cap);

/* list-marker continuation: "\r\n" + next marker into cont.
 * returns cont length, 0 when no marker. *exitList=1 for empty items. */
int ListContinuation(const wchar_t *line, int len,
                     wchar_t *cont, int cap, int *exitList);

#endif
