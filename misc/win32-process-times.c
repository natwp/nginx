/*
 * Copyright (C) Nginx, Inc.
 */


#include <windows.h>
#include <stdio.h>
#include <stdlib.h>


static unsigned long long
filetime_value(FILETIME *value)
{
    ULARGE_INTEGER  ticks;

    ticks.LowPart = value->dwLowDateTime;
    ticks.HighPart = value->dwHighDateTime;

    return ticks.QuadPart;
}


int
main(int argc, char **argv)
{
    char                *end;
    int                  i;
    DWORD                pid;
    HANDLE               process;
    FILETIME             created, exited, kernel, user;
    unsigned long long   kernel_ticks, user_ticks;

    for (i = 1; i < argc; i++) {
        pid = strtoul(argv[i], &end, 10);

        if (*argv[i] == '\0' || *end != '\0' || pid == 0) {
            continue;
        }

        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == NULL) {
            continue;
        }

        if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
            kernel_ticks = filetime_value(&kernel);
            user_ticks = filetime_value(&user);

            printf("%lu %llu %llu\n", (unsigned long) pid, kernel_ticks,
                   user_ticks);
        }

        CloseHandle(process);
    }

    return 0;
}
