// SPDX-License-Identifier: GPL-2.0
/* Detached descendants used to test the native subreaper's cleanup. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    int ready[2];
    if (pipe(ready)) return 2;
    pid_t first = fork();
    if (first < 0) return 2;
    if (!first) {
        close(ready[0]);
        if (setsid() < 0) _exit(2);
        pid_t second = fork();
        if (second < 0) _exit(2);
        if (!second) {
            close(ready[1]);
            for (;;) pause();
        }
        char identities[80];
        int n = snprintf(identities, sizeof(identities), "%ld %ld\n", (long)getpid(), (long)second);
        if (write(ready[1], identities, n) != n) _exit(2);
        close(ready[1]);
        for (;;) pause();
    }
    close(ready[1]);
    char identities[80];
    ssize_t n = read(ready[0], identities, sizeof(identities));
    close(ready[0]);
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (n <= 0 || fd < 0 || write(fd, identities, n) != n || close(fd)) return 2;
    if (!strcmp(argv[2], "exit")) return 7;
    if (strcmp(argv[2], "wait")) return 2;
    for (;;) pause();
}
