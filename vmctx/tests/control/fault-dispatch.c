/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../user/pgstate.h"
#include "execution-fixture.h"
typedef const struct execution_target *memory_target;
static inline int ctx_id(memory_target p) { return (int)p->context->identity; }

/* Exercise the production dispatcher with real reservations and storage.
 * A source signal writer must acquire the same page before replying. A legal
 * access answer must restart page service and observe the writer's new bytes. */
enum { OWNF_OFF, OWNF_CLAIMED, OWNF_BUSY };
static int vac_recent_n, coh_depth, advisory, fd, reply;
static unsigned n_ownf_wake_absent, n_handoff_yield;
static unsigned services, dispatches, receipts, restarts;
static uint64_t first_episode;
static struct { int op; } fault_map;
static uint64_t as_id(memory_target pid) { return pid->view.mm->identity; }
static void vac_recent_check(memory_target pid, uint64_t addr, uint64_t err)
{ (void)pid; (void)addr; (void)err; abort(); }
static int ownf_claim(uint64_t as, uint64_t page, uint32_t *gen)
{ (void)as; (void)page; (void)gen; assert(!advisory); advisory=1; return OWNF_CLAIMED; }
static int ownf_wait(uint64_t as, uint64_t page, uint32_t gen)
{ (void)as; (void)page; (void)gen; abort(); }
static void ownf_release(uint64_t as, uint64_t page, int present)
{ (void)as; (void)page; (void)present; assert(advisory); advisory=0; }
static int as_any_present(memory_target pid, uint64_t page)
{ (void)pid; (void)page; return 1; }
static void ack_flush(void)
{
    assert(advisory);
    struct pg_claim claim={0};
    assert(pg_try_claim(7, 4096, PST_UNINSTALLING, &claim)<0 && errno==EBUSY);
    receipts++;
}
static int fault_from_home_inner(memory_target, uint64_t, uint64_t, uint64_t, int, int);
static int owner_takes_fault(memory_target, uint64_t, uint64_t, uint64_t, uint64_t);
#include "../../user/fault-dispatch.h"

static int fault_from_home_inner(memory_target pid, uint64_t vec, uint64_t addr,
                                uint64_t err, int supply, int legal)
{
    (void)pid; (void)vec; (void)addr; (void)err;
    assert(advisory);
    struct pg_claim other={0};
    assert(pg_try_claim(7,4096,PST_UNINSTALLING,&other)<0 && errno==EBUSY);
    services++;
    if(supply)return 1;
    pthread_mutex_lock(&pgstate_mx);
    uint64_t episode=pg_slot_find(7,4096,0)->episode;
    pthread_mutex_unlock(&pgstate_mx);
    if (!legal) { first_episode=episode; fault_map.op=99; return FAULT_NEEDS_OWNER; }
    assert(episode>first_episode);
    unsigned char page[4096];
    assert(pread(fd,page,sizeof(page),0)==sizeof(page));
    for (unsigned i=0;i<sizeof(page);i++) assert(page[i]==0x5a);
    restarts++;
    return 1;
}

static void *source_writer(void *unused)
{
    (void)unused;
    struct pg_claim claim={0};
    assert(!advisory && receipts==1 && !fault_map.op);
    assert(pg_try_claim(7,4096,PST_UNINSTALLING,&claim)>=0);
    unsigned char page[4096];
    for (unsigned i=0;i<sizeof(page);i++) page[i]=0x5a;
    assert(pwrite(fd,page,sizeof(page),0)==sizeof(page));
    pg_finish_required(&claim,PST_INVALID);
    return NULL;
}

static int owner_takes_fault(memory_target pid, uint64_t vec, uint64_t base,
                             uint64_t addr, uint64_t err)
{
    (void)pid; (void)vec; (void)base; (void)addr; (void)err;
    pthread_t worker;
    assert(!coh_depth && !advisory);
    dispatches++;
    assert(!pthread_create(&worker,NULL,source_writer,NULL));
    assert(!pthread_join(worker,NULL));
    return reply;
}

int main(void)
{
    alarm(10);
    fd=memfd_create("fault-signal-frame",MFD_CLOEXEC);
    assert(fd>=0 && !ftruncate(fd,4096));
    for (reply=0;reply<=2;reply++) {
        services=dispatches=receipts=restarts=0;
        assert(fault_from_home_mode(test_target(7),14,4096+3,16,0)==(reply!=0));
        assert(dispatches==1 && services==1+(reply==2) && receipts==services);
        assert(restarts==(reply==2) && !advisory);
        struct pg_claim claim={0};
        assert(pg_try_claim(7,4096,PST_UNINSTALLING,&claim)>=0);
        pg_finish_required(&claim,PST_INVALID);
    }
    services=dispatches=receipts=0;
    struct pg_claim prefill={0};uint32_t generation;
    assert(ownf_claim(7,4096,&generation)==OWNF_CLAIMED);
    assert(pg_try_claim(7,4096,PST_UNINSTALLING,&prefill)>=0);
    assert(fault_from_home_mode(test_target(7),14,4096,4,1)==1);
    assert(services==1 && receipts==1 && !dispatches && advisory);
    pg_finish_required(&prefill,PST_OWNED);ownf_release(7,4096,1);
    close(fd);
    puts("PASS: exception dispatch releases both claims and legal access restarts from new bytes");
    return 0;
}
