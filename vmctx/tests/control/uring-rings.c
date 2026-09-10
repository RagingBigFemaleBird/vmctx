// SPDX-License-Identifier: GPL-2.0
/* Kernel-owned ring writers must agree with remote user loads and stores.
 * Two independent rings also test object identity and completion isolation. */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/io_uring.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
struct ring {
    int fd;
    struct io_uring_params p;
    void *sq, *cq;
    struct io_uring_sqe *sqes;
    size_t sqlen, cqlen;
};
static unsigned load(void *base, unsigned off)
{ return __atomic_load_n((unsigned *)((char *)base + off), __ATOMIC_ACQUIRE); }
static void store(void *base, unsigned off, unsigned value)
{ __atomic_store_n((unsigned *)((char *)base + off), value, __ATOMIC_RELEASE); }
static int setup(struct ring *r)
{
    r->fd = syscall(SYS_io_uring_setup, 8, &r->p);
    if (r->fd < 0) return -1;
    r->sqlen = r->p.sq_off.array + r->p.sq_entries * sizeof(unsigned);
    r->cqlen = r->p.cq_off.cqes + r->p.cq_entries * sizeof(struct io_uring_cqe);
    if (r->p.features & IORING_FEAT_SINGLE_MMAP) {
        if (r->sqlen < r->cqlen) r->sqlen = r->cqlen;
        r->cqlen = r->sqlen;
    }
    r->sq = mmap(NULL, r->sqlen, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, IORING_OFF_SQ_RING);
    r->cq = r->p.features & IORING_FEAT_SINGLE_MMAP ? r->sq :
        mmap(NULL, r->cqlen, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, IORING_OFF_CQ_RING);
    r->sqes = mmap(NULL, r->p.sq_entries * sizeof(*r->sqes),
                   PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, IORING_OFF_SQES);
    return r->sq == MAP_FAILED || r->cq == MAP_FAILED || r->sqes == MAP_FAILED ? -1 : 0;
}
int main(void)
{
    struct ring rings[2] = {0};
    alarm(20);
    for (unsigned i = 0; i < 2; i++) if (setup(&rings[i])) {
        fprintf(stderr, "FAIL: io_uring setup ring %u: %s\n", i, strerror(errno)); return 2;
    }
    for (unsigned round = 0; round < 32; round++) for (unsigned i = 0; i < 2; i++) {
        struct ring *r = &rings[i];
        unsigned tail = load(r->sq, r->p.sq_off.tail), old_head = load(r->cq, r->p.cq_off.head);
        unsigned mask = load(r->sq, r->p.sq_off.ring_mask);
        for (unsigned j = 0; j < 4; j++) {
            unsigned slot = (tail + j) & mask;
            struct io_uring_sqe *s = &r->sqes[slot];
            memset(s, 0, sizeof(*s)); s->opcode = IORING_OP_NOP;
            s->user_data = UINT64_C(0xabcd00000000) | ((uint64_t)i << 24) | (round << 8) | j;
            ((unsigned *)((char *)r->sq + r->p.sq_off.array))[slot] = slot;
        }
        store(r->sq, r->p.sq_off.tail, tail + 4);
        long n = syscall(SYS_io_uring_enter, r->fd, 4, 4, IORING_ENTER_GETEVENTS, NULL, 0);
        unsigned sqhead = load(r->sq, r->p.sq_off.head), cqtail = load(r->cq, r->p.cq_off.tail);
        if (n != 4 || sqhead != tail + 4 || cqtail - old_head != 4) {
            fprintf(stderr, "FAIL: ring %u round %u submitted=%ld errno=%d SQ=%u/%u CQ=%u/%u\n",
                    i, round, n, errno, sqhead, tail + 4, old_head, cqtail); return 1;
        }
        unsigned cqmask = load(r->cq, r->p.cq_off.ring_mask);
        struct io_uring_cqe *cqes = (void *)((char *)r->cq + r->p.cq_off.cqes);
        for (unsigned j = 0; j < 4; j++) {
            struct io_uring_cqe *c = &cqes[(old_head + j) & cqmask];
            uint64_t wanted = UINT64_C(0xabcd00000000) | ((uint64_t)i << 24) | (round << 8) | j;
            if (c->res || c->flags || c->user_data != wanted) {
                fprintf(stderr, "FAIL: ring %u round %u completion %u data=%llx wanted=%llx res=%d flags=%u\n",
                        i, round, j, (unsigned long long)c->user_data, (unsigned long long)wanted, c->res, c->flags);
                return 1;
            }
        }
        store(r->cq, r->p.cq_off.head, cqtail);
    }
    for (unsigned i = 0; i < 2; i++) {
        struct ring *r = &rings[i];
        if (munmap(r->sqes, r->p.sq_entries * sizeof(*r->sqes)) || munmap(r->sq, r->sqlen) ||
            (r->cq != r->sq && munmap(r->cq, r->cqlen)) || close(r->fd)) return 2;
    }
    puts("PASS: independent io_uring rings preserve kernel-written heads and all completion bytes across wraparound");
    return 0;
}
