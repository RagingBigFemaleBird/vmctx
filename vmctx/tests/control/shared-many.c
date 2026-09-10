// SPDX-License-Identifier: GPL-2.0
/* Keep more than 64 shared mappings live. Read-only shared mappings must read
 * each source file's own data, and retirement must not consume table capacity. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void)
{
    enum { N = 96 };
    const volatile uint32_t *mappings[N];
    for (unsigned round = 0; round < 2; round++) {
        for (unsigned i = 0; i < N; i++) {
            int fd = memfd_create("shared-many", MFD_CLOEXEC);
            uint32_t word = 0x51a00000 | round << 16 | i;
            if (fd < 0 || ftruncate(fd, 4096) || pwrite(fd, &word, sizeof(word), 0) != sizeof(word)) return 2;
            mappings[i] = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
            close(fd);
            if (mappings[i] == MAP_FAILED) return 2;
        }
        for (unsigned i = 0; i < N; i++) {
            uint32_t want = 0x51a00000 | round << 16 | i;
            if (mappings[i][0] != want) {
                fprintf(stderr, "FAIL: shared mapping %u round %u got=%08x want=%08x\n", i, round, mappings[i][0], want);
                return 1;
            }
        }
        for (unsigned i = 0; i < N; i++) if (munmap((void *)mappings[i], 4096)) return 2;
    }
    puts("PASS: 96 live read-only shared mappings retain distinct file contents across reuse");
    return 0;
}
