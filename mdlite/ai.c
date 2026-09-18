/* MDLite - AI chat & agent background tasks */
#define _WIN32_WINNT 0x0601
#include "mdlite.h"
#include "editlogic.h"
#include <stdlib.h>
#include <string.h>
#include <winhttp.h>

#ifndef EM_REDO
#define EM_REDO 0x0454
#endif

/* AI provider config (OpenAI-compatible chat/completions) */
wchar_t g_aiBase[256]  = L"";
wchar_t g_aiModel[128] = L"";
wchar_t g_aiKey[256]   = L"";
wchar_t g_aiSys[1024]  = L"";
int     g_aiTimeout    = 10;
int     g_agTimeout    = 60;   /* agent kill timeout, seconds */
int     g_aiRounds     = 2;    /* prior Q/A turns sent as context */

/* rolling in-session AI context (question/answer pairs), newest last */
#define AI_CTX_MAX 10
static wchar_t *g_aiCtxQ[AI_CTX_MAX], *g_aiCtxA[AI_CTX_MAX];
static int      g_aiCtxCnt;
static wchar_t  g_aiLastQ[1024];      /* current turn being streamed */
static wchar_t *g_aiLastA;
static int      g_aiLastALen;

/* AI streaming state */
volatile BOOL  g_aiBusy;
static volatile LONG  g_aiCancelFlag;
static volatile HANDLE g_aiReq;       /* active WinHTTP request (cancel) */
/* chunk batching: coalesce SSE deltas so the edit control and the view
 * update at a smooth ~15fps instead of jumping on every tiny chunk */
static wchar_t *g_aiPending;   /* heap buffer of pending text */
static int      g_aiPendingLen, g_aiPendingCap;

/* Agent (opencode) config */
wchar_t g_agPath[512] = L"";   /* opencode.exe full path; "" = PATH */
wchar_t g_agSys[1024] = L"";   /* agent prompt prepended to question */

/* background task slots: 0 = AI chat, 1 = agent. each slot tracks where
 * to insert (anchored to the question line so concurrent user edits in
 * other places never displace or lose the streamed answer) */
#define N_TASKS 2
typedef struct {
    BOOL     active;
    wchar_t  anchor[2048]; /* question line text (no CRLF) */
    int      insEnd;       /* current insert offset (refreshed via anchor) */
    int      totalLen;     /* streamed chars so far (offset compensation) */
} BgTask;
static BgTask g_tasks[N_TASKS];

/* agent process state */
volatile BOOL  g_agBusy;
static volatile LONG  g_agCancel;
static volatile HANDLE g_agProc;      /* active agent process (cancel) */
static wchar_t *g_agPending;
static int      g_agPendingLen, g_agPendingCap;

/* task timing (status readout) */
DWORD  g_aiStartTick, g_agStartTick;

/* ------------------------------------------------------------------ */
/* AI slash command: OpenAI-compatible streaming chat via WinHTTP      */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t base[256], model[128], key[256], sys[1024], q[1024];
    int timeout;
} AiJob;

/* escape a wide string into a JSON string body (no quotes) */
static int JsonEscape(const wchar_t *s, wchar_t *out, int cap)
{
    int n = 0;
    for (; *s; s++) {
        if (n + 8 >= cap) break;
        switch (*s) {
        case L'"':  out[n++] = L'\\'; out[n++] = L'"';  break;
        case L'\\': out[n++] = L'\\'; out[n++] = L'\\'; break;
        case L'\n': out[n++] = L'\\'; out[n++] = L'n';  break;
        case L'\r': out[n++] = L'\\'; out[n++] = L'r';  break;
        case L'\t': out[n++] = L'\\'; out[n++] = L't';  break;
        default:
            if (*s < 0x20) {
                static const wchar_t HX[] = L"0123456789abcdef";
                out[n++] = L'\\'; out[n++] = L'u';
                out[n++] = HX[(*s >> 12) & 15];
                out[n++] = HX[(*s >> 8) & 15];
                out[n++] = HX[(*s >> 4) & 15];
                out[n++] = HX[*s & 15];
            } else {
                out[n++] = *s;
            }
        }
    }
    out[n] = 0;
    return n;
}

/* read a JSON string at p (p points at the opening quote);
 * returns chars consumed, fills out (NUL-terminated). 0 on error. */
static int JsonReadString(const char *p, const char *end, wchar_t *out,
                          int outMax)
{
    const char *start = p;
    if (p >= end || *p != '"') return 0;
    p++;
    int n = 0;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            out[n] = 0;
            return (int)(p - start) + 1;
        }
        if (c == '\\') {
            p++;
            if (p >= end) return 0;
            char e = *p;
            if (e == 'u') {
                if (p + 4 >= end) return 0;
                int v = 0;
                for (int k = 1; k <= 4; k++) {
                    char h = p[k];
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= h - '0';
                    else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
                    else return 0;
                }
                if (n < outMax - 1) out[n++] = (wchar_t)v;
                p += 5;
                continue;
            }
            wchar_t m = 0;
            switch (e) {
            case '"':  m = L'"';  break;
            case '\\': m = L'\\'; break;
            case '/':  m = L'/';  break;
            case 'b':  m = L'\b'; break;
            case 'f':  m = L'\f'; break;
            case 'n':  m = L'\n'; break;
            case 'r':  m = L'\r'; break;
            case 't':  m = L'\t'; break;
            default: return 0;
            }
            if (n < outMax - 1) out[n++] = m;
            p++;
            continue;
        }
        /* raw UTF-8 byte -> decode */
        int need = 1;
        if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        if (c < 0x80) {
            if (n < outMax - 1) out[n++] = (wchar_t)c;
            p++;
        } else {
            if (p + need > end) return 0;
            DWORD cp = 0;
            if (need == 2) cp = c & 0x1F;
            else if (need == 3) cp = c & 0x0F;
            else cp = c & 0x07;
            BOOL bad = FALSE;
            for (int k = 1; k < need; k++) {
                unsigned char cc = (unsigned char)p[k];
                if ((cc & 0xC0) != 0x80) { bad = TRUE; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (bad) return 0;
            wchar_t w[2];
            int wl = 0;
            if (cp < 0x10000) w[wl++] = (wchar_t)cp;
            else {
                cp -= 0x10000;
                w[wl++] = (wchar_t)(0xD800 + (cp >> 10));
                w[wl++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
            }
            for (int k = 0; k < wl; k++)
                if (n < outMax - 1) out[n++] = w[k];
            p += need;
        }
    }
    return 0;
}

/* find "key" [ws]* ':' [ws]* '"' inside [p,end); returns pointer to the
 * value's opening quote, or NULL. whitespace-tolerant. */
static void AiPost(const wchar_t *txt);
static const char *JsonFindStr(const char *p, const char *end, const char *key)
{
    int kl = (int)lstrlenA(key);
    for (const char *q = p; q + kl < end; q++) {
        if (memcmp(q, key, kl) != 0) continue;
        const char *r = q + kl;
        while (r < end && (*r == ' ' || *r == '\t')) r++;
        if (r >= end || *r != ':') continue;
        r++;
        while (r < end && (*r == ' ' || *r == '\t')) r++;
        if (r < end && *r == '"') return r;
    }
    return NULL;
}

/* extract answer text from one SSE "data:" payload or a whole JSON body;
 * handles streamed "delta":{"content":..} and non-streamed
 * "message":{"content":..} shapes. returns wchar count, 0 when none. */
static int SseParsePayload(const char *pl, int plen, wchar_t *out, int outMax)
{
    const char *end = pl + plen;
    const char *d = JsonFindStr(pl, end, "\"delta\"");
    const char *v = d ? JsonFindStr(d, end, "\"content\"") : NULL;
    if (!v) v = JsonFindStr(pl, end, "\"content\"");
    if (!v) return 0;
    int used = JsonReadString(v, end, out, outMax);
    if (used > 0) return lstrlenW(out);
    return 0;
}

/* parse one SSE/JSON line and post visible text; returns TRUE on [DONE] */
static BOOL AiHandleLine(const char *ln, int ll, wchar_t *chunk, int *got)
{
    if (ll > 5 && memcmp(ln, "data:", 5) == 0) {
        const char *pl = ln + 5;
        int plen = ll - 5;
        while (plen > 0 && (*pl == ' ' || *pl == '\t')) { pl++; plen--; }
        if (plen == 5 && memcmp(pl, "[DONE]", 5) == 0) return TRUE;
        int cn = SseParsePayload(pl, plen, chunk, 8192);
        if (cn > 0) { AiPost(chunk); *got += cn; }
    } else if (ll > 2) {
        int off = 0;
        while (off < ll && (ln[off] == ' ' || ln[off] == '\t'
                            || ln[off] == '\r')) off++;
        /* non-SSE line: maybe a whole non-stream JSON body */
        if (ln[off] == '{') {
            int cn = SseParsePayload(ln + off, ll - off, chunk, 8192);
            if (cn > 0) { AiPost(chunk); *got += cn; }
        }
    }
    return FALSE;
}

static void AiPost(const wchar_t *txt)
{
    int n = lstrlenW(txt);
    wchar_t *heap = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!heap) return;
    memcpy(heap, txt, (n + 1) * sizeof(wchar_t));
    PostMessageW(g_hwnd, WM_AI_CHUNK, n, (LPARAM)heap);
}

static void AiPostDone(const wchar_t *err)
{
    int n = err ? lstrlenW(err) : 0;
    wchar_t *heap = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!heap) return;
    if (err) memcpy(heap, err, (n + 1) * sizeof(wchar_t));
    else heap[0] = 0;
    PostMessageW(g_hwnd, WM_AI_DONE, err ? 0 : 1, (LPARAM)heap);
}

static DWORD WINAPI AiThreadProc(LPVOID param)
{
    AiJob job = *(AiJob *)param;
    free(param);

    wchar_t *errText = NULL;
    wchar_t *doneText = NULL;

    /* full endpoint url: base minus trailing slashes + /chat/completions
     * (skip when the base already ends with it) */
    wchar_t url[600];
    lstrcpynW(url, job.base, 580);
    int ul = lstrlenW(url);
    while (ul > 0 && url[ul - 1] == L'/') url[--ul] = 0;
    static const wchar_t EP[] = L"/chat/completions";
    const int epl = 17;
    BOOL hasEp = FALSE;
    if (ul >= epl - 1 && wcscmp(url + ul - (epl - 1), EP + 1) == 0
        && (ul == epl - 1 || url[ul - epl] == L'/'))
        hasEp = TRUE;
    if (!hasEp) lstrcpynW(url + ul, EP, 600 - ul);

    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[512];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 511;
    uc.dwSchemeLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        doneText = L"Base URL 无效";
        goto post;
    }

    HINTERNET ses = WinHttpOpen(L"MDLite/1.0",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { doneText = L"网络初始化失败"; goto post; }
    HINTERNET conn = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!conn) { WinHttpCloseHandle(ses); doneText = L"连接失败"; goto post; }
    HINTERNET req = WinHttpOpenRequest(conn, L"POST", path, NULL,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       (uc.nScheme == INTERNET_SCHEME_HTTPS)
                                           ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        WinHttpCloseHandle(conn); WinHttpCloseHandle(ses);
        doneText = L"创建请求失败"; goto post;
    }
    g_aiReq = req;
    InterlockedExchange((volatile LONG *)&g_aiCancelFlag, 0);

    int to = job.timeout > 0 ? job.timeout : 10;
    WinHttpSetTimeouts(ses, 5000, 5000, to * 1000, to * 1000);

    /* request body (UTF-8, JSON-escaped fields) */
    char body[28672];
    wchar_t esc[5120];
    int bo = 0;
    int bodyCap = 28000;
    strcpy(body + bo, "{\"model\":\"");
    bo += lstrlenA(body + bo);
    {
        JsonEscape(job.model, esc, 5120);
        int bl = WideCharToMultiByte(CP_UTF8, 0, esc, -1, body + bo,
                                     bodyCap - bo, NULL, NULL) - 1;
        if (bl < 0) bl = 0;
        bo += bl;
    }
    strcpy(body + bo, "\",\"stream\":true,\"messages\":[");
    bo += lstrlenA(body + bo);
    if (job.sys[0]) {
        JsonEscape(job.sys, esc, 5120);
        strcpy(body + bo, "{\"role\":\"system\",\"content\":\"");
        bo += lstrlenA(body + bo);
        int bl = WideCharToMultiByte(CP_UTF8, 0, esc, -1, body + bo,
                                     bodyCap - bo, NULL, NULL) - 1;
        if (bl < 0) bl = 0;
        bo += bl;
        strcpy(body + bo, "\"},");
        bo += lstrlenA(body + bo);
    }
    /* prior turns: collect newest-first within a budget, emit oldest-first */
    {
        const wchar_t *hq[AI_CTX_MAX], *ha[AI_CTX_MAX];
        int hn = 0, budget = 8000; /* wchar budget for history */
        int rounds = g_aiRounds;
        if (rounds > AI_CTX_MAX) rounds = AI_CTX_MAX;
        for (int i = g_aiCtxCnt - 1; i >= 0 && hn < rounds; i--) {
            int ql = lstrlenW(g_aiCtxQ[i]);
            int al = lstrlenW(g_aiCtxA[i]);
            if (ql + al > budget) break;
            budget -= ql + al;
            hq[hn] = g_aiCtxQ[i];
            ha[hn] = g_aiCtxA[i];
            hn++;
        }
        for (int i = hn - 1; i >= 0; i--) {
            const wchar_t *turn[2] = { hq[i], ha[i] };
            for (int t = 0; t < 2; t++) {
                JsonEscape(turn[t], esc, 5120);
                strcpy(body + bo, "{\"role\":\"");
                bo += lstrlenA(body + bo);
                lstrcpyA(body + bo, t ? "assistant" : "user");
                bo += lstrlenA(body + bo);
                strcpy(body + bo, "\",\"content\":\"");
                bo += lstrlenA(body + bo);
                int bl = WideCharToMultiByte(CP_UTF8, 0, esc, -1, body + bo,
                                             bodyCap - bo, NULL, NULL) - 1;
                if (bl < 0) bl = 0;
                bo += bl;
                strcpy(body + bo, "\"},");
                bo += lstrlenA(body + bo);
            }
        }
    }
    {
        JsonEscape(job.q, esc, 5120);
        strcpy(body + bo, "{\"role\":\"user\",\"content\":\"");
        bo += lstrlenA(body + bo);
        int bl = WideCharToMultiByte(CP_UTF8, 0, esc, -1, body + bo,
                                     bodyCap - bo, NULL, NULL) - 1;
        if (bl < 0) bl = 0;
        bo += bl;
        strcpy(body + bo, "\"}");
        bo += lstrlenA(body + bo);
    }
    strcpy(body + bo, "]}");
    bo += lstrlenA(body + bo);

    wchar_t hdrs[1100];
    if (job.key[0])
        wsprintfW(hdrs, L"Content-Type: application/json\r\n"
                        L"Authorization: Bearer %s\r\n", job.key);
    else
        lstrcpynW(hdrs, L"Content-Type: application/json\r\n", 600);

    BOOL ok = WinHttpSendRequest(req, hdrs, (DWORD)-1,
                                 (LPVOID)body, bo, bo, 0)
              && WinHttpReceiveResponse(req, NULL);
    if (!ok) {
        DWORD we = GetLastError();
        if (g_aiCancelFlag || we == ERROR_INVALID_HANDLE)
            doneText = L"已取消";
        else if (we == ERROR_WINHTTP_TIMEOUT)
            doneText = L"请求超时";
        else {
            errText = (wchar_t *)malloc(64 * sizeof(wchar_t));
            if (errText)
                wsprintfW(errText, L"发送失败（错误码 %u）", we);
            doneText = errText;
        }
        goto cleanup;
    }

    /* HTTP status */
    DWORD status = 0, szd = sizeof(status);
    WinHttpQueryHeaders(req,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL,
        &status, &szd, NULL);
    if (status != 200) {
        errText = (wchar_t *)malloc(64 * sizeof(wchar_t));
        if (errText) wsprintfW(errText, L"HTTP %u", status);
        doneText = errText;
        goto cleanup;
    }

    /* stream + SSE (also tolerates non-stream whole-JSON bodies) */
    {
        char sse[16384];
        int sseLen = 0;
        BOOL fin = FALSE;
        int got = 0;              /* chars successfully shown */
        wchar_t chunk[8192];
        while (!fin && !g_aiCancelFlag) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) {
                DWORD we = GetLastError();
                if (g_aiCancelFlag || we == ERROR_INVALID_HANDLE)
                    doneText = L"已取消";
                else if (we == ERROR_WINHTTP_TIMEOUT)
                    doneText = L"接收超时";
                else
                    doneText = L"连接中断";
                break;
            }
            if (avail == 0) break; /* server closed: stream end */
            DWORD cap = (DWORD)(sizeof(sse) - 1 - (size_t)sseLen);
            if (cap == 0) sseLen = 0, cap = sizeof(sse) - 1; /* giant line */
            if (avail > cap) avail = cap;
            DWORD rd = 0;
            if (!WinHttpReadData(req, sse + sseLen, avail, &rd) || rd == 0) {
                DWORD we = GetLastError();
                if (g_aiCancelFlag || we == ERROR_INVALID_HANDLE)
                    doneText = L"已取消";
                else if (we == ERROR_WINHTTP_TIMEOUT)
                    doneText = L"接收超时";
                else
                    doneText = L"连接中断";
                break;
            }
            sseLen += rd;
            /* process complete lines */
            int ls = 0;
            for (int i = 0; i < sseLen; i++) {
                if (sse[i] != '\n') continue;
                int le = i;
                if (le > ls && sse[le - 1] == '\r') le--;
                if (!fin && AiHandleLine(sse + ls, le - ls, chunk, &got))
                    fin = TRUE;
                ls = i + 1;
            }
            if (ls > 0) {
                memmove(sse, sse + ls, sseLen - ls);
                sseLen -= ls;
            }
        }
        /* server closed without trailing newline: last unterminated line */
        if (!doneText && sseLen > 0)
            AiHandleLine(sse, sseLen, chunk, &got);
        /* never leave the user without feedback */
        if (!doneText && got == 0) {
            if (g_aiCancelFlag) doneText = L"已取消";
            else doneText = L"未收到内容（模型或接口可能不兼容，"
                             L"请检查 Base URL 与模型）";
        }
    }

cleanup:
    g_aiReq = NULL;
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(ses);
post:
    AiPostDone(doneText);
    free(errText);
    return 0;
}

/* -------- shared background-task insert machinery (AI + agent) -------- */

/* find the insert point: end of the line that matches the task's anchor.
 * the anchor is the question line text; when the user edits elsewhere the
 * line moves but is still found, so the answer always lands directly
 * under its question. falls back to document end when the anchor is gone
 * (e.g. user deleted the question) so no streamed text is ever lost. */
static int TaskLocate(BgTask *t)
{
    if (!t->anchor[0]) {
        int len = GetWindowTextLengthW(g_edit);
        if (t->insEnd < len) t->insEnd = len;
        return t->insEnd;
    }
    int aLen = lstrlenW(t->anchor);
    int total = GetWindowTextLengthW(g_edit);
    /* scan lines from the top; compare each line against the anchor */
    int line = 0, pos = 0;
    wchar_t buf[2048];
    while (pos <= total) {
        int ll = (int)SendMessageW(g_edit, EM_LINELENGTH, pos, 0);
        if (ll > 0) {
            int want = ll + 1;
            if (want > 2048) want = 2048;
            buf[0] = (wchar_t)(want - 1);
            int n = (int)SendMessageW(g_edit, EM_GETLINE, line,
                                      (LPARAM)buf);
            if (n > 0) {
                buf[n] = 0;
                if (n >= aLen && wcsncmp(buf, t->anchor, aLen) == 0) {
                    /* derive the offset from the anchor every time:
                     * answer block starts right below the question
                     * ("\r\n---\r\n" = 7 chars) plus everything this task
                     * has streamed so far. user edits anywhere else can
                     * shift raw offsets, so never trust a stored one. */
                    int lineEnd = pos + ll;
                    t->insEnd = lineEnd + 7 + t->totalLen;
                    return t->insEnd;
                }
            }
        }
        if (pos + ll >= total) break;
        pos = pos + ll + 2;   /* skip CRLF */
        line++;
    }
    /* anchor lost: append at document end */
    if (t->insEnd < total) t->insEnd = total;
    return t->insEnd;
}

/* insert text at the task's tracked position without stealing the user's
 * caret: remember the caret, insert, then restore it shifted by however
 * many chars landed before it */
static void TaskInsert(BgTask *t, const wchar_t *txt)
{
    DWORD s, e;
    BOOL hadSel = TRUE;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    int at = TaskLocate(t);
    int n = lstrlenW(txt);
    SendMessageW(g_edit, EM_SETSEL, at, at);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)txt);
    t->insEnd = at + n;
    t->totalLen += n;
    /* restore the user's caret/selection, compensated for the insert */
    if (s >= (DWORD)at) s += n;
    if (e >= (DWORD)at) e += n;
    SendMessageW(g_edit, EM_SETSEL, s, e);
    if (s == e) hadSel = FALSE;
    /* keep the answer tail roughly in view only when the user's caret
     * sits inside the streaming region (watching it arrive) */
    if (s >= (DWORD)at && s <= (DWORD)(at + n))
        SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
    (void)hadSel;
}

/* begin a task slot: record anchor and open the answer block under the
 * question line with a leading separator */
static BOOL TaskBegin(int slot, const wchar_t *question)
{
    (void)question;
    BgTask *t = &g_tasks[slot];
    DWORD s, e;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    int li = (int)SendMessageW(g_edit, EM_LINEFROMCHAR, s, 0);
    int ls = (int)SendMessageW(g_edit, EM_LINEINDEX, li, 0);
    int ll = (int)SendMessageW(g_edit, EM_LINELENGTH, ls, 0);
    int lineEnd = ls + ll;

    /* remember the question line as anchor (truncate long ones) */
    wchar_t buf[2048];
    buf[0] = 2048;
    int n = (int)SendMessageW(g_edit, EM_GETLINE, li, (LPARAM)buf);
    if (n > 0) buf[n] = 0; else buf[0] = 0;
    lstrcpynW(t->anchor, buf, 2048);

    SendMessageW(g_edit, EM_SETSEL, lineEnd, lineEnd);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n---\r\n");
    t->insEnd = lineEnd + 5;
    t->totalLen = 0;
    t->active = TRUE;
    return TRUE;
}

/* finish a task slot: trailing separator (+ optional error note) */
static void TaskFinish(int slot, const wchar_t *err)
{
    BgTask *t = &g_tasks[slot];
    wchar_t tail[1100];
    if (err && err[0])
        wsprintfW(tail, L"\r\n---\r\n（%s）\r\n", err);
    else
        lstrcpynW(tail, L"\r\n---\r\n", 512);
    DWORD s, e;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    int at = TaskLocate(t);
    int n = lstrlenW(tail);
    SendMessageW(g_edit, EM_SETSEL, at, at);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)tail);
    if (s >= (DWORD)at) s += n;
    if (e >= (DWORD)at) e += n;
    SendMessageW(g_edit, EM_SETSEL, s, e);
    t->active = FALSE;
}

/* duplicate at most `max` characters of s (NUL-terminated) */
static wchar_t *DupTruncW(const wchar_t *s, int max)
{
    int n = lstrlenW(s);
    if (n > max) n = max;
    wchar_t *d = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (d) { memcpy(d, s, n * sizeof(wchar_t)); d[n] = 0; }
    return d;
}

/* remember a finished Q/A turn for multi-turn context */
static void StoreAiTurn(const wchar_t *q, const wchar_t *a)
{
    if (!q || !a || !q[0] || !a[0]) return;
    if (g_aiCtxCnt >= AI_CTX_MAX) {
        free(g_aiCtxQ[0]);
        free(g_aiCtxA[0]);
        for (int i = 1; i < AI_CTX_MAX; i++) {
            g_aiCtxQ[i - 1] = g_aiCtxQ[i];
            g_aiCtxA[i - 1] = g_aiCtxA[i];
        }
        g_aiCtxCnt = AI_CTX_MAX - 1;
    }
    wchar_t *dq = DupTruncW(q, 600);
    wchar_t *da = DupTruncW(a, 2400);
    if (dq && da) {
        g_aiCtxQ[g_aiCtxCnt] = dq;
        g_aiCtxA[g_aiCtxCnt] = da;
        g_aiCtxCnt++;
    } else {
        free(dq);
        free(da);
    }
}

/* wide-char append with bounds check (wsprintfW caps at 1024 chars) */

/* ------------------------------------------------------------------ */
/* selection AI (item 24): Ctrl+J acts on the selected text            */
/* ------------------------------------------------------------------ */

static BOOL     g_selAi;           /* selection-AI session active */
static DWORD    g_selAiStart;      /* original selection start */
static DWORD    g_selAiIns;        /* streaming insertion point */
static wchar_t *g_selAiBackup;     /* original selected text */

/* wide-char append with bounds check (wsprintfW caps at 1024 chars) */
static int Wa(wchar_t *dst, int cap, int off, const wchar_t *s);

static void SelAiInsert(const wchar_t *txt)
{
    int n = lstrlenW(txt);
    if (n <= 0) return;
    SendMessageW(g_edit, EM_SETSEL, g_selAiIns, g_selAiIns);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)txt);
    g_selAiIns += n;
    SendMessageW(g_edit, EM_SETSEL, g_selAiIns, g_selAiIns);
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
}

static void SelAiFinish(const wchar_t *err)
{
    if (err && err[0] && g_selAiBackup) {
        /* failed or interrupted: undo the streamed text, restore */
        SendMessageW(g_edit, EM_SETSEL, g_selAiStart, g_selAiIns);
        SendMessageW(g_edit, EM_REPLACESEL, FALSE,
                     (LPARAM)g_selAiBackup);
        int bl = lstrlenW(g_selAiBackup);
        SendMessageW(g_edit, EM_SETSEL, g_selAiStart,
                     g_selAiStart + (DWORD)bl);
    }
    free(g_selAiBackup);
    g_selAiBackup = NULL;
    g_selAi = FALSE;
}

/* tiny modal input popup (custom prompt for selection AI) */
static BOOL PromptInput(const wchar_t *title, wchar_t *out, int cch)
{
    HWND pw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"#32770" /* built-in dialog class: frame + title bar */,
        title, WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU,
        0, 0, SC(380), SC(130), g_hwnd, NULL, NULL, NULL);
    if (!pw) return FALSE;
    RECT rw;
    GetWindowRect(pw, &rw);
    RECT rm;
    GetWindowRect(g_hwnd, &rm);
    SetWindowPos(pw, 0,
                 rm.left + (rm.right - rm.left - (rw.right - rw.left)) / 2,
                 rm.top + (rm.bottom - rm.top - (rw.bottom - rw.top)) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", out,
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        SC(14), SC(14), SC(352), SC(26), pw, (HMENU)1, NULL, NULL);
    CreateWindowExW(0, L"BUTTON", L"确定",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        SC(220), SC(56), SC(66), SC(28), pw, (HMENU)IDOK, NULL, NULL);
    CreateWindowExW(0, L"BUTTON", L"取消",
        WS_CHILD | WS_VISIBLE,
        SC(296), SC(56), SC(66), SC(28), pw, (HMENU)IDCANCEL, NULL, NULL);
    SendMessageW(ed, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
    HWND bn = GetDlgItem(pw, IDOK);
    if (bn) SendMessageW(bn, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
    bn = GetDlgItem(pw, IDCANCEL);
    if (bn) SendMessageW(bn, WM_SETFONT, (WPARAM)g_fontHeader, TRUE);
    ShowWindow(pw, SW_SHOW);
    UpdateWindow(pw);
    SetFocus(ed);
    BOOL ok = FALSE;
    MSG msg;
    for (;;) {
        while (IsWindow(pw) && GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (!IsWindow(pw)) break;
            if ((msg.hwnd == pw || IsChild(pw, msg.hwnd))
                && msg.message == WM_KEYDOWN) {
                if (msg.wParam == VK_RETURN) {
                    GetWindowTextW(ed, out, cch);
                    ok = out[0] != 0;
                    goto done;
                }
                if (msg.wParam == VK_ESCAPE) { ok = FALSE; goto done; }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (!IsWindow(pw)) { ok = FALSE; goto done2; }
        }
        if (!IsWindow(pw)) break;
    }
done:
    DestroyWindow(pw);
done2:
    return ok;
}

static void StartSelAi(const wchar_t *instr)
{
    if (!g_aiBase[0] || !g_aiKey[0]) {
        MessageBoxW(g_hwnd,
            L"尚未配置 AI。\n按 Ctrl+, 打开设置，填写 Base URL 与 API Key。",
            APP_NAME, MB_ICONINFORMATION);
        return;
    }
    DWORD s, e;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    if (e <= s) return;
    int slen = (int)(e - s);
    if (slen > 2048) slen = 2048;
    wchar_t *sel = (wchar_t *)malloc((slen + 1) * sizeof(wchar_t));
    if (!sel) return;
    /* copy the selection directly from the window text */
    {
        wchar_t *all = NULL;
        int total = GetWindowTextLengthW(g_edit);
        all = (wchar_t *)malloc((total + 1) * sizeof(wchar_t));
        if (!all) { free(sel); return; }
        GetWindowTextW(g_edit, all, total + 1);
        int copy = (int)(e - s);
        if (copy > slen) copy = slen;
        memcpy(sel, all + s, copy * sizeof(wchar_t));
        sel[copy] = 0;
        free(all);
    }

    /* build question: instruction + content (cap 950 chars total) */
    wchar_t q[1024];
    int o = 0;
    o = Wa(q, 1024, o, instr);
    o = Wa(q, 1024, o, L"以下内容：\n\n");
    int room = 1010 - o - 4;
    int cl = lstrlenW(sel);
    if (cl > room) {
        for (int k = 0; k < room && o < 1010; k++) q[o++] = sel[k];
        q[o++] = L'…';
        q[o] = 0;
    } else {
        o = Wa(q, 1024, o, sel);
    }
    free(sel);

    g_selAiBackup = (wchar_t *)malloc((e - s + 1) * sizeof(wchar_t));
    if (!g_selAiBackup) return;
    {
        wchar_t *all = NULL;
        int total = GetWindowTextLengthW(g_edit);
        all = (wchar_t *)malloc((total + 1) * sizeof(wchar_t));
        if (!all) { free(g_selAiBackup); g_selAiBackup = NULL; return; }
        GetWindowTextW(g_edit, all, total + 1);
        int bl = (int)(e - s);
        memcpy(g_selAiBackup, all + s, bl * sizeof(wchar_t));
        g_selAiBackup[bl] = 0;
        free(all);
    }
    g_selAiStart = s;
    g_selAiIns = s;
    g_selAi = TRUE;
    /* remove the selection; the answer streams in its place */
    SendMessageW(g_edit, EM_SETSEL, s, e);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)L"");
    StartAi(q);
}

void ShowSelAiMenu(void)
{
    DWORD s, e;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    if (e <= s) {
        MessageBoxW(g_hwnd, L"请先选中要处理的文字，再按 Ctrl+J。",
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }
    static const struct { const wchar_t *label; const wchar_t *instr; }
    cmds[] = {
        { L"润色",         L"请润色并直接输出改进后的文字：" },
        { L"翻译成中文",   L"请翻译成中文并直接输出译文：" },
        { L"翻译成英文",   L"请翻译成英文并直接输出译文：" },
        { L"总结",         L"请用一两句话总结以下内容：" },
        { L"解释",         L"请解释以下内容：" },
    };
    HMENU pm = CreatePopupMenu();
    for (int i = 0; i < 5; i++)
        AppendMenuW(pm, MF_STRING, 500 + i, cmds[i].label);
    AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(pm, MF_STRING, 506, L"自定义…");
    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(pm, TPM_LEFTALIGN | TPM_RIGHTBUTTON
                                | TPM_RETURNCMD, pt.x, pt.y, 0, g_hwnd,
                             NULL);
    DestroyMenu(pm);
    if (cmd >= 500 && cmd < 505) {
        StartSelAi(cmds[cmd - 500].instr);
    } else if (cmd == 506) {
        wchar_t buf[256] = L"";
        if (PromptInput(L"自定义 AI 指令", buf, 256))
            StartSelAi(buf);
    }
}

void StartAi(const wchar_t *question)
{
    AiJob *job = (AiJob *)malloc(sizeof(AiJob));
    if (!job) return;
    lstrcpynW(job->base, g_aiBase, 256);
    lstrcpynW(job->model, g_aiModel, 128);
    lstrcpynW(job->key, g_aiKey, 256);
    lstrcpynW(job->sys, g_aiSys, 1024);
    lstrcpynW(job->q, question, 1024);
    job->timeout = g_aiTimeout;

    /* remember this turn; the answer accumulates via WM_AI_CHUNK */
    lstrcpynW(g_aiLastQ, question, 1024);
    if (!g_aiLastA)
        g_aiLastA = (wchar_t *)malloc(2410 * sizeof(wchar_t));
    if (g_aiLastA) { g_aiLastA[0] = 0; g_aiLastALen = 0; }

    if (!g_selAi)
        TaskBegin(0, question);   /* anchor + leading separator */

    g_aiBusy = TRUE;
    g_aiStartTick = GetTickCount();
    g_stBusySec = -1;
    InvalidateRect(g_hwnd, NULL, FALSE);

    HANDLE th = CreateThread(NULL, 0, AiThreadProc, job, 0, NULL);
    if (th) CloseHandle(th);
    else {
        free(job);
        g_aiBusy = FALSE;
        TaskFinish(0, L"无法启动任务");
    }
}

void AiCancel(void)
{
    if (!g_aiBusy) return;
    InterlockedExchange((volatile LONG *)&g_aiCancelFlag, 1);
    /* the worker notices the flag between reads; also close the request
     * so a blocked read unblocks immediately */
    if (g_aiReq) {
        WinHttpCloseHandle((HINTERNET)g_aiReq);
        g_aiReq = NULL;
    }
}

/* flush batched AI text into the editor in one shot */
void AiFlushPending(void)
{
    KillTimer(g_hwnd, TIMER_AICHUNK);
    if (!g_aiPendingLen || !g_aiPending) {
        g_aiPendingLen = 0;
        return;
    }
    if (g_selAi)
        SelAiInsert(g_aiPending);
    else
        TaskInsert(&g_tasks[0], g_aiPending);
    g_aiPendingLen = 0;
    if (g_aiPending) g_aiPending[0] = 0;
}

void AiCancel(void);
void AiFlushPending(void);

/* ------------------------------------------------------------------ */
/* Agent: "opencode run <prompt>" as a background task                  */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t exe[512];    /* resolved opencode path (unquoted) */
    BOOL    useCmd;      /* .cmd/.bat needs cmd.exe wrapper */
    wchar_t prompt[2048];
    wchar_t cwd[MAX_PATH];
    int timeout;
} AgJob;

/* wide-char append with bounds check (wsprintfW caps at 1024 chars) */
static int Wa(wchar_t *dst, int cap, int off, const wchar_t *s)
{
    while (*s && off < cap - 1) dst[off++] = *s++;
    return off;
}

/* strip ANSI escape sequences (CSI/OSC/2-char) in place */
static void StripAnsiW(wchar_t *s)
{
    int i = 0, o = 0;
    while (s[i]) {
        if (s[i] == 27 && s[i + 1] == L'[') {
            i += 2;
            while (s[i] && !(s[i] >= L'@' && s[i] <= L'~')) i++;
            if (s[i]) i++;
        } else if (s[i] == 27 && s[i + 1] == L']') {
            i += 2;
            while (s[i] && s[i] != 7
                   && !(s[i] == 27 && s[i + 1] == L'\\')) i++;
            if (s[i] == 7) i++;
            else if (s[i] == 27) i += 2;
        } else if (s[i] == 27) {
            i += 2;
        } else {
            s[o++] = s[i++];
        }
    }
    s[o] = 0;
}

/* resolve the opencode executable.
 * returns 0 = direct exe, 1 = script needing cmd.exe, -1 = not found.
 * out is filled with the unquoted path. */
static int ResolveOpenCode(wchar_t *out, int cch)
{
    out[0] = 0;
    wchar_t raw[512];
    lstrcpynW(raw, g_agPath, 512);
    int len = lstrlenW(raw);
    if (len >= 2 && raw[0] == L'"' && raw[len - 1] == L'"') {
        raw[len - 1] = 0;
        for (int i = 0; raw[i + 1]; i++) raw[i] = raw[i + 1];
        raw[len - 2] = 0;
    }
    if (raw[0]) {
        lstrcpynW(out, raw, cch);
        len = lstrlenW(out);
        int s = len - 4;
        if (s >= 0) {
            wchar_t lo[5];
            for (int i = 0; i < 4; i++)
                lo[i] = (out[s + i] >= L'A' && out[s + i] <= L'Z')
                        ? out[s + i] + 32 : out[s + i];
            lo[4] = 0;
            if (!lstrcmpW(lo, L".cmd") || !lstrcmpW(lo, L".bat"))
                return 1;
        }
        return 0;
    }
    /* no user path: search PATH dirs only (skip cwd to avoid hijack).
     * npm installs opencode as a .cmd shim on windows */
    wchar_t env[2048];
    DWORD pn = GetEnvironmentVariableW(L"PATH", env, 2048);
    if (pn == 0 || pn >= 2048) return -1;
    static const wchar_t *names[3] =
        { L"opencode.exe", L"opencode.cmd", L"opencode.bat" };
    for (int i = 0; i < 3; i++) {
        wchar_t f[MAX_PATH];
        if (SearchPathW(env, names[i], NULL, MAX_PATH, f, NULL)) {
            lstrcpynW(out, f, cch);
            return i == 0 ? 0 : 1;
        }
    }
    return -1;
}

static void AgPost(const wchar_t *txt)
{
    int n = lstrlenW(txt);
    wchar_t *heap = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!heap) return;
    memcpy(heap, txt, (n + 1) * sizeof(wchar_t));
    PostMessageW(g_hwnd, WM_AG_CHUNK, n, (LPARAM)heap);
}

static void AgPostDone(const wchar_t *err)
{
    int n = err ? lstrlenW(err) : 0;
    wchar_t *heap = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!heap) return;
    if (err) memcpy(heap, err, (n + 1) * sizeof(wchar_t));
    else heap[0] = 0;
    PostMessageW(g_hwnd, WM_AG_DONE, err ? 0 : 1, (LPARAM)heap);
}

static DWORD WINAPI AgThreadProc(LPVOID param)
{
    AgJob *job = (AgJob *)param;

    /* command line:
     *   direct: "<exe>" run "<prompt>"
     *   script: "<sysdir>\cmd.exe" /d /s /c ""<exe>" run "<prompt>>"
     * cmd.exe cannot run .cmd/.bat via CreateProcess directly; prompt
     * quotes doubled and newlines flattened for the script case */
    wchar_t cmd[3072];
    int co = 0;
    if (job->useCmd) {
        wchar_t sysdir[MAX_PATH];
        GetSystemDirectoryW(sysdir, MAX_PATH);
        cmd[co++] = L'"';
        co = Wa(cmd, 3072, co, sysdir);
        co = Wa(cmd, 3072, co, L"\\cmd.exe\" /d /s /c \"");
        cmd[co++] = L'"';
        co = Wa(cmd, 3072, co, job->exe);
        co = Wa(cmd, 3072, co, L"\" run \"");
    } else {
        cmd[co++] = L'"';
        co = Wa(cmd, 3072, co, job->exe);
        co = Wa(cmd, 3072, co, L"\" run \"");
    }
    for (const wchar_t *p = job->prompt; *p && co < 3000; p++) {
        if (*p == L'"' && job->useCmd) { cmd[co++] = L'"'; cmd[co++] = L'"'; }
        else if ((*p == L'\n' || *p == L'\r') && job->useCmd)
            cmd[co++] = L' ';
        else cmd[co++] = *p;
    }
    cmd[co++] = L'"';
    if (job->useCmd) cmd[co++] = L'"';
    cmd[co] = 0;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL, rdErr = NULL, wrErr = NULL, hNul = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        free(job); AgPostDone(L"无法创建管道"); return 0;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&rdErr, &wrErr, &sa, 0)) {
        CloseHandle(rd); CloseHandle(wr);
        free(job); AgPostDone(L"无法创建管道"); return 0;
    }
    SetHandleInformation(rdErr, HANDLE_FLAG_INHERIT, 0);
    /* stdin must be a valid handle: node/bun runtimes probe it; NULL
     * makes some builds die silently. NUL device reads EOF forever. */
    hNul = CreateFileW(L"NUL", GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       OPEN_EXISTING, 0, NULL);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (hNul != INVALID_HANDLE_VALUE) ? hNul : NULL;
    si.hStdOutput = wr;
    si.hStdError = wrErr;   /* collect diagnostics (gui proc has none) */

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL,
                        job->cwd[0] ? job->cwd : NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        CloseHandle(rdErr); CloseHandle(wrErr);
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        free(job); AgPostDone(L"启动 opencode 失败（检查路径配置）");
        return 0;
    }
    g_agProc = pi.hProcess;
    CloseHandle(pi.hThread);
    CloseHandle(wr);        /* parent must close its write end */
    CloseHandle(wrErr);

    /* read stdout as UTF-8, forward as chunks; accumulate stderr for
     * diagnostics when the agent produces no output */
    char buf[8192];
    DWORD rdBytes = 0;
    wchar_t wbuf[8192];
    char errAcc[8192];
    int errLen = 0;
    BOOL timedOut = FALSE;
    DWORD startTick = GetTickCount();
    int got = 0;
    /* UTF-8 sequences can split across pipe reads; carry the tail */
    char carry[4];
    int carryN = 0;
    for (;;) {
        if (g_agCancel) { TerminateProcess(pi.hProcess, 1); timedOut = 2; break; }
        DWORD avail = 0;
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            int prep = carryN;
            if (prep) { memcpy(buf, carry, prep); carryN = 0; }
            DWORD want = avail < (DWORD)(8191 - prep) ? avail
                                                      : (DWORD)(8191 - prep);
            if (!ReadFile(rd, buf + prep, want, &rdBytes, NULL) || !rdBytes)
                break;
            int total = prep + (int)rdBytes;
            /* stash a partial trailing sequence for the next round */
            if ((buf[total - 1] & 0xC0) == 0x80) {
                int q = total - 1, back = 0;
                while (q >= 0 && (buf[q] & 0xC0) == 0x80 && back < 3)
                    { q--; back++; }
                int keep = 0;
                if (q >= 0 && (buf[q] & 0x80)) {
                    int need = (buf[q] & 0xE0) == 0xC0 ? 2
                             : (buf[q] & 0xF0) == 0xE0 ? 3
                             : (buf[q] & 0xF8) == 0xF0 ? 4 : 0;
                    if (need && total - q < need) keep = total - q;
                }
                if (keep) { memcpy(carry, buf + total - keep, keep);
                            carryN = keep; total -= keep; }
            }
            int wn = MultiByteToWideChar(CP_UTF8, 0, buf, total,
                                         wbuf, 8191);
            if (wn > 0) {
                wbuf[wn] = 0;
                StripAnsiW(wbuf);
                if (wbuf[0]) { AgPost(wbuf); got += wn; }
            }
            continue;
        }
        DWORD availE = 0;
        if (PeekNamedPipe(rdErr, NULL, 0, NULL, &availE, NULL)
            && availE > 0) {
            DWORD want = availE < 1024 ? availE : 1024;
            if (!ReadFile(rdErr, buf, want, &rdBytes, NULL) || !rdBytes)
                break;
            int n = (int)rdBytes;
            if (n > 8191 - errLen) n = 8191 - errLen;
            if (n > 0) { memcpy(errAcc + errLen, buf, n); errLen += n; }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) break;
        if (job->timeout > 0
            && GetTickCount() - startTick > (DWORD)job->timeout * 1000) {
            TerminateProcess(pi.hProcess, 1);
            timedOut = 1;
            break;
        }
    }
    /* final drain: blocking reads until the pipe breaks (EOF). this is
     * the only reliable way to collect data still buffered in the pipe
     * when the process-handle signaled early; on the timeout/cancel
     * paths the child was already terminated, so the pipes break at
     * once */
    for (;;) {
        int prep = carryN;
        if (prep) { memcpy(buf, carry, prep); carryN = 0; }
        if (!ReadFile(rd, buf + prep, 8191 - prep, &rdBytes, NULL)
            || rdBytes == 0)
            break;
        int total = prep + (int)rdBytes;
        int wn = MultiByteToWideChar(CP_UTF8, 0, buf, total, wbuf, 8191);
        if (wn > 0) {
            wbuf[wn] = 0;
            StripAnsiW(wbuf);
            if (wbuf[0]) { AgPost(wbuf); got += wn; }
        }
    }
    for (;;) {
        if (!ReadFile(rdErr, buf, 2048, &rdBytes, NULL) || rdBytes == 0)
            break;
        int n = (int)rdBytes;
        if (n > 8191 - errLen) n = 8191 - errLen;
        if (n > 0) { memcpy(errAcc + errLen, buf, n); errLen += n; }
    }

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(rd);
    CloseHandle(rdErr);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);
    g_agProc = NULL;

    /* no output (or timed out)? surface the agent's own diagnostics so
     * failures are visible (auth errors, missing model, crash logs...) */
    if ((got == 0 || timedOut == 1) && errLen > 0) {
        int wn = MultiByteToWideChar(CP_UTF8, 0, errAcc, errLen,
                                     wbuf, 8191);
        if (wn > 0) {
            wbuf[wn] = 0;
            StripAnsiW(wbuf);
            AgPost(timedOut == 1 ? L"超时前 Agent 错误输出：\n"
                                 : L"Agent 未产生输出，其错误输出如下：\n");
            AgPost(wbuf);
            AgPost(L"\n");
        }
    }

    wchar_t err[128];
    err[0] = 0;
    if (timedOut == 1)
        wsprintfW(err, L"Agent 执行超时（上限 %d 秒）", job->timeout);
    else if (timedOut == 2) lstrcpynW(err, L"已取消", 128);
    else if (got == 0)
        lstrcpynW(err, errLen ? L"Agent 无输出（见错误信息）"
                              : L"Agent 无输出", 128);
    AgPostDone(err[0] ? err : NULL);
    free(job);
    return 0;
}

void StartAgent(const wchar_t *question)
{
    InterlockedExchange((volatile LONG *)&g_agCancel, 0);
    AgJob *job = (AgJob *)malloc(sizeof(AgJob));
    if (!job) return;
    int mode = ResolveOpenCode(job->exe, 512);
    if (mode < 0) {
        free(job);
        TaskBegin(1, question);
        TaskFinish(1, L"未找到 opencode（请安装或在设置中填写路径）");
        return;
    }
    job->useCmd = (mode == 1);
    job->timeout = g_agTimeout > 0 ? g_agTimeout : 60;
    /* prompt = agent system prompt prepended, then the user question */
    if (g_agSys[0])
        wsprintfW(job->prompt, L"%s\r\n\r\n%s", g_agSys, question);
    else
        lstrcpynW(job->prompt, question, 2048);
    /* run in the directory of the open document (agent needs the project
     * context); fall back to the user profile dir for unsaved docs */
    if (g_path[0]) {
        lstrcpynW(job->cwd, g_path, MAX_PATH);
        wchar_t *slash = wcsrchr(job->cwd, L'\\');
        if (slash) *slash = 0;
        else job->cwd[0] = 0;
    } else
        lstrcpynW(job->cwd, L".", MAX_PATH);

    TaskBegin(1, question);

    g_agBusy = TRUE;
    g_agStartTick = GetTickCount();
    g_stBusySec = -1;
    InvalidateRect(g_hwnd, NULL, FALSE);

    HANDLE th = CreateThread(NULL, 0, AgThreadProc, job, 0, NULL);
    if (th) CloseHandle(th);
    else {
        free(job);
        g_agBusy = FALSE;
        TaskFinish(1, L"无法启动任务");
    }
}

void AgCancel(void)
{
    if (!g_agBusy) return;
    InterlockedExchange((volatile LONG *)&g_agCancel, 1);
}

void AgFlushPending(void)
{
    KillTimer(g_hwnd, TIMER_AICHUNK);
    if (!g_agPendingLen || !g_agPending) {
        g_agPendingLen = 0;
        return;
    }
    TaskInsert(&g_tasks[1], g_agPending);
    g_agPendingLen = 0;
    if (g_agPending) g_agPending[0] = 0;
}


/* ------------------------------------------------------------------ */
/* editor subclass                                                      */
/* ------------------------------------------------------------------ */
#ifndef EM_GETSELTEXT
#define EM_GETSELTEXT 0x00B2
#endif

/* indent / unindent every line of the current selection */
static void EditIndentSel(HWND h, int add)
{
    DWORD s0, e0;
    SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
    int l0 = (int)SendMessageW(h, EM_LINEFROMCHAR, s0, 0);
    int l1 = (int)SendMessageW(h, EM_LINEFROMCHAR, e0, 0);
    if (l1 > l0 && e0 == (DWORD)SendMessageW(h, EM_LINEINDEX, l1, 0))
        l1--;                       /* caret on the following line start */
    if (l1 - l0 + 1 > 2000) return;

    wchar_t tmp[4096];
    int cap = 1 << 16;
    wchar_t *out = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!out) return;
    int ol = 0;
    for (int li = l0; li <= l1; li++) {
        int ls = (int)SendMessageW(h, EM_LINEINDEX, li, 0);
        int llen = (int)SendMessageW(h, EM_LINELENGTH, ls, 0);
        int n = 0;
        if (llen > 0 && llen < 4000)
            n = EditGetLine(h, li, tmp, 4000);
        if (add) {
            if (ol + n + 6 > cap) break;
            out[ol++] = L' '; out[ol++] = L' ';
            out[ol++] = L' '; out[ol++] = L' ';
        } else {
            int cut = 0;
            while (cut < n && cut < 4
                   && (tmp[cut] == L' ' || tmp[cut] == L'\t')) cut++;
            n -= cut;
            memmove(tmp, tmp + cut, (size_t)n * sizeof(wchar_t));
        }
        if (ol + n + 4 > cap) break;
        memcpy(out + ol, tmp, (size_t)n * sizeof(wchar_t));
        ol += n;
        if (li < l1) { out[ol++] = L'\r'; out[ol++] = L'\n'; }
    }
    out[ol] = 0;
    int la = (int)SendMessageW(h, EM_LINEINDEX, l0, 0);
    int lb = (int)SendMessageW(h, EM_LINEINDEX, l1, 0);
    lb += (int)SendMessageW(h, EM_LINELENGTH, lb, 0);
    SendMessageW(h, EM_SETSEL, la, lb);
    SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)out);
    SendMessageW(h, EM_SETSEL, la, la + ol);
    free(out);
}

/* duplicate the caret line right below it, keeping the column */
static void EditDupLine(HWND h)
{
    DWORD s0, e0;
    SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
    int li = (int)SendMessageW(h, EM_LINEFROMCHAR, s0, 0);
    int ls = (int)SendMessageW(h, EM_LINEINDEX, li, 0);
    int llen = (int)SendMessageW(h, EM_LINELENGTH, ls, 0);
    if (llen < 0 || llen > 16000) return;
    wchar_t *A = (wchar_t *)malloc((llen + 2) * sizeof(wchar_t));
    wchar_t *ins = (wchar_t *)malloc((llen + 4) * sizeof(wchar_t));
    if (!A || !ins) { free(A); free(ins); return; }
    EditGetLine(h, li, A, llen + 1);
    ins[0] = L'\r'; ins[1] = L'\n';
    memcpy(ins + 2, A, (size_t)llen * sizeof(wchar_t));
    ins[llen + 2] = 0;
    SendMessageW(h, EM_SETSEL, ls + llen, ls + llen);
    SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)ins);
    int col = (int)s0 - ls;
    if (col < 0) col = 0;
    if (col > llen) col = llen;
    SendMessageW(h, EM_SETSEL, ls + llen + 2 + col, ls + llen + 2 + col);
    free(A);
    free(ins);
}

/* swap the caret line with the one above/below */
static void EditMoveLine(HWND h, int dir)
{
    DWORD s0;
    SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)0);
    int li = (int)SendMessageW(h, EM_LINEFROMCHAR, s0, 0);
    int lj = li + dir;
    if (lj < 0) return;
    int total = (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0);
    if (lj >= total) return;

    int p = li < lj ? li : lj;
    int q = li < lj ? lj : li;
    int ip = (int)SendMessageW(h, EM_LINEINDEX, p, 0);
    int iq = (int)SendMessageW(h, EM_LINEINDEX, q, 0);
    int lp = (int)SendMessageW(h, EM_LINELENGTH, ip, 0);
    int lq = (int)SendMessageW(h, EM_LINELENGTH, iq, 0);
    if (lp < 0 || lq < 0 || lp + lq > 60000) return;

    wchar_t *A = (wchar_t *)malloc((lp + 2) * sizeof(wchar_t));
    wchar_t *B = (wchar_t *)malloc((lq + 2) * sizeof(wchar_t));
    wchar_t *ins = (wchar_t *)malloc((lp + lq + 8) * sizeof(wchar_t));
    if (!A || !B || !ins) { free(A); free(B); free(ins); return; }
    EditGetLine(h, p, A, lp + 1);
    EditGetLine(h, q, B, lq + 1);

    int ol = 0;
    memcpy(ins, B, (size_t)lq * sizeof(wchar_t)); ol += lq;
    ins[ol++] = L'\r'; ins[ol++] = L'\n';
    memcpy(ins + ol, A, (size_t)lp * sizeof(wchar_t)); ol += lp;
    ins[ol] = 0;
    SendMessageW(h, EM_SETSEL, ip, iq + lq);
    SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)ins);
    /* caret follows the moved line */
    int nl = dir > 0 ? q : p;
    int nls = (int)SendMessageW(h, EM_LINEINDEX, nl, 0);
    SendMessageW(h, EM_SETSEL, nls, nls);
    free(A);
    free(B);
    free(ins);
}

/* character at absolute caret position (works for EDIT and RichEdit);
 * returns 0 when out of range or on a line break */
static wchar_t EditCharAt(HWND h, int pos)
{
    if (pos < 0) return 0;
    int li = (int)SendMessageW(h, EM_LINEFROMCHAR, pos, 0);
    int ls = (int)SendMessageW(h, EM_LINEINDEX, li, 0);
    int off = pos - ls;
    if (off < 0) return 0;
    wchar_t buf[1024];
    *(LPWORD)buf = (WORD)(sizeof(buf) / sizeof(wchar_t) - 1);
    int got = (int)SendMessageW(h, EM_GETLINE, li, (LPARAM)buf);
    if (got > 0) buf[got] = 0; else buf[0] = 0;
    if (off >= lstrlenW(buf)) return 0;   /* caret sits at line end */
    return buf[off];
}

LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEWHEEL && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(g_hwnd, WM_EDITCMD, IDM_ZOOM,
                     ((short)HIWORD(wp) > 0) ? 1 : -1);
        return 0;
    }

    /* Backspace between an auto-inserted pair removes both chars */
    if (msg == WM_KEYDOWN && wp == VK_BACK
        && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        DWORD s0 = 0, e0 = 0;
        SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
        if (s0 == e0 && s0 > 0) {
            wchar_t prev = EditCharAt(h, (int)s0 - 1);
            wchar_t next = EditCharAt(h, (int)s0);
            BOOL pair = (prev == L'[' && next == L']')
                     || (prev == L'(' && next == L')')
                     || (prev == L'{' && next == L'}');
            if (pair) {
                SendMessageW(h, EM_SETSEL, s0 - 1, s0 + 1);
                SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"");
                SendMessageW(h, EM_SETSEL, s0 - 1, s0 - 1);
                WikiCompleteCheck(h);
                SlashCompleteCheck(h);
                return 0;
            }
        }
    }

    /* [[ autocomplete: arrows / Enter / Tab / Esc drive the popup */
    if (msg == WM_KEYDOWN && WikiMenuActive()) {
        BOOL eaten = 0;
        WikiCompleteKey(h, (UINT)wp, &eaten);
        if (eaten) return 0;
    }

    /* slash commands: same keyboard flow, wins only when its popup
     * is the live one (they never coexist) */
    if (msg == WM_KEYDOWN && SlashMenuActive()) {
        BOOL eaten = 0;
        SlashCompleteKey(h, (UINT)wp, &eaten);
        if (eaten) return 0;
    }

    /* any caret move outside a "[[" context closes the popup */
    if (msg == WM_KEYDOWN && (wp == VK_LEFT || wp == VK_RIGHT
                              || wp == VK_HOME || wp == VK_END)) {
        LRESULT r = CallWindowProcW(g_editProc, h, msg, wp, lp);
        WikiCompleteCheck(h);
        SlashCompleteCheck(h);
        return r;
    }
    if (msg == WM_KILLFOCUS) {
        HideWikiMenu();
        HideSlashMenu();
    }

    if (msg == WM_KEYDOWN) {
        BOOL ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        BOOL shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl && wp == 'S') {
            SendMessageW(g_hwnd, WM_EDITCMD,
                         shift ? IDM_SAVEAS : IDM_SAVE, 0);
            return 0;
        }
        if (ctrl && wp == 'O') {
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_OPEN, 0);
            return 0;
        }
        if (ctrl && wp == 'B') {    /* toggle file tree sidebar */
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_TREEBAR, 0);
            return 0;
        }
        if (ctrl && shift && wp == 'L') {   /* backlinks & orphans panel */
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_LINKS, 0);
            return 0;
        }
        if (ctrl && wp == 'N') {
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_NEW, 0);
            return 0;
        }
        if (ctrl && wp == 'F') {
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_FIND, 0);
            return 0;
        }
        if (ctrl && wp == 'P') {
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_OUTLINE, 0);
            return 0;
        }
        if (ctrl && wp == 'G') {    /* knowledge graph view */
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_GRAPH, 0);
            return 0;
        }
        if (ctrl && wp == 'J' && !g_aiBusy && !g_agBusy) {
            ShowSelAiMenu();
            return 0;
        }
        if (ctrl && wp == VK_OEM_2) { /* Ctrl+/ */
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_TOGGLE, 0);
            return 0;
        }
        if (ctrl && wp == VK_OEM_COMMA) { /* Ctrl+, settings */
            SendMessageW(g_hwnd, WM_EDITCMD, IDM_SETTINGS, 0);
            return 0;
        }
        if (ctrl && wp == 'A') {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        if (ctrl && wp == 'L') {            /* select current line */
            DWORD s0 = 0, e0 = 0;
            SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
            int li = (int)SendMessageW(h, EM_LINEFROMCHAR, s0, 0);
            int ls = (int)SendMessageW(h, EM_LINEINDEX, li, 0);
            int llen = (int)SendMessageW(h, EM_LINELENGTH, ls, 0);
            SendMessageW(h, EM_SETSEL, ls, ls + llen);
            return 0;
        }
        if (ctrl && (wp == 'D')) {          /* duplicate line */
            EditDupLine(h);
            return 0;
        }
        if (GetKeyState(VK_MENU) & 0x8000
            && (wp == VK_UP || wp == VK_DOWN)) {   /* move line */
            if (shift) {
                EditMoveLine(h, wp == VK_UP ? -1 : 1);
                return 0;
            }
        }
        if ((ctrl && wp == 'Y')
            || (ctrl && shift && wp == 'Z')) {  /* redo (richedit aware) */
            SendMessageW(h, EM_REDO, 0, 0);
            return 0;
        }
        if (wp == VK_TAB && !ctrl) {
            DWORD s0 = 0, e0 = 0;
            SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
            if (s0 != e0) {                 /* selection: indent block */
                EditIndentSel(h, !shift);
                return 0;
            }
            return 0;                       /* single tab handled in WM_CHAR */
        }
        if (wp == VK_TAB) return 0; /* handled in WM_CHAR */
        if (ctrl && wp == 'C') { SendMessageW(h, WM_COPY, 0, 0); return 0; }
        if (ctrl && wp == 'X') { SendMessageW(h, WM_CUT, 0, 0);  return 0; }
        if (ctrl && wp == 'V') { SendMessageW(h, WM_PASTE, 0, 0); return 0; }
        if (wp == VK_TAB) return 0; /* handled in WM_CHAR */
        if (wp == VK_ESCAPE && (g_aiBusy || g_agBusy)) {
            if (g_aiBusy) AiCancel();
            if (g_agBusy) AgCancel();
            return 0;
        }
        if (wp == VK_RETURN && !ctrl) {
            /* "/q" -> AI chat, "//q" -> agent; both run in background
             * and insert their answer right under the question line */
            DWORD s0, e0;
            SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
            if (s0 == e0) {
                int li = (int)SendMessageW(h, EM_LINEFROMCHAR, s0, 0);
                int ls = (int)SendMessageW(h, EM_LINEINDEX, li, 0);
                int ll = (int)SendMessageW(h, EM_LINELENGTH, ls, 0);
                if (ll >= 2 && ll < 1024) {
                    wchar_t buf[1026];
                    buf[0] = 1024;
                    int n = (int)SendMessageW(h, EM_GETLINE, li,
                                              (LPARAM)buf);
                    if (n > 0) buf[n] = 0; else buf[0] = 0;
                    if (buf[0] == L'/' && buf[1] == L'/' && buf[2] == L'/') {
                        /* agent command: ///task */
                        if (!buf[3]) return 0;
                        if (g_agBusy) {
                            MessageBoxW(g_hwnd,
                                L"Agent 任务进行中，按 Esc 可取消。",
                                APP_NAME, MB_ICONINFORMATION);
                            return 0;
                        }
                        StartAgent(buf + 3);
                        return 0;
                    }
                    if (buf[0] == L'/' && buf[1] == L'/') {
                        /* AI question: //question */
                        if (!g_aiBase[0] || !g_aiModel[0]) {
                            MessageBoxW(g_hwnd,
                                L"未配置 AI：请先在「配置」中填写"
                                L" Base URL 与模型。",
                                APP_NAME, MB_ICONINFORMATION);
                            return 0;
                        }
                        if (!buf[2]) return 0;
                        if (g_aiBusy) {
                            MessageBoxW(g_hwnd,
                                L"AI 任务进行中，按 Esc 可取消。",
                                APP_NAME, MB_ICONINFORMATION);
                            return 0;
                        }
                        StartAi(buf + 2);
                        return 0; /* we manage the newline ourselves */
                    }
                }
            }
        }
    }
    else if (msg == WM_CHAR) {
        /* @ opens the markdown snippet insert menu */
        if (wp == L'@' && !(GetKeyState(VK_CONTROL) & 0x8000)
            && !(GetKeyState(VK_MENU) & 0x8000)) {
            ShowInsertMenu();
            return 0;
        }
        /* auto-close brackets: [ ( { — Obsidian/VSCode style */
        if (wp == L'[' || wp == L'(' || wp == L'{') {
            DWORD s0 = 0, e0 = 0;
            SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
            wchar_t open = (wchar_t)wp;
            wchar_t close = (wp == L'[') ? L']'
                          : (wp == L'(') ? L')' : L'}';
            if (s0 != e0) {
                /* wrap the selection: open + sel + close */
                int len = (int)(e0 - s0);
                wchar_t *sel = (wchar_t *)malloc(
                    (len + 3) * sizeof(wchar_t));
                if (sel) {
                    SendMessageW(h, EM_GETSELTEXT, 0, (LPARAM)(sel + 1));
                    sel[0] = open;
                    sel[len + 1] = close;
                    sel[len + 2] = 0;
                    SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)sel);
                    /* keep the wrapped text selected */
                    SendMessageW(h, EM_SETSEL, s0 + 1, s0 + 1 + len);
                    free(sel);
                    return 0;
                }
            } else {
                /* insert the pair, caret between them */
                wchar_t pair[3] = { open, close, 0 };
                SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)pair);
                SendMessageW(h, EM_SETSEL, s0 + 1, s0 + 1);
                /* typing the second '[' opens the wiki autocomplete */
                WikiCompleteCheck(h);
                SlashCompleteCheck(h);
                return 0;
            }
        }
        /* type-over: typing a closing bracket when the next char is
         * already that bracket just steps the caret over it */
        if (wp == L']' || wp == L')' || wp == L'}') {
            DWORD s0 = 0, e0 = 0;
            SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
            if (s0 == e0) {
                wchar_t next = EditCharAt(h, (int)s0);
                if (next == (wchar_t)wp) {
                    SendMessageW(h, EM_SETSEL, s0 + 1, s0 + 1);
                    WikiCompleteCheck(h);
                    SlashCompleteCheck(h);
                    return 0;   /* skip over the existing bracket */
                }
            }
        }
        if (wp == VK_TAB && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"    ");
            return 0;
        }
        /* Enter: inherit leading blanks from the current line */
        if (wp == L'\r' && g_indentRet
            && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            DWORD s0, e0;
            SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&e0);
            int li = (int)SendMessageW(h, EM_LINEFROMCHAR, s0, 0);
            int ls = (int)SendMessageW(h, EM_LINEINDEX, li, 0);
            int llen = (int)SendMessageW(h, EM_LINELENGTH, s0, 0);
            wchar_t lbuf[512];
            *(LPWORD)lbuf = (WORD)(sizeof(lbuf) / sizeof(wchar_t));
            int got = (int)SendMessageW(h, EM_GETLINE, li,
                                        (LPARAM)lbuf);
            if (got > 0) lbuf[got] = 0; else lbuf[0] = 0;
            int pre = 0;
            while (lbuf[pre] == L' ' || lbuf[pre] == L'\t') pre++;
            if (pre > 0 && pre >= got) {
                /* blank indented line: clear it, raw newline */
                SendMessageW(h, EM_SETSEL, ls, ls + llen);
                SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"\r\n");
                SendMessageW(h, EM_SETSEL, ls + 2, ls + 2);
                return 0;
            }
            /* list item? continue the marker (or exit on empty item) */
            {
                wchar_t cont[112];
                int exitList = 0;
                int clen = ListContinuation(lbuf, got, cont, 112,
                                            &exitList);
                if (clen > 0) {
                    if (exitList) {
                        SendMessageW(h, EM_SETSEL, ls, ls + llen);
                        SendMessageW(h, EM_REPLACESEL, TRUE,
                                     (LPARAM)L"\r\n");
                        SendMessageW(h, EM_SETSEL, ls + 2, ls + 2);
                    } else {
                        SendMessageW(h, EM_REPLACESEL, TRUE,
                                     (LPARAM)cont);
                    }
                    return 0;
                }
            }
            if (pre > 0 && pre < 64) {
                wchar_t ins[80];
                ins[0] = L'\r'; ins[1] = L'\n';
                for (int k = 0; k < pre; k++)
                    ins[2 + k] = lbuf[k];
                ins[2 + pre] = 0;
                SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)ins);
                return 0;
            }
            /* fall through: default newline */
        }
        /* swallow control chars except backspace, enter, Ctrl+Z (undo) */
        if (wp < 32 && wp != VK_BACK && wp != '\r' && wp != '\n'
            && wp != VK_TAB && wp != 26)
            return 0;
        /* printable input (and backspace): let it land, then
         * re-evaluate the [[ and / autocomplete contexts */
        LRESULT r = CallWindowProcW(g_editProc, h, msg, wp, lp);
        WikiCompleteCheck(h);
        SlashCompleteCheck(h);
        return r;
    }
    return CallWindowProcW(g_editProc, h, msg, wp, lp);
}
/* ---- main-thread message handlers (invoked from WndProc) ---- */

void AiOnChunk(const wchar_t *txt, int n)
{
    if (g_aiBusy && txt && n > 0) {
        if (g_aiPendingLen + n + 1 > g_aiPendingCap) {
            int cap = g_aiPendingCap ? g_aiPendingCap : 4096;
            while (cap < g_aiPendingLen + n + 1) cap *= 2;
            wchar_t *nb = (wchar_t *)realloc(g_aiPending,
                                             cap * sizeof(wchar_t));
            if (nb) { g_aiPending = nb; g_aiPendingCap = cap; }
        }
        if (g_aiPending && g_aiPendingLen + n < g_aiPendingCap) {
            memcpy(g_aiPending + g_aiPendingLen, txt, n * sizeof(wchar_t));
            g_aiPendingLen += n;
            g_aiPending[g_aiPendingLen] = 0;
        }
        /* batch for 70ms: enough to merge multi-token bursts,
         * short enough to feel like live streaming */
        SetTimer(g_hwnd, TIMER_AICHUNK, 70, NULL);
        /* accumulate for multi-turn context (capped) */
        if (g_aiLastA && g_aiLastALen < 2400) {
            int room = 2400 - g_aiLastALen;
            int cp = n < room ? n : room;
            memcpy(g_aiLastA + g_aiLastALen, txt, cp * sizeof(wchar_t));
            g_aiLastALen += cp;
            g_aiLastA[g_aiLastALen] = 0;
        }
    }
    free((void *)txt);
}

void AiOnDone(const wchar_t *err)
{
    g_aiBusy = FALSE;
    KillTimer(g_hwnd, TIMER_AICHUNK);
    AiFlushPending();
    if (g_selAi) {
        SelAiFinish(err);
    } else {
        /* successful turns become context for the next question */
        if ((!err || !err[0]) && g_aiLastQ[0] && g_aiLastA && g_aiLastA[0])
            StoreAiTurn(g_aiLastQ, g_aiLastA);
        TaskFinish(0, err);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
    free((void *)err);
}

void AgOnChunk(const wchar_t *txt, int n)
{
    if (g_agBusy && txt && n > 0) {
        if (g_agPendingLen + n + 1 > g_agPendingCap) {
            int cap = g_agPendingCap ? g_agPendingCap : 4096;
            while (cap < g_agPendingLen + n + 1) cap *= 2;
            wchar_t *nb = (wchar_t *)realloc(g_agPending,
                                             cap * sizeof(wchar_t));
            if (nb) { g_agPending = nb; g_agPendingCap = cap; }
        }
        if (g_agPending && g_agPendingLen + n < g_agPendingCap) {
            memcpy(g_agPending + g_agPendingLen, txt, n * sizeof(wchar_t));
            g_agPendingLen += n;
            g_agPending[g_agPendingLen] = 0;
        }
        SetTimer(g_hwnd, TIMER_AICHUNK, 70, NULL);
    }
    free((void *)txt);
}

void AgOnDone(const wchar_t *err)
{
    g_agBusy = FALSE;
    KillTimer(g_hwnd, TIMER_AICHUNK);
    AgFlushPending();
    TaskFinish(1, err);
    InvalidateRect(g_hwnd, NULL, FALSE);
    free((void *)err);
}
