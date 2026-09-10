// SPDX-License-Identifier: GPL-2.0
/* Fault injection at the real page-settlement helper's storage boundary. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdatomic.h>
#include <time.h>
#include "../../user/pgstate.h"
#include "execution-fixture.h"
#include "../../user/page-receipt.h"
typedef const struct execution_target *memory_target;
static int ctx_id(memory_target p) { return (int)p->context->identity; }
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#define VMR_PG_SIZE 4096
#define SETTLE_BYTES 1
#define SETTLE_ABSENT 2
#define SETTLE_CLAIM 3
#define SETTLE_NOTHOLDER 6
struct loan { uint64_t page; };
static int response, shared, object_present, acked, retired, marked, retained;
static long poke_result, object_result;
static unsigned long n_settle_to_object;
static unsigned char storage[VMR_PG_SIZE];
/* Fault injection below exercises private native/object writes. Real shared
 * backing, aliases and receipt ordering are covered by settle-shared.c. */
static int shared_obj = -1;
static int shared_range_has(memory_target p,uint64_t a) { (void)p; (void)a; return 0; }
static off_t shared_obj_off(memory_target p,uint64_t a) { (void)p; (void)a; return -1; }
static int shared_obj_has(off_t off) { (void)off; return 0; }
static int coherence, crossing;
static atomic_int source_phase;
static uint64_t now_us(void)
{ struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t)); return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
static void reserved(void)
{ struct pg_claim c={0}; assert(pg_try_claim(1,0x1000,PST_UNINSTALLING,&c)<0 && errno==EBUSY); }
static void coh_enter(void) { assert(!coherence); coherence=1; }
static void coh_leave(void) { assert(coherence); coherence=0; }
static void *source_get(void *unused)
{
    (void)unused;
    while(!atomic_load(&source_phase)) usleep(100);
    struct pg_claim c={0};
    assert(pg_claim_wait(1,0x1000,PST_UNINSTALLING,1000,&c)>=0);
    atomic_store(&source_phase,2);
    pg_finish_required(&c,PST_INVALID);
    return NULL;
}
static uint64_t as_id(memory_target pid) { return pid->view.mm->identity; }
static struct loan *loan_snapshot(uint64_t as, size_t *n)
{ (void)as; *n=1; struct loan *p=malloc(sizeof(*p)); assert(p); p->page=0x1000; return p; }
static uint64_t page_holder(memory_target p, uint64_t a) { (void)p; (void)a; return 12; }
static int page_shared(memory_target p, uint64_t a) { (void)p; (void)a; return shared; }
static int ctx_page_pull(memory_target p, uint64_t a, void *b,struct source_page_receipt *receipt)
{ receipt->target=p->source; receipt->base=a; receipt->count=1; reserved(); assert(!coherence);
  if(crossing && atomic_load(&source_phase)<2) {
      int before=0; atomic_compare_exchange_strong(&source_phase,&before,1);
      return SETTLE_CLAIM;
  }
  memset(b,0xa7,VMR_PG_SIZE); return response; }
static long poke(memory_target p,uint64_t a,const void *b,uint64_t n)
{ (void)p; (void)a; reserved(); assert(coherence); assert(n==VMR_PG_SIZE); if(poke_result>0)memcpy(storage,b,poke_result); return poke_result; }
static long obj_write(memory_target p,uint64_t a,const void *b,uint64_t n)
{ (void)p; (void)a; reserved(); assert(coherence); assert(n==VMR_PG_SIZE); if(object_result>0)memcpy(storage,b,object_result); return object_result; }
static void mark_installed(memory_target p,uint64_t a,uint64_t n) { (void)p; (void)a; (void)n; marked++; }
static void retain_put(memory_target p,uint64_t a,const void *b) { (void)p; (void)a; assert(!memcmp(b,storage,VMR_PG_SIZE)); retained++; }
static void ack_installed(const struct source_page_receipt *receipt,uint64_t a) { assert(receipt->base==a && receipt->count==1 && receipt->target.mm==1); reserved(); assert(!coherence); acked++; }
static int own_object_page(memory_target p,uint64_t a,void *b) { (void)p; (void)a; (void)b; reserved(); return object_present; }
static int page_make_writable(memory_target p,uint64_t a) { (void)p; (void)a; return 1; }
static void page_lend(memory_target p,uint64_t a,uint64_t who) { (void)p; (void)a; reserved(); assert(coherence); assert(!who); retired++; }
static void ack_flush(void) { assert(!coherence); if(acked && !retired) reserved(); }
/* These table fixtures do not issue native transfer tickets. Real received
 * ticket lifetime is covered by fault-page-origin's failed-landing modes. */
static void source_receipt_finish(struct source_page_receipt *receipt) { assert(!receipt->episode); }
#include "../../user/page-settle.h"
static void reset(int r)
{ assert(!coherence); crossing=0; atomic_store(&source_phase,0); response=r; shared=object_present=acked=retired=marked=retained=0; poke_result=object_result=-1; memset(storage,0xcc,sizeof(storage)); }
int main(void)
{
    alarm(15);
    assert(!strcmp(pgst_name(PST_REQUESTING),"REQUESTING"));
    for (int s=0;s<2;s++) {
        reset(5); shared=s;
        assert(page_recall_all(test_target(1))==-1 && !acked && !retired);
        reset(SETTLE_BYTES); shared=s; poke_result=42; object_result=31;
        assert(page_recall_all(test_target(1))==-1 && !acked && !retired && !marked);
        reset(SETTLE_BYTES); shared=s; poke_result=42; object_result=VMR_PG_SIZE;
        assert(!page_recall_all(test_target(1)) && acked==1 && retired==1 && !marked && retained==1);
        reset(SETTLE_BYTES); shared=s; poke_result=VMR_PG_SIZE;
        assert(!page_recall_all(test_target(1)) && acked==1 && retired==1 && marked==1 && !retained);
        for(int r=SETTLE_ABSENT;r<=SETTLE_NOTHOLDER;r+=4) {
            reset(r); shared=s;
            assert(page_recall_all(test_target(1))==-1 && !acked && !retired);
            object_present=1;
            assert(!page_recall_all(test_target(1)) && !acked && retired==1);
        }
    }
    reset(SETTLE_BYTES); crossing=1; poke_result=VMR_PG_SIZE;
    pthread_t source; assert(!pthread_create(&source,NULL,source_get,NULL));
    assert(!page_recall_all(test_target(1)));
    assert(!pthread_join(source,NULL));
    assert(atomic_load(&source_phase)==2 && acked==1 && retired==1 && marked==1);
    struct pg_claim after={0};
    assert(pg_try_claim(1,0x1000,PST_REQUESTING,&after)>=0);
    pg_finish_required(&after,PST_OWNED);
    puts("PASS: source CLAIM releases local exclusion for the source GET, then reserves the complete landing/ACK/retirement episode");
    puts("PASS: settlement refuses failed pulls, partial landings and absent unproven copies; shared and private loans retire only with complete bytes");
    return 0;
}
