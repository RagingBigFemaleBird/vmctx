/* SPDX-License-Identifier: GPL-2.0 */
/* A private RO mapping still owes a fork snapshot when its parent gains write
 * access. Keep the child off the page until after the parent's modification. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int check(const volatile unsigned char *page, unsigned char mask)
{
    for (unsigned i = 0; i < 4096; i++) {
        unsigned char expected = (unsigned char)(i * 37 + 19) ^ mask;
        if (page[i] != expected) {
            fprintf(stderr, "FAIL: %s byte %u got %02x expected %02x\n",
                    mask ? "parent" : "child", i, page[i], expected);
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    unsigned char seed[4096], observed[4096];
    int channel[2], fd = -1, status = 0, result = 1;
    pid_t child = -1;
    volatile unsigned char *page = MAP_FAILED;
    for (unsigned i = 0; i < sizeof(seed); i++) seed[i] = i * 37 + 19;
    fd = memfd_create("readonly-fork-upgrade", MFD_CLOEXEC);
    if (fd < 0 || write(fd, seed, sizeof(seed)) != sizeof(seed) || pipe(channel))
        goto done;
    page = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (page == MAP_FAILED || check(page, 0)) goto done;
    child = fork();
    if (child < 0) goto done;
    if (!child) {
        char ready;
        close(channel[1]);
        if (read(channel[0], &ready, 1) != 1 || ready != 'w') _exit(2);
        _exit(check(page, 0));
    }
    close(channel[0]);
    if (mprotect((void *)page, 4096, PROT_READ | PROT_WRITE)) goto done;
    for (unsigned i = 0; i < sizeof(seed); i++) page[i] = seed[i] ^ 0xa5;
    if (check(page, 0xa5) || write(channel[1], "w", 1) != 1) goto done;
    pid_t reaped;
    do { reaped = waitpid(child, &status, 0); } while (reaped < 0 && errno == EINTR);
    if (reaped != child) goto done;
    child = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status)) goto done;
    if (pread(fd, observed, 4096, 0) != 4096 || memcmp(seed, observed, 4096))
        goto done;
    result = 0;
done:
    if (child > 0) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    if (page != MAP_FAILED) munmap((void *)page, 4096);
    if (fd >= 0) close(fd);
    puts(result ? "FAIL: read-only fork then parent upgrade" :
         "PASS: parent upgrade preserves private child snapshot and original file");
    return result;
}
