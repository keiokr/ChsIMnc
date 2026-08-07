#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

static SOCKET conn_fd = INVALID_SOCKET;

static int send_all(SOCKET s, const char *p, int n) {
    int off = 0;
    while (off < n) {
        int r = send(s, p + off, n - off, 0);
        if (r <= 0) return 0;
        off += r;
    }
    return 1;
}

static int port_ok(const char *v, int *out) {
    char *e = NULL;
    long n = strtol(v, &e, 10);
    if (!v || !*v || !e || *e || n < 1 || n > 65535) return 0;
    *out = (int)n;
    return 1;
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

static void write_wide(const wchar_t *w, int wn) {
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

static void write_cp(UINT cp, const char *p, int n) {
    int wn;
    wchar_t *w;
    if (n <= 0) return;
    wn = MultiByteToWideChar(cp, 0, p, n, NULL, 0);
    if (wn <= 0) return;
    w = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
    if (!w) return;
    MultiByteToWideChar(cp, 0, p, n, w, wn);
    write_wide(w, wn);
    free(w);
}

static DWORD WINAPI recv_thread(LPVOID arg) {
    char b[2048], mix[2052], tail[4];
    int n, tn = 0;
    (void)arg;
    while ((n = recv(conn_fd, b, sizeof(b), 0)) > 0) {
        int total, ok, pref;
        if (tn) memcpy(mix, tail, tn);
        memcpy(mix + tn, b, n);
        total = tn + n;
        pref = utf8_prefix((unsigned char *)mix, total, &ok);
        if (ok) {
            if (pref > 0) write_cp(CP_UTF8, mix, pref);
            tn = total - pref;
            if (tn > 0 && tn <= 4) memcpy(tail, mix + pref, tn);
            else tn = 0;
        } else {
            write_cp(CP_OEMCP, mix, total);
            tn = 0;
        }
    }
    if (tn) write_cp(CP_OEMCP, tail, tn);
    ExitProcess(0);
    return 0;
}

static void input_loop(void) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode, raw;
    if (in == INVALID_HANDLE_VALUE || in == NULL) return;
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
                    if (!send_all(conn_fd, "\r\n", 2)) goto done;
                } else if (k->wVirtualKeyCode == VK_BACK) {
                    if (!send_all(conn_fd, "\b", 1)) goto done;
                } else if (k->uChar.UnicodeChar) {
                    n = WideCharToMultiByte(CP_UTF8, 0, &k->uChar.UnicodeChar, 1, out, sizeof(out), NULL, NULL);
                    if (n > 0 && !send_all(conn_fd, out, n)) goto done;
                }
            }
        }
done:
        SetConsoleMode(in, mode);
    } else {
        char b[1024];
        DWORD n;
        while (ReadFile(in, b, sizeof(b), &n, NULL) && n) {
            if (!send_all(conn_fd, b, (int)n)) break;
        }
    }
    shutdown(conn_fd, SD_SEND);
}

int main(int argc, char **argv) {
    WSADATA w;
    SOCKET listen_fd;
    struct sockaddr_in a, c;
    int port = 1088, yes = 1, cl = sizeof(c);
    if (argc > 1 && !port_ok(argv[1], &port)) return 1;
    if (WSAStartup(MAKEWORD(2, 2), &w)) return 1;
    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd == INVALID_SOCKET) return 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((u_short)port);
    if (bind(listen_fd, (struct sockaddr *)&a, sizeof(a)) == SOCKET_ERROR || listen(listen_fd, 5) == SOCKET_ERROR) return 1;
    conn_fd = accept(listen_fd, (struct sockaddr *)&c, &cl);
    closesocket(listen_fd);
    if (conn_fd == INVALID_SOCKET) return 1;
    CreateThread(NULL, 0, recv_thread, NULL, 0, NULL);
    input_loop();
    closesocket(conn_fd);
    WSACleanup();
    return 0;
}
