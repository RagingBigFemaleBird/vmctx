// SPDX-License-Identifier: GPL-2.0
/* Run under the source loader policy. Repeat after exec to catch policies that
 * only edit the first image's auxv. --native runs the clock checks with vDSO
 * permitted; --prepare CTL_NR enables the source adapter before a native exec.
 * cc -O2 -Wall -Wextra -static source-clocks.c -o source-clocks
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t ns(struct timespec t) { return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static int check_clocks(void)
{
    const clockid_t clocks[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_REALTIME_COARSE,
                               CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME};
    for (unsigned i = 0; i < sizeof(clocks)/sizeof(clocks[0]); i++) {
        struct timespec a, b, c, later, pause = {.tv_nsec = 40000000};
        if (syscall(SYS_clock_gettime, clocks[i], &a) || clock_gettime(clocks[i], &b) ||
            syscall(SYS_clock_gettime, clocks[i], &c)) return 2;
        if (ns(a) > ns(b) || ns(b) > ns(c)) {
            fprintf(stderr, "FAIL: libc clock %d is outside source syscall readings\n", clocks[i]);
            return 1;
        }
        if (nanosleep(&pause, NULL) || clock_gettime(clocks[i], &later)) return 2;
        if (ns(later) <= ns(b)) {
            fprintf(stderr, "FAIL: clock %d did not advance\n", clocks[i]);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    alarm(15);
    if (argc == 3 && !strcmp(argv[1], "--prepare")) {
        char *end;
        long nr = strtol(argv[2], &end, 10);
        if (*end || nr <= 0) return 2;
        if (syscall(nr, getpid(), 32, NULL)) { perror("source exec policy"); return 2; }
        execl("/proc/self/exe", "source-clocks", NULL);
        return 2;
    }
    int native = argc == 2 && !strcmp(argv[1], "--native");
    int child = argc == 2 && !strcmp(argv[1], "--child");
    if (!native && getauxval(AT_SYSINFO_EHDR)) {
        fputs("FAIL: source image exposes a vDSO clock\n", stderr);
        return 1;
    }
    int r = check_clocks();
    if (r) return r;
    if (native) { puts("PASS: native clock readings and advancement"); return 0; }
    if (child) return 0;
    pid_t pid = fork();
    if (pid < 0) return 2;
    if (!pid) { execl("/proc/self/exe", "source-clocks", "--child", NULL); _exit(93); }
    int status;
    if (waitpid(pid, &status, 0) != pid) return 2;
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "FAIL: clocks after exec status=%x\n", status); return 1;
    }
    puts("PASS: source clocks advance and stay authoritative across exec");
    return 0;
}
