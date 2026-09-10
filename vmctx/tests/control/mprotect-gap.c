/* SPDX-License-Identifier: GPL-2.0 */
/* Linux can change the first VMA before mprotect encounters a later hole.
 * A failed syscall must still leave the guest with the actual permissions.
 * cc -O2 -Wall -Wextra -static mprotect-gap.c -o mprotect-gap
 */
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf checkpoint;
static volatile sig_atomic_t caught;
static volatile unsigned char *mapping;

static void denied(int sig, siginfo_t *info, void *context)
{
    (void)context;
    if (sig != SIGSEGV || info->si_code != SEGV_ACCERR ||
        info->si_addr != (void *)mapping)
        _exit(92);
    caught = 1;
    siglongjmp(checkpoint, 1);
}

int main(void)
{
    long page = sysconf(_SC_PAGESIZE);
    struct sigaction action = { .sa_sigaction = denied, .sa_flags = SA_SIGINFO };
    if (page <= 0 || sigemptyset(&action.sa_mask) || sigaction(SIGSEGV, &action, NULL))
        return 2;
    mapping = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return 2;
    mapping[0] = 41;
    mapping[2 * page] = 42;
    if (munmap((void *)(mapping + page), page)) return 2;
    errno = 0;
    int result = mprotect((void *)mapping, 3 * page, PROT_READ);
    if (result != -1 || errno != ENOMEM) {
        fprintf(stderr, "FAIL: mprotect across the hole returned %d errno=%d\n", result, errno);
        return 1;
    }
    if (!sigsetjmp(checkpoint, 1))
        mapping[0] = 99;
    printf("%s: failed mprotect still revokes write before the hole (fault=%d)\n",
           caught ? "PASS" : "FAIL", (int)caught);
    /* The mapping after the hole was never visited and stays writable. */
    mapping[2 * page] = 43;
    if (mapping[2 * page] != 43) return 1;
    if (munmap((void *)mapping, page) || munmap((void *)(mapping + 2 * page), page))
        return 2;
    return caught ? 0 : 1;
}
