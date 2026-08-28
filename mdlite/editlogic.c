/* MDLite - editor pure logic shared by app and tests */
#include "editlogic.h"

int EditGetLine(HWND h, int li, wchar_t *buf, int cap)
{
    if (cap < 2) return 0;
    *(LPWORD)buf = (WORD)(cap - 1);
    int n = (int)SendMessageW(h, EM_GETLINE, li, (LPARAM)buf);
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    buf[n] = 0;
    return n;
}

int ListContinuation(const wchar_t *line, int len,
                     wchar_t *cont, int cap, int *exitList)
{
    *exitList = 0;
    int pre = 0;
    while (pre < len && (line[pre] == L' ' || line[pre] == L'\t')) pre++;
    if (pre >= len) return 0;

    wchar_t marker[104];
    int mi = 0;
    for (int k = 0; k < pre && mi < 100; k++) marker[mi++] = line[k];

    wchar_t c = line[pre];
    int i = pre;
    if (c == L'-' || c == L'*' || c == L'+') {
        if (i + 1 >= len || line[i + 1] != L' ') return 0;
        marker[mi++] = c;
        marker[mi++] = L' ';
        i += 2;
    } else if (c >= L'0' && c <= L'9') {
        long v = 0;
        int d = 0;
        while (i < len && line[i] >= L'0' && line[i] <= L'9') {
            v = v * 10 + (line[i] - L'0');
            i++; d++;
        }
        if (d == 0 || d > 5 || v > 99999) return 0;
        if (i >= len || line[i] != L'.') return 0;
        i++;
        if (i >= len || line[i] != L' ') return 0;
        i++;
        v++;
        wchar_t nb[8];
        int ni = 0;
        if (v == 0) nb[ni++] = L'0';
        while (v > 0) { nb[ni++] = L'0' + (wchar_t)(v % 10); v /= 10; }
        while (ni > 0 && mi < 100) marker[mi++] = nb[--ni];
        if (mi + 2 > 100) return 0;
        marker[mi++] = L'.';
        marker[mi++] = L' ';
    } else {
        return 0;
    }

    /* task marker continues unchecked */
    if (i + 2 < len && line[i] == L'[' && line[i + 2] == L']'
        && (line[i + 1] == L' ' || line[i + 1] == L'x'
            || line[i + 1] == L'X')) {
        if (mi + 4 > 100) return 0;
        marker[mi++] = L'['; marker[mi++] = L' ';
        marker[mi++] = L']'; marker[mi++] = L' ';
        i += 3;
        if (i < len && line[i] == L' ') i++;
    }

    if (i >= len) {          /* empty item: Enter exits the list */
        *exitList = 1;
        return mi;
    }
    if (mi + 3 >= cap) return 0;
    cont[0] = L'\r'; cont[1] = L'\n';
    memcpy(cont + 2, marker, (size_t)mi * sizeof(wchar_t));
    cont[2 + mi] = 0;
    return mi + 2;
}
