// SPDX-License-Identifier: GPL-2.0
/* Exercise retained shared-object identity, capacity, range splits, permissions,
 * independent address spaces, inheritance and concurrent lookup/publication. */
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include "../../user/shared-layout.h"
static atomic_int finished;
static void *reader(void *unused)
{
    (void)unused;
    while (!atomic_load(&finished)) {
        struct shared_range r;
        if (shared_layout_lookup(71, 0x2000, &r)) {
            assert(r.st == 0x2000 && r.en == 0x3000);
            assert((r.off == 0x9000 && r.prot == 1) || (r.off == 0xb000 && r.prot == 3));
        }
    }
    return NULL;
}
int main(void)
{
    struct shared_range r;
    for (unsigned i = 0; i < 2048; i++)
        shared_layout_note(7, 0x10000 + i * 8192ULL, 0x11000 + i * 8192ULL, 0x800000 + i * 4096ULL, 1, 41);
    for (unsigned i = 0; i < 2048; i++) {
        assert(shared_layout_lookup(7, 0x10000 + i * 8192ULL, &r));
        assert(r.off == 0x800000 + i * 4096ULL && r.prot == 1);
    }
    shared_layout_note(8, 0x10000, 0x11000, 0xf000, 3, 42);
    assert(shared_layout_lookup(7, 0x10000, &r) && r.off == 0x800000);
    shared_layout_note(70, 0x1000, 0x9000, 0x8000, 3, 42);
    shared_layout_forget(70, 0x3000, 0x5000);
    assert(!shared_layout_lookup(70, 0x3000, &r));
    assert(shared_layout_lookup(70, 0x5000, &r) && r.off == 0xc000 && r.en == 0x9000);
    shared_layout_protect(70, 0x6000, 0x8000, 1);
    assert(shared_layout_lookup(70, 0x7000, &r) && r.st == 0x6000 && r.en == 0x8000 && r.off == 0xd000 && r.prot == 1);
    assert(shared_layout_lookup(70, 0x8000, &r) && r.off == 0xf000 && r.prot == 3);
    shared_layout_inherit(72, 70);
    shared_layout_forget(70, 0, UINT64_MAX);
    assert(!shared_layout_lookup(70, 0x7000, &r));
    assert(shared_layout_lookup(72, 0x7000, &r) && r.prot == 1 && r.off == 0xd000 && r.object_id == 42);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, reader, NULL));
    for (unsigned i = 0; i < 10000; i++) {
        shared_layout_note(71, 0x2000, 0x3000, i & 1 ? 0x9000 : 0xb000, i & 1 ? 1 : 3, 43);
        if (!(i % 3)) shared_layout_forget(71, 0x2000, 0x3000);
    }
    atomic_store(&finished, 1);
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    puts("PASS: shared mapping capacity, identity, partial retirement, permissions, inheritance and concurrent snapshots");
    free(shared_rr);
}
