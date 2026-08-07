#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

typedef struct {
    const char *host;
    const char *cmd;
    int rport;
    int lport;
    int listen;
    int numeric;
    int timeout;
} Opt;

typedef struct {
    SOCKET s;
    HANDLE h;
} IoCtx;

static SOCKET g_sock = INVALID_SOCKET;
static HANDLE g_in_w = NULL;
static HANDLE g_out_r = NULL;

static int port_ok(const char *v, int *out) {
    char *e = NULL;
    long n;
    if (!v || !*v) return 0;
    n = strtol(v, &e, 10);
    if (!e || *e || n < 1 || n > 65535) return 0;
    *out = (int)n;
    return 1;
}

static int num_ok(const char *v, int *out) {
    char *e = NULL;
    long n;
    if (!v || !*v) return 0;
    n = strtol(v, &e, 10);
    if (!e || *e || n < 1 || n > 86400) return 0;
    *out = (int)n;
    return 1;
}

static int parse(int argc, char **argv, Opt *o) {
    const char *pos[2] = {0, 0};
    int pc = 0, i;
    memset(o, 0, sizeof(*o));

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-l")) { o->listen = 1; continue; }
        if (!strcmp(argv[i], "-n")) { o->numeric = 1; continue; }
        if (!strcmp(argv[i], "-v")) { continue; }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) return 0;
        if (!strcmp(argv[i], "-w")) {
            if (++i >= argc || !num_ok(argv[i], &o->timeout)) return 0;
            continue;
        }
        if (!strcmp(argv[i], "-p")) {
            if (++i >= argc || !port_ok(argv[i], &o->lport)) return 0;
            continue;
        }
        if (!strcmp(argv[i], "-c")) {
            if (++i >= argc || !*argv[i]) return 0;
            o->cmd = argv[i];
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1]) return 0;
        if (pc >= 2) return 0;
        pos[pc++] = argv[i];
    }

    if (o->listen) {
        if (pc == 1) {
            if (o->lport || !port_ok(pos[0], &o->lport)) return 0;
        } else if (pc != 0) {
            return 0;
        }
        return o->lport > 0;
    }

    if (pc != 2 || !port_ok(pos[1], &o->rport)) return 0;
    o->host = pos[0];
    return 1;
}

static int send_all(SOCKET s, const char *p, int n) {
    int off = 0;
    while (off < n) {
        int r = send(s, p + off, n - off, 0);
        if (r <= 0) return 0;
        off += r;
    }
    return 1;
}

static int write_all(HANDLE h, const char *p, DWORD n) {
    DWORD off = 0;
    while (off < n) {
        DWORD w = 0;
        if (!WriteFile(h, p + off, n - off, &w, NULL) || !w) return 0;
        off += w;
    }
    return 1;
}

static char *dup_s(const char *s) {
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int utf8_prefix(const unsigned char *s, int n, int *valid) {
    int i = 0;
    *valid = 1;
    while (i < n) {
        int need = 0, j;
        unsigned char c = s[i];
        if (c < 0x80) { ++i; continue; }
        if (c >= 0xC2 && c <= 0xDF) need = 2;
        else if (c >= 0xE0 && c <= 0xEF) need = 3;
        else if (c >= 0xF0 && c <= 0xF4) need = 4;
        else { *valid = 0; return n; }
        if (i + need > n) return i;
        for (j = 1; j < need; ++j) {
            if ((s[i + j] & 0xC0) != 0x80) { *valid = 0; return n; }
        }
        if (need == 3) {
            if ((c == 0xE0 && s[i + 1] < 0xA0) || (c == 0xED && s[i + 1] >= 0xA0)) { *valid = 0; return n; }
        } else if (need == 4) {
            if ((c == 0xF0 && s[i + 1] < 0x90) || (c == 0xF4 && s[i + 1] >= 0x90)) { *valid = 0; return n; }
        }
        i += need;
    }
    return n;
}

static int has_hi(const char *p, int n) {
    int i;
    for (i = 0; i < n; ++i) if (((unsigned char)p[i]) & 0x80) return 1;
    return 0;
}

static void console_write_wide(const wchar_t *w, int wn) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0, done = 0;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        WriteConsoleW(out, w, (DWORD)wn, &done, NULL);
    } else {
        int n = WideCharToMultiByte(CP_UTF8, 0, w, wn, NULL, 0, NULL, NULL);
        char *b = n > 0 ? (char *)malloc(n) : NULL;
        if (b) {
            WideCharToMultiByte(CP_UTF8, 0, w, wn, b, n, NULL, NULL);
            WriteFile(out, b, (DWORD)n, &done, NULL);
            free(b);
        }
    }
}

static void console_write_cp(UINT cp, const char *p, int n) {
    int wn;
    wchar_t *w;
    if (n <= 0) return;
    wn = MultiByteToWideChar(cp, 0, p, n, NULL, 0);
    if (wn <= 0) {
        DWORD d = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), p, (DWORD)n, &d, NULL);
        return;
    }
    w = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
    if (!w) return;
    MultiByteToWideChar(cp, 0, p, n, w, wn);
    console_write_wide(w, wn);
    free(w);
}

static void console_write_utf8(const char *p, int n) {
    console_write_cp(CP_UTF8, p, n);
}

static int to_utf8(UINT cp, const char *p, int n, char **out, int *outn) {
    int wn, un;
    wchar_t *w;
    char *u;
    *out = NULL;
    *outn = 0;
    if (n <= 0) return 0;
    wn = MultiByteToWideChar(cp, 0, p, n, NULL, 0);
    if (wn <= 0) return 0;
    w = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
    if (!w) return 0;
    MultiByteToWideChar(cp, 0, p, n, w, wn);
    un = WideCharToMultiByte(CP_UTF8, 0, w, wn, NULL, 0, NULL, NULL);
    if (un <= 0) { free(w); return 0; }
    u = (char *)malloc(un);
    if (!u) { free(w); return 0; }
    WideCharToMultiByte(CP_UTF8, 0, w, wn, u, un, NULL, NULL);
    free(w);
    *out = u;
    *outn = un;
    return 1;
}

static int utf8_to_cp(UINT cp, const char *p, int n, char **out, int *outn) {
    int wn, an;
    wchar_t *w;
    char *a;
    *out = NULL;
    *outn = 0;
    if (n <= 0) return 0;
    wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, n, NULL, 0);
    if (wn <= 0) return 0;
    w = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
    if (!w) return 0;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, n, w, wn);
    an = WideCharToMultiByte(cp, 0, w, wn, NULL, 0, NULL, NULL);
    if (an <= 0) { free(w); return 0; }
    a = (char *)malloc(an);
    if (!a) { free(w); return 0; }
    WideCharToMultiByte(cp, 0, w, wn, a, an, NULL, NULL);
    free(w);
    *out = a;
    *outn = an;
    return 1;
}

static int resolve4(const Opt *o, struct sockaddr_in *a) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons((u_short)o->rport);
    if (o->numeric) return InetPtonA(AF_INET, o->host, &a->sin_addr) == 1;
    {
        struct addrinfo h, *r = NULL;
        char ps[16];
        int ok;
        memset(&h, 0, sizeof(h));
        h.ai_family = AF_INET;
        h.ai_socktype = SOCK_STREAM;
        h.ai_protocol = IPPROTO_TCP;
        snprintf(ps, sizeof(ps), "%d", o->rport);
        if (getaddrinfo(o->host, ps, &h, &r) || !r) return 0;
        memcpy(a, r->ai_addr, sizeof(*a));
        freeaddrinfo(r);
        ok = 1;
        return ok;
    }
}

static int wait_connect(SOCKET s, const struct sockaddr_in *a, int sec) {
    u_long nb = 1, bl = 0;
    int r;
    if (sec <= 0) return connect(s, (const struct sockaddr *)a, sizeof(*a));
    if (ioctlsocket(s, FIONBIO, &nb)) return SOCKET_ERROR;
    r = connect(s, (const struct sockaddr *)a, sizeof(*a));
    if (r == SOCKET_ERROR) {
        int e = WSAGetLastError();
        fd_set wf, ef;
        struct timeval tv;
        int so = 0, sl = sizeof(so);
        if (e != WSAEWOULDBLOCK && e != WSAEINPROGRESS && e != WSAEINVAL) {
            ioctlsocket(s, FIONBIO, &bl);
            return SOCKET_ERROR;
        }
        FD_ZERO(&wf); FD_ZERO(&ef);
        FD_SET(s, &wf); FD_SET(s, &ef);
        tv.tv_sec = sec; tv.tv_usec = 0;
        r = select(0, NULL, &wf, &ef, &tv);
        if (r <= 0) {
            ioctlsocket(s, FIONBIO, &bl);
            if (!r) WSASetLastError(WSAETIMEDOUT);
            return SOCKET_ERROR;
        }
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so, &sl) || so) {
            ioctlsocket(s, FIONBIO, &bl);
            if (so) WSASetLastError(so);
            return SOCKET_ERROR;
        }
    }
    ioctlsocket(s, FIONBIO, &bl);
    return 0;
}

static SOCKET dial(const Opt *o) {
    struct sockaddr_in a, l;
    SOCKET s;
    if (!resolve4(o, &a)) return INVALID_SOCKET;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    if (o->lport > 0) {
        memset(&l, 0, sizeof(l));
        l.sin_family = AF_INET;
        l.sin_addr.s_addr = htonl(INADDR_ANY);
        l.sin_port = htons((u_short)o->lport);
        if (bind(s, (struct sockaddr *)&l, sizeof(l)) == SOCKET_ERROR) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }
    if (wait_connect(s, &a, o->timeout) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static SOCKET listen_one(const Opt *o) {
    SOCKET ls, cs;
    struct sockaddr_in a, p;
    int yes = 1, pl = sizeof(p);
    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return ls;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((u_short)o->lport);
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) == SOCKET_ERROR || listen(ls, 5) == SOCKET_ERROR) {
        closesocket(ls);
        return INVALID_SOCKET;
    }
    if (o->timeout > 0) {
        fd_set rf;
        struct timeval tv;
        int r;
        FD_ZERO(&rf); FD_SET(ls, &rf);
        tv.tv_sec = o->timeout; tv.tv_usec = 0;
        r = select(0, &rf, NULL, NULL, &tv);
        if (r <= 0) {
            closesocket(ls);
            if (!r) WSASetLastError(WSAETIMEDOUT);
            return INVALID_SOCKET;
        }
    }
    cs = accept(ls, (struct sockaddr *)&p, &pl);
    closesocket(ls);
    return cs;
}

static DWORD WINAPI pipe_to_sock(LPVOID p) {
    char b[2048];
    DWORD n;
    (void)p;
    while (ReadFile(g_out_r, b, sizeof(b), &n, NULL) && n) {
        char *u = NULL;
        int un = 0;
        if (to_utf8(CP_OEMCP, b, (int)n, &u, &un) || to_utf8(CP_ACP, b, (int)n, &u, &un)) {
            if (!send_all(g_sock, u, un)) { free(u); break; }
            free(u);
        } else if (!send_all(g_sock, b, (int)n)) {
            break;
        }
    }
    shutdown(g_sock, SD_SEND);
    return 0;
}

static int pipe_write_normalized(const char *p, int n, int *prev_cr) {
    char out[4096];
    int i, o = 0;
    for (i = 0; i < n; ++i) {
        char c = p[i];
        if (c == '\n' && !*prev_cr) out[o++] = '\r';
        out[o++] = c;
        *prev_cr = (c == '\r');
        if (o > (int)sizeof(out) - 8) {
            if (!write_all(g_in_w, out, (DWORD)o)) return 0;
            o = 0;
        }
    }
    return o ? write_all(g_in_w, out, (DWORD)o) : 1;
}

static DWORD WINAPI sock_to_pipe(LPVOID p) {
    char b[2048];
    int n, prev = 0;
    (void)p;
    while ((n = recv(g_sock, b, sizeof(b), 0)) > 0) {
        int v = 0, pref = utf8_prefix((unsigned char *)b, n, &v);
        char *a = NULL;
        int an = 0;
        if (v && pref == n && has_hi(b, n) && utf8_to_cp(CP_OEMCP, b, n, &a, &an)) {
            if (!pipe_write_normalized(a, an, &prev)) { free(a); break; }
            free(a);
        } else if (!pipe_write_normalized(b, n, &prev)) {
            break;
        }
    }
    return 0;
}

static DWORD WINAPI sock_to_console(LPVOID p) {
    SOCKET s = ((IoCtx *)p)->s;
    char b[2048], mix[2052], tail[4];
    int n, tn = 0;
    while ((n = recv(s, b, sizeof(b), 0)) > 0) {
        int total, ok, pref;
        if (tn) memcpy(mix, tail, tn);
        memcpy(mix + tn, b, n);
        total = tn + n;
        pref = utf8_prefix((unsigned char *)mix, total, &ok);
        if (ok) {
            if (pref > 0) console_write_utf8(mix, pref);
            tn = total - pref;
            if (tn > 0 && tn <= 4) memcpy(tail, mix + pref, tn);
            else tn = 0;
        } else {
            console_write_cp(CP_OEMCP, mix, total);
            tn = 0;
        }
    }
    if (tn) console_write_cp(CP_OEMCP, tail, tn);
    return 0;
}

static DWORD WINAPI console_to_sock(LPVOID p) {
    SOCKET s = ((IoCtx *)p)->s;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode, raw;
    if (in == INVALID_HANDLE_VALUE || in == NULL) return 0;
    if (GetConsoleMode(in, &mode)) {
        INPUT_RECORD rec[32];
        DWORD cnt, i;
        raw = (mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) | ENABLE_PROCESSED_INPUT;
        SetConsoleMode(in, raw);
        while (ReadConsoleInputW(in, rec, 32, &cnt) && cnt) {
            for (i = 0; i < cnt; ++i) {
                KEY_EVENT_RECORD *k;
                char out[8];
                int n;
                if (rec[i].EventType != KEY_EVENT || !rec[i].Event.KeyEvent.bKeyDown) continue;
                k = &rec[i].Event.KeyEvent;
                if (k->wVirtualKeyCode == VK_RETURN) {
                    if (!send_all(s, "\r\n", 2)) goto done;
                } else if (k->wVirtualKeyCode == VK_BACK) {
                    if (!send_all(s, "\b", 1)) goto done;
                } else if (k->uChar.UnicodeChar) {
                    n = WideCharToMultiByte(CP_UTF8, 0, &k->uChar.UnicodeChar, 1, out, sizeof(out), NULL, NULL);
                    if (n > 0 && !send_all(s, out, n)) goto done;
                }
            }
        }
done:
        SetConsoleMode(in, mode);
    } else {
        char b[1024];
        DWORD n;
        while (ReadFile(in, b, sizeof(b), &n, NULL) && n) {
            if (!send_all(s, b, (int)n)) break;
        }
    }
    shutdown(s, SD_SEND);
    return 0;
}

static int relay(SOCKET s) {
    IoCtx c;
    HANDLE a, b;
    c.s = s;
    c.h = NULL;
    a = CreateThread(NULL, 0, sock_to_console, &c, 0, NULL);
    b = CreateThread(NULL, 0, console_to_sock, &c, 0, NULL);
    if (a) WaitForSingleObject(a, INFINITE);
    shutdown(s, SD_BOTH);
    if (b) WaitForSingleObject(b, 1000);
    if (a) CloseHandle(a);
    if (b) CloseHandle(b);
    return 0;
}

static int run_cmd(const Opt *o, SOCKET s) {
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE out_w = NULL, in_r = NULL, t1 = NULL, t2 = NULL;
    char *cl = dup_s(o->cmd ? o->cmd : "cmd.exe /Q");
    if (!cl) return 1;
    g_sock = s;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&g_out_r, &out_w, &sa, 0)) { free(cl); return 1; }
    SetHandleInformation(g_out_r, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&in_r, &g_in_w, &sa, 0)) {
        CloseHandle(g_out_r); CloseHandle(out_w); free(cl); return 1;
    }
    SetHandleInformation(g_in_w, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    if (!CreateProcessA(NULL, cl, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(g_out_r); CloseHandle(out_w); CloseHandle(in_r); CloseHandle(g_in_w); free(cl); return 1;
    }
    free(cl);
    CloseHandle(out_w);
    CloseHandle(in_r);
    t1 = CreateThread(NULL, 0, pipe_to_sock, NULL, 0, NULL);
    t2 = CreateThread(NULL, 0, sock_to_pipe, NULL, 0, NULL);
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (t1) CloseHandle(t1);
    if (t2) CloseHandle(t2);
    CloseHandle(g_in_w);
    CloseHandle(g_out_r);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}

int main(int argc, char **argv) {
    WSADATA w;
    Opt o;
    SOCKET s;
    int rc;
    if (!parse(argc, argv, &o)) return 1;
    if (WSAStartup(MAKEWORD(2, 2), &w)) return 1;
    s = o.listen ? listen_one(&o) : dial(&o);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }
    rc = (o.listen && !o.cmd) ? relay(s) : run_cmd(&o, s);
    closesocket(s);
    WSACleanup();
    return rc;
}
