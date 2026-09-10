// SPDX-License-Identifier: GPL-2.0
/* A completed native io_uring submission must publish its shared ring writes
 * to the executor before the forwarded syscall returns. */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/io_uring.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
    struct io_uring_params p = {0};
    int fd = syscall(SYS_io_uring_setup, 8, &p);
    if (fd < 0) { perror("FAIL: io_uring_setup"); return 1; }
    size_t sq_length = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cq_length = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP)
        sq_length = cq_length = sq_length > cq_length ? sq_length : cq_length;
    char *sq = mmap(NULL, sq_length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IORING_OFF_SQ_RING);
    char *cq = sq;
    if (!(p.features & IORING_FEAT_SINGLE_MMAP))
        cq = mmap(NULL, cq_length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IORING_OFF_CQ_RING);
    size_t entries_length = p.sq_entries * sizeof(struct io_uring_sqe);
    struct io_uring_sqe *entries = mmap(NULL, entries_length, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd, IORING_OFF_SQES);
    int result = 1;
    if (sq == MAP_FAILED || cq == MAP_FAILED || entries == MAP_FAILED) {
        perror("FAIL: mmap ring"); goto done;
    }
    unsigned *head = (unsigned *)(sq + p.sq_off.head);
    unsigned *tail = (unsigned *)(sq + p.sq_off.tail);
    unsigned *array = (unsigned *)(sq + p.sq_off.array);
    unsigned mask = *(unsigned *)(sq + p.sq_off.ring_mask);
    unsigned *cq_head = (unsigned *)(cq + p.cq_off.head);
    unsigned *cq_tail = (unsigned *)(cq + p.cq_off.tail);
    unsigned cq_mask = *(unsigned *)(cq + p.cq_off.ring_mask);
    struct io_uring_cqe *completions = (void *)(cq + p.cq_off.cqes);
    for (unsigned round = 0; round < 256; round++) {
        unsigned index = round & mask;
        memset(&entries[index], 0, sizeof(entries[index]));
        entries[index].opcode = IORING_OP_NOP;
        entries[index].user_data = UINT64_C(0xabc0000000000000) + round;
        array[index] = index;
        __atomic_store_n(tail, round + 1, __ATOMIC_RELEASE);
        int submitted;
        do { submitted = syscall(SYS_io_uring_enter, fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0); }
        while (submitted < 0 && errno == EINTR);
        unsigned observed_head = __atomic_load_n(head, __ATOMIC_ACQUIRE);
        unsigned observed_tail = __atomic_load_n(cq_tail, __ATOMIC_ACQUIRE);
        if (submitted != 1 || observed_head != round + 1 || observed_tail != round + 1) {
            fprintf(stderr, "FAIL: round=%u submitted=%d SQ head=%u expected=%u CQ tail=%u errno=%d\n",
                    round, submitted, observed_head, round + 1, observed_tail, errno);
            goto done;
        }
        struct io_uring_cqe entry = completions[round & cq_mask];
        if (entry.res || entry.user_data != UINT64_C(0xabc0000000000000) + round) {
            fprintf(stderr, "FAIL: round=%u CQ result=%d user_data=%llx\n", round,
                    entry.res, (unsigned long long)entry.user_data);
            goto done;
        }
        __atomic_store_n(cq_head, round + 1, __ATOMIC_RELEASE);
    }
    puts("PASS: 256 io_uring submissions publish exact SQ/CQ progress and completion data");
    result = 0;
done:
    if (entries != MAP_FAILED) munmap(entries, entries_length);
    if (cq != MAP_FAILED && cq != sq) munmap(cq, cq_length);
    if (sq != MAP_FAILED) munmap(sq, sq_length);
    close(fd);
    return result;
}
