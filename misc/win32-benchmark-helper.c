/* Loopback-only HTTP upstream and CPU counters for the CI benchmarks. */

#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static char   *ngx_bench_response[2];
static int     ngx_bench_length[2];
static DWORD   ngx_bench_delay;


static unsigned long long
ngx_bench_ticks(FILETIME value)
{
    return ((unsigned long long) value.dwHighDateTime << 32)
           | value.dwLowDateTime;
}


static int
ngx_bench_snapshot(int argc, char **argv)
{
    int       i, comma;
    DWORD     pid;
    HANDLE    process;
    FILETIME  idle, kernel, user, created, exited;

    if (!GetSystemTimes(&idle, &kernel, &user)) {
        return 1;
    }

    printf("{\"at\":%llu,\"idle\":%llu,\"kernel\":%llu,\"user\":%llu,"
           "\"processes\":{",
           (unsigned long long) GetTickCount64(), ngx_bench_ticks(idle),
           ngx_bench_ticks(kernel), ngx_bench_ticks(user));

    comma = 0;

    for (i = 2; i < argc; i++) {
        pid = (DWORD) strtoul(argv[i], NULL, 10);
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                              FALSE, pid);

        if (process == NULL) {
            continue;
        }

        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT
            && GetProcessTimes(process, &created, &exited, &kernel, &user))
        {
            printf("%s\"%lu\":{\"ticks\":%llu,\"created\":\"%lx-%lx\"}",
                   comma ? "," : "", (unsigned long) pid,
                   ngx_bench_ticks(kernel) + ngx_bench_ticks(user),
                   (unsigned long) created.dwHighDateTime,
                   (unsigned long) created.dwLowDateTime);
            comma = 1;
        }

        CloseHandle(process);
    }

    printf("}}\n");
    return 0;
}


static int
ngx_bench_terminate(char *pid_text, char *expected)
{
    char      identity[32];
    int       rc;
    HANDLE    process;
    FILETIME  created, exited, kernel, user;

    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE
                          | SYNCHRONIZE,
                          FALSE, (DWORD) strtoul(pid_text, NULL, 10));
    if (process == NULL) {
        return 0;
    }

    rc = 1;

    if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
        rc = 0;

    } else if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        sprintf(identity, "%lx-%lx", (unsigned long) created.dwHighDateTime,
                (unsigned long) created.dwLowDateTime);

        if (strcmp(identity, expected) != 0
            || TerminateProcess(process, 1)
            || WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
        {
            rc = 0;
        }
    }

    CloseHandle(process);
    return rc;
}


static int
ngx_bench_send(SOCKET fd, const char *data, int length)
{
    int  n;

    while (length > 0) {
        n = send(fd, data, length, 0);

        if (n <= 0) {
            return 0;
        }

        data += n;
        length -= n;
    }

    return 1;
}


static unsigned __stdcall
ngx_bench_serve(void *argument)
{
    char    request[8192], target[128], *end;
    int     used, n, size, slow, consumed;
    SOCKET  fd;

    fd = (SOCKET) (uintptr_t) argument;
    used = 0;

    for ( ;; ) {
        request[used] = '\0';
        end = strstr(request, "\r\n\r\n");

        if (end == NULL) {
            if (used == (int) sizeof(request) - 1) {
                break;
            }

            n = recv(fd, request + used, sizeof(request) - 1 - used, 0);

            if (n <= 0) {
                break;
            }

            used += n;
            continue;
        }

        consumed = (int) (end + 4 - request);

        if (sscanf(request, "GET %127s HTTP/", target) != 1) {
            break;
        }

        slow = strncmp(target, "/slow/", 6) == 0;
        size = -1;

        if (slow || strncmp(target, "/fast/", 6) == 0) {
            if (strcmp(target + 6, "1k.bin") == 0) {
                size = 0;

            } else if (strcmp(target + 6, "1m.bin") == 0) {
                size = 1;
            }
        }

        if (size < 0) {
            static const char missing[] =
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                "Connection: close\r\n\r\n";

            ngx_bench_send(fd, missing, sizeof(missing) - 1);
            break;
        }

        if (slow) {
            Sleep(ngx_bench_delay);
        }

        if (!ngx_bench_send(fd, ngx_bench_response[size],
                            ngx_bench_length[size]))
        {
            break;
        }

        used -= consumed;
        memmove(request, request + consumed, used);
    }

    closesocket(fd);
    return 0;
}


int
main(int argc, char **argv)
{
    int                 i, length, port, enabled;
    DWORD               timeout;
    SOCKET              listener, fd;
    WSADATA             wsadata;
    uintptr_t           thread;
    struct sockaddr_in  address;

    if (argc >= 2 && strcmp(argv[1], "snapshot") == 0) {
        return ngx_bench_snapshot(argc, argv);
    }

    if (argc == 4 && strcmp(argv[1], "terminate") == 0) {
        return ngx_bench_terminate(argv[2], argv[3]);
    }

    if (argc != 4 || strcmp(argv[1], "serve") != 0) {
        fprintf(stderr, "usage: helper.exe serve port delay-ms\n"
                "       helper.exe snapshot [pid ...]\n"
                "       helper.exe terminate pid creation-id\n");
        return 2;
    }

    port = atoi(argv[2]);
    ngx_bench_delay = (DWORD) atoi(argv[3]);

    if (port < 0 || port > 65535 || ngx_bench_delay > 1000) {
        return 2;
    }

    for (i = 0; i < 2; i++) {
        length = i ? 1048576 : 1024;
        ngx_bench_response[i] = malloc(length + 256);

        if (ngx_bench_response[i] == NULL) {
            return 1;
        }

        ngx_bench_length[i] = sprintf(ngx_bench_response[i],
            "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Connection: keep-alive\r\n\r\n", length);
        memset(ngx_bench_response[i] + ngx_bench_length[i], 'a', length);
        ngx_bench_length[i] += length;
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
        return 1;
    }

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (listener == INVALID_SOCKET) {
        return 1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((u_short) port);

    if (bind(listener, (struct sockaddr *) &address, sizeof(address))
        == SOCKET_ERROR
        || listen(listener, SOMAXCONN) == SOCKET_ERROR)
    {
        fprintf(stderr, "upstream bind/listen failed: %d\n", WSAGetLastError());
        return 1;
    }

    length = sizeof(address);

    if (getsockname(listener, (struct sockaddr *) &address, &length)
        == SOCKET_ERROR)
    {
        return 1;
    }

    printf("READY %u\n", (unsigned) ntohs(address.sin_port));
    fflush(stdout);
    timeout = 10000;
    enabled = 1;

    for ( ;; ) {
        fd = accept(listener, NULL, NULL);

        if (fd == INVALID_SOCKET) {
            return 1;
        }

        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                   (char *) &enabled, sizeof(enabled));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   (char *) &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   (char *) &timeout, sizeof(timeout));

        thread = _beginthreadex(NULL, 0, ngx_bench_serve,
                               (void *) (uintptr_t) fd, 0, NULL);

        if (thread == 0) {
            closesocket(fd);

        } else {
            CloseHandle((HANDLE) thread);
        }
    }
}
