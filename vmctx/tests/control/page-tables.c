// SPDX-License-Identifier: GPL-2.0
/* Exercise the destination's real tables without a vmctx kernel or network.
 * cc -O2 -ffunction-sections -fdata-sections -I vmctx/kernel -pthread \
 *    vmctx/tests/control/page-tables.c -Wl,--gc-sections -o page-tables
 * Run each case in a fresh process (see usage).
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/resource.h>
static int fail_table_alloc;
static void *table_calloc(size_t n, size_t size)
{
    return fail_table_alloc ? NULL : calloc(n, size);
}
#define calloc table_calloc
#define main vmremote_program_main
#include "../../user/vmremote.c"
#undef main
#undef calloc
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"

/* Publication now requires the outer layout lock before any metadata change.
 * Exercise the current API with the same lock contract as forward(). */
static void cons_note(memory_target pid, uint64_t start, uint64_t end, uint64_t seq,
                      uint32_t op, uint32_t prot, uint64_t off)
{
    pthread_mutex_lock(&layout_order_lock);
    cons_note_locked(pid, start, end, seq, op, prot, off);
    pthread_mutex_unlock(&layout_order_lock);
}

static int loan_delete(void)
{
    const pid_t as = 7;
    const uint64_t first = 0x10000000;
    /* Multiplying the page number modulo LENT_SLOTS maps these two
     * different pages to the same bucket. Verify that precondition. */
    const uint64_t second = first + (uint64_t)LENT_SLOTS * VMR_PG_SIZE;
    if (lent_slot(as, first) != lent_slot(as, second)) {
        fprintf(stderr, "FAIL: collision fixture is invalid\n");
        return 1;
    }
    page_lend(test_target(as), first, 41);
    page_lend(test_target(as), second, 42);
    if (page_holder(test_target(as), first) != 41 || page_holder(test_target(as), second) != 42) {
        fprintf(stderr, "FAIL: initial loans were not recorded\n");
        return 1;
    }
    lend_forget_range(as, first, first + VMR_PG_SIZE);
    uint64_t owner = page_holder(test_target(as), second);
    int pass = page_holder(test_target(as), first) == 0 && owner == 42;
    printf("%s: unmapping one page preserves the other loan (owner=%llu)\n",
           pass ? "PASS" : "FAIL", (unsigned long long)owner);
    return pass ? 0 : 1;
}

static int loan_identity(void)
{
    const pid_t first_as = 7, second_as = first_as + (1 << 17);
    const uint64_t page = 0x10000000;
    page_lend(test_target(first_as), page, 41);
    page_lend(test_target(second_as), page, 42);
    uint64_t first = page_holder(test_target(first_as), page);
    uint64_t second = page_holder(test_target(second_as), page);
    int pass = first == 41 && second == 42;
    printf("%s: different address spaces retain independent loans (%llu, %llu)\n",
           pass ? "PASS" : "FAIL", (unsigned long long)first,
           (unsigned long long)second);
    return pass ? 0 : 1;
}

static int installed_delete(void)
{
    const pid_t ctx = 7;
    const uint64_t first = 0x10000000;
    const uint64_t second = first + (uint64_t)INSTALLED_SLOTS * VMR_PG_SIZE;
    page_installed(test_target(ctx), first);
    page_installed(test_target(ctx), second);
    mark_handed_over(test_target(ctx), first);
    int already = page_installed(test_target(ctx), second);
    mark_handed_over(test_target(ctx), second);
    int present = page_is_installed(test_target(ctx), second);
    int pass = already == 1 && present == 0;
    printf("%s: an existing installation beyond a tombstone is found once "
           "and fully removed on handover (already=%d, present=%d)\n",
           pass ? "PASS" : "FAIL", already, present);
    return pass ? 0 : 1;
}

static int installed_identity(void)
{
    const pid_t first = 7, second = first + 65536;
    const uint64_t page = 0x10000000;
    if (page_installed(test_target(first), page) || page_is_installed(test_target(second), page) ||
        page_installed(test_target(second), page)) {
        puts("FAIL: context ids separated by 65536 alias installation records");
        return 1;
    }
    mark_handed_over(test_target(first), page);
    if (page_is_installed(test_target(first), page) || !page_is_installed(test_target(second), page))
        return 1;
    puts("PASS: installation records retain full context identity");
    return 0;
}

static int installed_normalize(void)
{
    const uint64_t page = 0x10000000;
    if (page_installed(test_target(7), page + 3) || !page_is_installed(test_target(7), page) ||
        !page_installed(test_target(7), page + 4095)) {
        puts("FAIL: addresses in one page name different installation records");
        return 1;
    }
    mark_handed_over(test_target(7), page + 42);
    if (page_is_installed(test_target(7), page)) return 1;
    puts("PASS: installation queries and removal normalize page addresses");
    return 0;
}

static int installed_capacity_test(void)
{
    const uint64_t base = 0x10000000;
    const unsigned pages = 100000;
    for (unsigned i = 0; i < pages; i++) {
        uint64_t page = base + (uint64_t)i * VMR_PG_SIZE;
        if (page_installed(test_target(7), page) || !page_is_installed(test_target(7), page)) {
            fprintf(stderr, "FAIL: installation %u not independently recorded\n", i);
            return 1;
        }
    }
    for (unsigned i = 0; i < pages; i++)
        if (!page_installed(test_target(7), base + (uint64_t)i * VMR_PG_SIZE)) return 1;
    for (unsigned i = 0; i < pages; i += 2)
        mark_handed_over(test_target(7), base + (uint64_t)i * VMR_PG_SIZE);
    for (unsigned i = 0; i < pages; i++)
        if (page_is_installed(test_target(7), base + (uint64_t)i * VMR_PG_SIZE) != (int)(i & 1))
            return 1;
    for (unsigned i = 0; i < pages; i += 2)
        if (page_installed(test_target(7), base + (uint64_t)i * VMR_PG_SIZE)) return 1;
    for (unsigned i = 0; i < pages; i++)
        if (!page_is_installed(test_target(7), base + (uint64_t)i * VMR_PG_SIZE)) return 1;
    puts("PASS: 100000 installations survive deletion and reuse");
    return 0;
}

static int installed_collisions(void)
{
    const uint64_t base = 0x10000000;
    const uint64_t stride = (uint64_t)INSTALLED_SLOTS * VMR_PG_SIZE;
    for (unsigned i = 0; i < 128; i++)
        if (page_installed(test_target(7), base + i * stride) ||
            !page_is_installed(test_target(7), base + i * stride)) {
            fprintf(stderr, "FAIL: colliding installation %u was lost\n", i);
            return 1;
        }
    for (unsigned i = 0; i < 128; i += 2)
        mark_handed_over(test_target(7), base + i * stride);
    for (unsigned i = 1; i < 128; i += 2)
        if (!page_installed(test_target(7), base + i * stride)) return 1;
    for (unsigned i = 1; i < 128; i += 2) {
        mark_handed_over(test_target(7), base + i * stride);
        if (page_is_installed(test_target(7), base + i * stride)) return 1;
    }
    puts("PASS: collided installations remain unique across deletion");
    return 0;
}

static int installed_alloc_failure(void)
{
    for (unsigned arm = 0; arm < 3; arm++) {
        pid_t child = fork();
        if (child < 0) return 2;
        if (!child) {
            struct rlimit zero = { 0, 0 };
            setrlimit(RLIMIT_CORE, &zero);
            alarm(5);
            if (arm) {
                for (unsigned i = 0; i < INSTALLED_SLOTS / 2; i++)
                    page_installed(test_target(7), 0x10000000 + (uint64_t)i * VMR_PG_SIZE);
                if (arm == 2)
                    for (unsigned i = 0; i < INSTALLED_SLOTS / 2 - 8; i++)
                        installed_remove(test_target(7), 0x10000000 + (uint64_t)i * VMR_PG_SIZE);
            }
            fail_table_alloc = 1;
            page_installed(test_target(7), 0x700000000000);
            _exit(91);
        }
        int status;
        if (waitpid(child, &status, 0) != child) return 2;
        int pass = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
        if (!pass) {
            fprintf(stderr, "FAIL: installation allocation arm %u continued\n", arm);
            return 1;
        }
    }
    puts("PASS: initial, growth, and compaction allocation failures stop the monitor");
    return 0;
}

static int installed_retirement(void)
{
    const uint64_t base = 0x10000000;
    for (unsigned i = 0; i < INSTALLED_SLOTS / 2; i++)
        if (page_installed(test_target(7), base + (uint64_t)i * VMR_PG_SIZE)) return 1;
    for (unsigned i = 0; i < INSTALLED_SLOTS / 2 - 8; i++)
        installed_remove(test_target(7), base + (uint64_t)i * VMR_PG_SIZE);
    if (page_installed(test_target(65543), base)) return 1;
    if (installed_rehashes < 2) return 2; /* the compaction arm must run */
    for (unsigned i = 0; i < INSTALLED_SLOTS / 2; i++)
        if (page_is_installed(test_target(7), base + (uint64_t)i * VMR_PG_SIZE) !=
            (i >= INSTALLED_SLOTS / 2 - 8)) return 1;
    installed_forget_context(test_target(7));
    if (installed_live != 1 || !page_is_installed(test_target(65543), base)) return 1;
    if (page_installed(test_target(7), base) || !page_is_installed(test_target(65543), base)) return 1;
    puts("PASS: compaction and context retirement preserve other contexts and live pages");
    return 0;
}

static pthread_barrier_t install_barrier;
static atomic_uint install_winners;
static void *installed_worker(void *arg)
{
    pid_t pid = 7 + (uintptr_t)arg * 65536;
    const uint64_t base = 0x10000000;
    int failed = 0;
    for (unsigned i = 0; i < 20000; i++) {
        uint64_t page = base + (uint64_t)i * VMR_PG_SIZE;
        if (page_installed(test_target(pid), page) || !page_is_installed(test_target(pid), page)) failed = 1;
    }
    pthread_barrier_wait(&install_barrier);
    if (!page_installed(test_target(123456789), base)) atomic_fetch_add(&install_winners, 1);
    for (unsigned i = 0; i < 20000; i += 2)
        installed_remove(test_target(pid), base + (uint64_t)i * VMR_PG_SIZE);
    for (unsigned i = 0; i < 20000; i++)
        if (page_is_installed(test_target(pid), base + (uint64_t)i * VMR_PG_SIZE) != (int)(i & 1))
            failed = 1;
    installed_forget_context(test_target(pid));
    return (void *)(uintptr_t)failed;
}

static int installed_concurrent(void)
{
    pthread_t workers[4];
    alarm(20);
    if (pthread_barrier_init(&install_barrier, NULL, 4)) return 2;
    for (uintptr_t i = 0; i < 4; i++)
        if (pthread_create(&workers[i], NULL, installed_worker, (void *)i)) _exit(2);
    int failed = 0;
    for (unsigned i = 0; i < 4; i++) {
        void *result;
        if (pthread_join(workers[i], &result)) return 2;
        failed |= (int)(uintptr_t)result;
    }
    pthread_barrier_destroy(&install_barrier);
    if (atomic_load(&install_winners) != 1 || installed_live != 1 ||
        !page_is_installed(test_target(123456789), 0x10000000)) failed = 1;
    printf("%s: concurrent growth, queries, deletion and unique insertion\n", failed ? "FAIL" : "PASS");
    return failed;
}

static int installed_scale(void)
{
    const unsigned pages = 32768, rounds = 4;
    const uint64_t base = 0x10000000;
    uint64_t start = own_now_us();
    for (unsigned r = 0; r < rounds; r++)
        for (unsigned i = 0; i < pages; i++) {
            uint64_t page = base + (uint64_t)i * VMR_PG_SIZE;
            if (page_installed(test_target(7), page) != !!r || !page_is_installed(test_target(7), page))
                return 1;
        }
    printf("{\"cycles\":%u,\"elapsed_us\":%llu}\n", pages * rounds,
           (unsigned long long)(own_now_us() - start));
    return 0;
}

static int loan_collisions(void)
{
    const pid_t as = 7;
    const uint64_t base = 0x10000000, stride = (uint64_t)LENT_SLOTS * VMR_PG_SIZE;
    for (unsigned i = 0; i < 128; i++) {
        page_lend_mode(test_target(as), base + i * stride, 900 + i, i & 1);
        page_watch_set(test_target(as), base + i * stride + 3, 1);
    }
    for (unsigned i = 0; i < 128; i++) {
        uint64_t page = base + i * stride;
        if (page_holder(test_target(as), page + 7) != 900 + i ||
            page_shared(test_target(as), page) != (int)(i & 1)) {
            fprintf(stderr, "FAIL: collided loan %u lost or aliased\n", i);
            return 1;
        }
        if (i & 1)
            page_lend(test_target(as), page, 0);
    }
    for (unsigned i = 0; i < 128; i += 3)
        lend_forget_range(as, base + i * stride, base + i * stride + VMR_PG_SIZE);
    for (unsigned i = 0; i < 128; i++) {
        uint64_t page = base + i * stride;
        int removed = i % 3 == 0;
        uint64_t owner = removed || (i & 1) ? 0 : 900 + i;
        if (page_holder(test_target(as), page) != owner ||
            page_watched(test_target(as), page) != (!removed && (i & 1)) ||
            (removed && page_lent_state(test_target(as), page) != LENT_UNSEEN)) {
            fprintf(stderr, "FAIL: deleting collided loan %u changed another record\n", i);
            return 1;
        }
        if (removed) {
            page_lend(test_target(as), page, 2000 + i);
            page_lend(test_target(as), page, 0);
            if (page_watched(test_target(as), page)) {
                fprintf(stderr, "FAIL: a recycled page inherited a watch\n");
                return 1;
            }
        }
    }
    puts("PASS: 128 colliding loans, normalized offsets, deletion and watch reuse");
    return 0;
}

static int loan_capacity(void)
{
    const unsigned pages = 150000;
    const uint64_t base = 0x10000000;
    for (unsigned i = 0; i < pages; i++)
        page_lend(test_target(7), base + (uint64_t)i * VMR_PG_SIZE, i + 1);
    for (unsigned i = 0; i < pages; i++)
        if (page_holder(test_target(7), base + (uint64_t)i * VMR_PG_SIZE) != i + 1) {
            fprintf(stderr, "FAIL: live loan %u was not retained\n", i);
            return 1;
        }
    lend_forget_range(7, base, base + (uint64_t)pages * VMR_PG_SIZE);
    for (unsigned i = 0; i < pages; i++)
        if (page_lent_state(test_target(7), base + (uint64_t)i * VMR_PG_SIZE) != LENT_UNSEEN) {
            fprintf(stderr, "FAIL: retired loan %u still exists\n", i);
            return 1;
        }
    puts("PASS: 150000 live loans survive and retire");
    return 0;
}

static pthread_barrier_t loan_barrier;
static atomic_int loan_failed;
static void *loan_worker(void *arg)
{
    const pid_t as = 7 + (*(unsigned *)arg << 17);
    const uint64_t base = 0x10000000;
    for (unsigned i = 0; i < 5000; i++)
        page_lend(test_target(as), base + (uint64_t)i * VMR_PG_SIZE, (uint64_t)as + i);
    pthread_barrier_wait(&loan_barrier);
    for (unsigned i = 0; i < 5000; i++) {
        uint64_t page = base + (uint64_t)i * VMR_PG_SIZE;
        if (page_holder(test_target(as), page) != (uint64_t)as + i)
            atomic_store(&loan_failed, 1);
    }
    lend_forget_range(as, base, base + 2500 * VMR_PG_SIZE);
    pthread_barrier_wait(&loan_barrier);
    for (unsigned i = 0; i < 5000; i++) {
        uint64_t page = base + (uint64_t)i * VMR_PG_SIZE;
        uint64_t owner = i < 2500 ? 0 : (uint64_t)as + i;
        if (page_holder(test_target(as), page) != owner)
            atomic_store(&loan_failed, 1);
    }
    return NULL;
}

static int loan_concurrent(void)
{
    pthread_t threads[4];
    unsigned ids[4];
    alarm(20);
    if (pthread_barrier_init(&loan_barrier, NULL, 4))
        return 2;
    for (unsigned i = 0; i < 4; i++) {
        ids[i] = i;
        if (pthread_create(&threads[i], NULL, loan_worker, &ids[i]))
            return 2;
    }
    for (unsigned i = 0; i < 4; i++)
        if (pthread_join(threads[i], NULL))
            return 2;
    pthread_barrier_destroy(&loan_barrier);
    int failed = atomic_load(&loan_failed);
    printf("%s: concurrent loans and deletion in four colliding address spaces\n",
           failed ? "FAIL" : "PASS");
    return failed;
}

static int loan_alloc_failure(void)
{
    for (int snapshot = 0; snapshot < 2; snapshot++) {
        pid_t child = fork();
        int status;
        if (child < 0)
            return 2;
        if (!child) {
            struct rlimit no_core = {0, 0};
            setrlimit(RLIMIT_CORE, &no_core);
            alarm(5);
            if (snapshot)
                page_lend(test_target(7), 0x10000000, 41);
            fail_table_alloc = 1;
            if (snapshot) {
                size_t count;
                free(loan_snapshot(7, &count));
            } else {
                page_lend(test_target(7), 0x10000000, 41);
            }
            _exit(91);
        }
        int pass = waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
                   WTERMSIG(status) == SIGABRT;
        if (!pass) {
            fprintf(stderr, "FAIL: allocation failure returned success (snapshot=%d)\n", snapshot);
            return 1;
        }
    }
    puts("PASS: allocation failure stops loan creation and snapshotting");
    return 0;
}

static int loan_snapshot_lifetime(void)
{
    const uint64_t first = 0x10000000;
    const uint64_t second = first + (uint64_t)LENT_SLOTS * VMR_PG_SIZE;
    size_t count;
    page_lend(test_target(7), first, 41);
    page_lend_mode(test_target(7), second, 42, 1);
    page_lend(test_target(131079), first, 43);
    struct loan *rows = loan_snapshot(7, &count);
    if (count != 2) return 1;
    lend_forget_range(7, first, second + VMR_PG_SIZE);
    page_lend(test_target(7), first, 91);
    unsigned seen = 0;
    for (size_t i = 0; i < count; i++) {
        if (rows[i].as != 7) return 1;
        if (rows[i].page == first && rows[i].owner == 41 && !rows[i].shared)
            seen |= 1;
        else if (rows[i].page == second && rows[i].owner == 42 && rows[i].shared)
            seen |= 2;
        else return 1;
    }
    free(rows);
    if (seen != 3 || page_holder(test_target(131079), first) != 43) return 1;
    rows = loan_snapshot(0, &count);
    if (count != 2) return 1;
    free(rows);
    page_lend(test_target(7), first, 0);
    rows = loan_snapshot(7, &count);
    free(rows);
    if (count) return 1;
    puts("PASS: loan scans preserve copied identity across deletion and reuse");
    return 0;
}

static int transfer_exclusion(void)
{
    const pid_t as = 7;
    const uint64_t page = 0x10000000;
    for (enum pg_st state = PST_UNINSTALLING; state <= PST_REQUESTING; state++) {
        struct pg_claim claim = {0};
        if (pg_try_claim(as, page, state, &claim) < 0)
            return 2;
        for (unsigned i = 0; i < 100; i++) {
            int retry = fault_from_home_mode(test_target(as), 14, page + 3, 2, 0);
            struct pg_claim other = {0};
            if (retry != 1 || pg_try_claim(as, page, PST_REQUESTING, &other) >= 0) {
                fprintf(stderr, "FAIL: fault retry %u cleared %s\n", i, pgst_name(state));
                return 1;
            }
        }
        pg_finish_required(&claim, PST_INVALID);
    }
    struct pg_claim claim = {0};
    if (n_handoff_yield != 400 || pg_try_claim(as, page, PST_REQUESTING, &claim) < 0)
        return 1;
    puts("PASS: 400 real fault retries preserve all four transfer states until settlement");
    return 0;
}

/* A real page-service request races an incumbent transfer. The storage is
 * an actual sparse memfd; no vmctx syscall is required to reject the request.
 * The old GET path continued after its claim wait expired and punched this
 * object through its object-only fallback. */
static void *busy_get_service(void *arg)
{
    int fd=*(int *)arg;
    int result=pg_serve_conn(fd);
    close(fd);
    return (void *)(intptr_t)result;
}

static int table_replace_object(void *unused,int fd,uint64_t epoch,uint64_t *next)
{ (void)unused;(void)fd;*next=epoch+1;return 0; }

static int transfer_busy_get(unsigned op,int rebind)
{
    alarm(15);
    coh_lock_init();
    const uint64_t base=UINT64_C(0x3000000000);
    const pid_t context=getpid();
    int fd=backing_new("busy-transfer-control"), sockets[2];
    if (fd<0 || socketpair(AF_UNIX,SOCK_STREAM,0,sockets)) return 2;
    unsigned char before[VMR_PG_SIZE], after[VMR_PG_SIZE], payload[VMR_PG_SIZE];
    memset(before,0x6b,sizeof(before));
    if (pwrite(fd,before,sizeof(before),base)!=sizeof(before)) return 2;
    struct vmr_mm_binding source={.context=(uint64_t)context,.mm=(uint64_t)context,.epoch=1};
    struct execution_mm *mm=source_mm_resolve(&source);
    if(!mm)return 2;
    close(fd);fd=mm->backing_fd;
    if(pwrite(fd,before,sizeof(before),base)!=sizeof(before))return 2;
    if(execution_context_binding_init(&ctx_memory[0],context,mm,&source,1))return 2;
    ctx_source_context[0]=source.context;
    struct linux_execution_context native={.fd=-1,
        .exit_fd=syscall(SYS_pidfd_open,context,0),.identity=1};
    if(native.exit_fd<0)return 2;
    /* This fixture stops before any native VM control: it owns only a real
     * process-exit descriptor and a synthetic context-table entry. */
    ctx_native[0]=native;ctxs[0]=context;nctxs=1;
    ctx_pid=context;
    memory_target current=ctx_target(context);
    int newfd=-1;
    if(rebind) {
        struct vmr_mm_binding newer=source;
        newer.mm+=UINT64_C(1)<<32;newer.epoch++;
        struct execution_mm *next=source_mm_resolve(&newer);
        if(!next)return 2;
        newfd=next->backing_fd;
        memset(payload,0xa9,sizeof(payload));
        if(pwrite(newfd,payload,sizeof(payload),base)!=sizeof(payload))return 2;
        current=execution_target_publish(&ctx_memory[0],next,&newer,table_replace_object,NULL);
        if(!current)return 2;
    }
    page_lend(test_target(context),base,0);
    struct pg_claim claim={0}, other={0};
    if (pg_try_claim(context,base,PST_REQUESTING,&claim)<0) return 2;
    pthread_t server;
    if (pthread_create(&server,NULL,busy_get_service,&sockets[1])) return 2;
    struct vmr_pgreq request={.magic=VMR_PG_MAGIC,.op=op,
        .addr=base,.target=source,.owner=123};
    struct vmr_pgrsp response;
    if (pg_rw(sockets[0],&request,sizeof(request),1) ||
        pg_rw(sockets[0],&response,sizeof(response),0) ||
        pg_rw(sockets[0],payload,sizeof(payload),0)) return 2;
    shutdown(sockets[0],SHUT_RDWR);
    close(sockets[0]);
    void *result;
    if (pthread_join(server,&result) || result) return 2;
    int pass=vmr_page_reply_matches(&request,&response) && response.status==VMR_PG_INFLIGHT &&
        response.why==VMR_INFLIGHT_PULL &&
        pread(fd,after,sizeof(after),base)==sizeof(after) &&
        !memcmp(before,after,sizeof(before)) && !page_holder(test_target(context),base) &&
        !pgen_get(test_target(context),base) && pg_try_claim(context,base,PST_UNINSTALLING,&other)<0;
    if(rebind) {
        pass &= ctx_target(context)==current && current->source.mm!=source.mm;
        if(pread(newfd,after,sizeof(after),base)!=sizeof(after))return 2;
        for(unsigned i=0;i<sizeof(after);i++)pass &= after[i]==0xa9;
    }
    pg_finish_required(&claim,PST_OWNED);
    close(fd);
    printf("%s: busy GET preserves the incumbent transfer, bytes, loan and generation (status=%d)\n",
        pass ? "PASS" : "FAIL",response.status);
    return pass ? 0 : 1;
}

struct acknowledgement_peer {int fd,result;struct vmr_mm_binding target;};
static void *acknowledgement_peer_run(void *opaque)
{
    struct acknowledgement_peer *peer=opaque;struct vmr_req request;
    peer->result=pg_rw(peer->fd,&request,sizeof(request),0) || request.magic!=VMR_MAGIC ||
        request.nr!=VMR_OP_INSTALLED || request.args[0]!=0x4000 ||
        request.args[1]!=UINT64_C(0x800000010000beef) || !vmr_binding_equal(&request.target,&peer->target);
    if(!peer->result) {
        struct vmr_rsp response={.magic=VMR_MAGIC,.binding=peer->target};
        vmr_memory_receipt(&request,&response);
        peer->result=pg_rw(peer->fd,&response,sizeof(response),1);
    }
    return NULL;
}

static int acknowledgement_transport(void)
{
    int sockets[2];
    if (socketpair(AF_UNIX,SOCK_STREAM,0,sockets)) return 2;
    sock=sockets[0];
    ack_pend.target=test_target(7)->source;
    ack_pend.page=0x4000; ack_pend.episode=UINT64_C(0x800000010000beef); ack_pend.set=1;
    struct acknowledgement_peer peer={.fd=sockets[1],.target=ack_pend.target};
    pthread_t worker;
    if(pthread_create(&worker,NULL,acknowledgement_peer_run,&peer))return 2;
    ack_flush();
    if(pthread_join(worker,NULL) || peer.result || ack_pend.set || n_acks_sent!=1)return 1;
    ack_flush();
    char byte;
    if (recv(sockets[1],&byte,1,MSG_DONTWAIT)!=-1 || errno!=EAGAIN) return 1;
    close(sockets[1]);
    pid_t child=fork();
    if(child<0) return 2;
    if(!child) {
        ack_pend.page=0x5000; ack_pend.episode=UINT64_C(0x800000010000beef); ack_pend.set=1;
        ack_flush();
        _exit(0);
    }
    int status;
    if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)!=97) return 1;
    close(sock); sock=-1;
    puts("PASS: ACK confirms exactly once; broken transport ends the monitor before pretending the acknowledgement succeeded");
    return 0;
}

static int loan_scale(void)
{
    const unsigned pages = 32768, rounds = 4;
    const uint64_t base = 0x10000000;
    uint64_t start = own_now_us();
    for (unsigned r = 0; r < rounds; r++)
        for (unsigned i = 0; i < pages; i++) {
            uint64_t page = base + (uint64_t)i * VMR_PG_SIZE;
            page_lend(test_target(7), page, 41);
            if (page_holder(test_target(7), page) != 41) return 1;
            page_lend(test_target(7), page, 0);
        }
    uint64_t transitions_us = own_now_us() - start;
    /* No outstanding loans: this exercises the real pre-fork scan without
     * a network/kernel call. Scanning settled records must not recall them. */
    start = own_now_us();
    for (unsigned r = 0; r < 100; r++)
        page_recall_all(test_target(7));
    printf("{\"transitions\":%u,\"transitions_us\":%llu,\"empty_scans\":100,\"scans_us\":%llu}\n",
           pages * rounds, (unsigned long long)transitions_us,
           (unsigned long long)(own_now_us() - start));
    return 0;
}

static int cons_expect(pid_t as, uint64_t addr, int present,
                       uint64_t start, uint64_t end, uint64_t off,
                       uint64_t seq, uint32_t prot)
{
    uint64_t rs = 0, re = 0, ro = 0, rq = 0;
    uint32_t op = 0, rp = 0;
    int hit = cons_covering(as, addr, &rs, &re, &op, &rp, &ro, &rq);
    if (hit != present || (hit &&
        (rs != start || re != end || ro != off || rq != seq || rp != prot))) {
        fprintf(stderr, "FAIL: mapping as=%d addr=%llx hit=%d "
                "span=%llx-%llx off=%llx seq=%llu prot=%u\n", as,
                (unsigned long long)addr, hit, (unsigned long long)rs,
                (unsigned long long)re, (unsigned long long)ro,
                (unsigned long long)rq, rp);
        return 1;
    }
    return 0;
}

static int construction_capacity(void)
{
    for (unsigned i = 0; i < 10000; i++) {
        uint64_t page = 0x10000000 + (uint64_t)i * 8192;
        cons_note(test_target(7), page, page + 4096, i + 1, VMCTX_MAP_SET, 3, page);
    }
    for (unsigned i = 0; i < 10000; i++) {
        uint64_t page = 0x10000000 + (uint64_t)i * 8192;
        if (cons_expect(7, page, 1, page, page + 4096, page, i + 1, 3)) return 1;
    }
    puts("PASS: 10000 live mapping records remain replayable");
    return 0;
}

static int construction_partial(void)
{
    cons_note(test_target(7), 0x1000, 0x9000, 5, VMCTX_MAP_SET_SHARED, 3, 0x21000);
    cons_note(test_target(131079), 0x1000, 0x9000, 5, VMCTX_MAP_SET_SHARED, 1, 0x81000);
    cons_kill(7, 0x3000, 0x5000, 6);
    if (cons_expect(7, 0x2000, 1, 0x1000, 0x3000, 0x21000, 5, 3) ||
        cons_expect(7, 0x3000, 0, 0, 0, 0, 0, 0) ||
        cons_expect(7, 0x6000, 1, 0x5000, 0x9000, 0x25000, 5, 3) ||
        cons_expect(131079, 0x3000, 1, 0x1000, 0x9000, 0x81000, 5, 1)) return 1;
    cons_kill(7, 0x4000, 0x6000, 6);
    cons_kill(7, 0x8000, 0xa000, 6);
    if (cons_expect(7, 0x7000, 1, 0x6000, 0x8000, 0x26000, 5, 3)) return 1;
    cons_kill(7, 0, UINT64_MAX, 0);
    if (cons_expect(7, 0x7000, 0, 0, 0, 0, 0, 0) ||
        cons_expect(131079, 0x7000, 1, 0x1000, 0x9000, 0x81000, 5, 1)) return 1;
    puts("PASS: partial retirement preserves both tails, offsets and full AS identity");
    return 0;
}

static int construction_overlap(void)
{
    cons_note(test_target(7), 0x1000, 0x9000, 5, VMCTX_MAP_SET_SHARED, 3, 0x21000);
    cons_note(test_target(7), 0x3000, 0x5000, 7, VMCTX_MAP_SET_SHARED, 1, 0x83000);
    /* A late older reply cannot overwrite an exact newer range. */
    cons_note(test_target(7), 0x3000, 0x5000, 6, VMCTX_MAP_SET_SHARED, 0, 0x93000);
    if (cons_expect(7, 0x2000, 1, 0x1000, 0x3000, 0x21000, 5, 3) ||
        cons_expect(7, 0x3000, 1, 0x3000, 0x5000, 0x83000, 7, 1) ||
        cons_expect(7, 0x6000, 1, 0x5000, 0x9000, 0x25000, 5, 3)) return 1;
    cons_note(test_target(7), 0x5000, 0x6000, 8, 0, 0, 0);
    if (cons_expect(7, 0x5000, 0, 0, 0, 0, 0, 0)) return 1;
    cons_kill(7, 0x2000, 0x8000, 7);
    if (cons_expect(7, 0x3000, 1, 0x3000, 0x5000, 0x83000, 7, 1) ||
        cons_expect(7, 0x7000, 0, 0, 0, 0, 0, 0)) return 1;
    puts("PASS: replay clips older spans and preserves newer or opaque constructions");
    return 0;
}

static int construction_alloc_failure(void)
{
    for (unsigned arm = 0; arm < 3; arm++) {
        pid_t child = fork();
        if (child < 0) return 2;
        if (!child) {
            struct rlimit zero = {0, 0};
            setrlimit(RLIMIT_CORE, &zero);
            alarm(5);
            if (arm)
                for (unsigned i = 0; i < 128; i++)
                    cons_note(test_target(7), 0x10000 + i * 0x10000,
                              0x18000 + i * 0x10000, 5, VMCTX_MAP_SET, 3, 0);
            fail_table_alloc = 1;
            if (arm == 2)
                cons_kill(7, 0x12000, 0x14000, 6);
            else
                cons_note(test_target(7), 0x80000000, 0x80001000, 5, VMCTX_MAP_SET, 3, 0);
            _exit(91);
        }
        int status;
        if (waitpid(child, &status, 0) != child) return 2;
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
            fprintf(stderr, "FAIL: mapping allocation failure arm %u continued\n", arm);
            return 1;
        }
    }
    puts("PASS: initial, growth and split allocation failures stop execution");
    return 0;
}

static void *construction_worker(void *arg)
{
    pid_t as = 7 + (uintptr_t)arg * 131072;
    for (unsigned i = 0; i < 2000; i++) {
        uint64_t page = 0x10000000 + (uint64_t)i * 0x10000;
        cons_note(test_target(as), page, page + 0x8000, 5, VMCTX_MAP_SET, 3, page);
        cons_kill(as, page + 0x2000, page + 0x4000, 6);
        if (cons_expect(as, page + 0x5000, 1, page + 0x4000,
                        page + 0x8000, page + 0x4000, 5, 3))
            return (void *)1;
    }
    cons_kill(as, 0, UINT64_MAX, 0);
    return NULL;
}

static int construction_concurrent(void)
{
    pthread_t workers[4];
    alarm(30);
    for (uintptr_t i = 0; i < 4; i++)
        if (pthread_create(&workers[i], NULL, construction_worker, (void *)i)) _exit(2);
    for (unsigned i = 0; i < 4; i++) {
        void *result;
        if (pthread_join(workers[i], &result) || result) return 1;
    }
    puts("PASS: concurrent mapping publication, splits, growth and retirement");
    return 0;
}

static int mapping_effects(void)
{
    struct vmr_rsp response = {.magic = VMR_MAGIC, .retval = -1,
        .mapping = {.kind = VMR_MAP_PROTECT, .address = 0x40000,
                    .length = 0x1800, .protection = VMR_PROT_READ | VMR_PROT_EXEC}};
    struct vmctx_reply reply = {0};
    /* A negative syscall result does not suppress a source-declared effect.
     * No syscall number or argument exists in the executor's mapping API. */
    if (reply_note_map(test_target(7), &reply, &response) ||
        reply.map_op != VMCTX_MAP_PROT || reply.map_addr != 0x40000 ||
        reply.map_len != 0x2000 || reply.map_prot != (PROT_READ | PROT_EXEC))
        return 1;
    response.mapping.protection = 8;
    if (reply_note_map(test_target(7), &reply, &response) != -1) return 1;
    response.mapping.protection = 0;
    response.mapping.address = UINT64_MAX - 4095;
    if (reply_note_map(test_target(7), &reply, &response) != -1) return 1;
    response.mapping.address = 0x40000;
    response.mapping.kind = 77;
    if (reply_note_map(test_target(7), &reply, &response) != -1) return 1;
    puts("PASS: executor applies source effects independently of call results and rejects malformed effects");
    return 0;
}

static int execution_descriptors(void)
{
    int channel[2];
    if (pipe(channel)) return 2;
    int high=fcntl(channel[1],F_DUPFD_CLOEXEC,4096);
    int private=backing_new("execution-private"), shared=backing_new("execution-shared");
    if (high<0 || private<0 || shared<0) return 2;
    shared_obj=fcntl(shared,F_DUPFD_CLOEXEC,8192); close(shared);
    if (shared_obj<0) return 2;
    pid_t child=fork();if(child<0)return 2;
    if (!child) {
        alarm(3);
        drop_monitor_fds(private);
        if (fcntl(high,F_GETFD)!=-1 || errno!=EBADF ||
            fcntl(channel[0],F_GETFD)!=-1 || errno!=EBADF ||
            fcntl(private,F_GETFD)<0 || fcntl(shared_obj,F_GETFD)<0) _exit(1);
        usleep(500000);
        _exit(0);
    }
    close(high);close(channel[1]);
    struct pollfd p={.fd=channel[0],.events=POLLIN|POLLHUP};
    int status=0, ready=poll(&p,1,300);
    pid_t early=waitpid(child,&status,WNOHANG);
    int pass=ready==1 && (p.revents&POLLHUP) && early==0;
    if(early==0) { pid_t got;do {got=waitpid(child,&status,0);}while(got<0&&errno==EINTR);pass&=got==child; }
    pass&=WIFEXITED(status) && WEXITSTATUS(status)==0;
    close(channel[0]);close(private);close(shared_obj);shared_obj=-1;
    printf("%s: a living execution child retains its two objects and closes monitor channels above fd 4095\n",pass?"PASS":"FAIL");
    return pass?0:1;
}

static int execution_object_stdio(void)
{
    pid_t child=fork();if(child<0)return 2;
    if(!child) {
        close(0);close(1);close(2);
        int fd=backing_new("execution-object-with-closed-stdio");
        _exit(fd>=3 && (fcntl(fd,F_GETFD)&FD_CLOEXEC) ? 0 : 1);
    }
    int status;pid_t got;
    do {got=waitpid(child,&status,0);}while(got<0 && errno==EINTR);
    int pass=got==child && WIFEXITED(status) && !WEXITSTATUS(status);
    printf("%s: execution objects stay outside standard descriptors and close on exec\n",pass?"PASS":"FAIL");
    return pass?0:1;
}

/* Full MM keys can share every low 32-bit bit. Exercise the actual tables,
 * including retirement of just one key, before the wire starts supplying
 * those identities. Native context IDs do not participate in this fixture. */
static int mm_cache_identity(void)
{
    volatile uint64_t input=UINT64_C(0x100000007);
    uint64_t first=input,second=first|UINT64_C(0x8000000000000000);
    const uint64_t page=0x10000000;
    int pass=1,ok;
#define MM_CHECK(name, expression) do {ok=!!(expression);pass &= ok; \
    printf("%s: full-width MM keys in %s\n",ok?"PASS":"FAIL",name);}while(0)
    pthread_mutex_lock(&lent_lock);
    struct loan *a=loan_find(first,page,1),*b=loan_find(second,page,1);
    a->owner=41;b->owner=42;
    MM_CHECK("page loans",a!=b && (uint64_t)a->as==first && (uint64_t)b->as==second && a->owner==41);
    pthread_mutex_unlock(&lent_lock);
    lend_forget_range(first,page,page+4096);
    pthread_mutex_lock(&lent_lock);
    b=loan_find(second,page,0);
    MM_CHECK("loan retirement",!loan_find(first,page,0) && b && b->owner==42);
    pthread_mutex_unlock(&lent_lock);

    pthread_mutex_lock(&pgen_lock);
    struct pgen_ent *pa=pgen_find(first,page,1),*pb=pgen_find(second,page,1);
    pa->gen=19;pb->gen=23;
    MM_CHECK("capture generations",pa!=pb && (uint64_t)pa->as==first && (uint64_t)pb->as==second && pa->gen==19);
    pthread_mutex_unlock(&pgen_lock);
    pthread_mutex_lock(&retain_lock);
    struct retain_ent *ra=retain_find(first,page,1);ra->live=1;ra->bytes[0]='A';
    struct retain_ent *rb=retain_find(second,page,1);rb->live=1;rb->bytes[0]='B';
    MM_CHECK("retained bytes",ra!=rb && (uint64_t)ra->as==first && (uint64_t)rb->as==second && ra->bytes[0]=='A');
    pthread_mutex_unlock(&retain_lock);

    cow_give_note(first,page);
    MM_CHECK("COW completion",cow_was_given(first,page) && !cow_was_given(second,page));
    cow_give_note(second,page);
    cow_prot_set(first,page,COW_PROT_OWED);
    cow_prot_set(second,page,COW_PROT_SPENT);
    MM_CHECK("COW protection",cow_prot_state(first,page)==COW_PROT_OWED && cow_prot_state(second,page)==COW_PROT_SPENT);

    region_add(first,page,page+4096,PROT_READ);
    region_add(second,page,page+4096,PROT_WRITE);
    MM_CHECK("region layout",nregions==2 && regions[0].as==first && regions[1].as==second);
    pthread_mutex_lock(&layout_order_lock);
    struct access_cursor *ca=access_cursor_for(first),*cb=access_cursor_for(second);
    ca->acknowledged=19;cb->acknowledged=23;
    MM_CHECK("journal cursor",ca!=cb && (uint64_t)ca->as==first && (uint64_t)cb->as==second && ca->acknowledged==19);
    pthread_mutex_lock(&cons_lock);
    size_t ia=cons_slot_locked();
    cons[ia]=(struct construction){.as=first,.start=page,.end=page+4096,.seq=19,.op=VMCTX_MAP_SET,.prot=PROT_READ};
    size_t ib=cons_slot_locked();
    cons[ib]=(struct construction){.as=second,.start=page,.end=page+4096,.seq=23,.op=VMCTX_MAP_SET,.prot=PROT_WRITE};
    pthread_mutex_unlock(&cons_lock);
    uint64_t start,end,off,seq;uint32_t op,prot;
    MM_CHECK("mapping constructions",cons_covering(first,page,&start,&end,&op,&prot,&off,&seq) && seq==19 && prot==PROT_READ);
    cons_kill(first,page,page+4096,0);
    MM_CHECK("construction retirement",!cons_covering(first,page,&start,&end,&op,&prot,&off,&seq) &&
        cons_covering(second,page,&start,&end,&op,&prot,&off,&seq) && seq==23 && prot==PROT_WRITE);
    pthread_mutex_unlock(&layout_order_lock);
#undef MM_CHECK
    return pass?0:1;
}

static int cow_records(unsigned count,int collide)
{
    const uint64_t mm=UINT64_C(0x100000007),base=0x10000000;
    const uint64_t stride=VMR_PG_SIZE*(collide?(uint64_t)COW_GIVEN_SLOTS:1);
    unsigned given_missing=0,protection_missing=0;
    for(unsigned i=0;i<count;i++) {
        uint64_t page=base+(uint64_t)i*stride;
        if(collide && cow_given_slot(mm,page)!=cow_given_slot(mm,base))return 2;
        cow_give_note(mm,page);
        cow_prot_set(mm,page,COW_PROT_OWED);
    }
    for(unsigned i=0;i<count;i++) {
        uint64_t page=base+(uint64_t)i*stride;
        given_missing+=!cow_was_given(mm,page);
        protection_missing+=cow_prot_state(mm,page)!=COW_PROT_OWED;
    }
    int pass=!given_missing && !protection_missing;
    printf("%s: COW records count=%u collide=%d missing_given=%u missing_protection=%u\n",
        pass?"PASS":"FAIL",count,collide,given_missing,protection_missing);
    return pass?0:1;
}

static int cow_retirement(void)
{
    uint64_t first=UINT64_C(0x100000007),second=first|UINT64_C(0x8000000000000000);
    const uint64_t base=0x10000000,stride=(uint64_t)COW_GIVEN_SLOTS*VMR_PG_SIZE;
    for(unsigned i=0;i<128;i++) {
        uint64_t page=base+i*stride;
        cow_give_note(first,page);cow_give_note(second,page);
        cow_prot_set(first,page,COW_PROT_OWED);cow_prot_set(second,page,COW_PROT_SPENT);
    }
    cow_forget_mm(first);
    if(cow_record_count!=128)return 1;
    for(unsigned i=0;i<128;i++) {
        uint64_t page=base+i*stride;
        if(cow_was_given(first,page) || cow_prot_state(first,page) ||
           !cow_was_given(second,page) || cow_prot_state(second,page)!=COW_PROT_SPENT)return 1;
    }
    cow_prot_set(first,base+4095,COW_PROT_OWED);
    if(cow_was_given(first,base) || cow_prot_state(first,base)!=COW_PROT_OWED)return 1;
    cow_give_note(first,base+3);
    if(!cow_was_given(first,base+17) || cow_record_count!=129)return 1;
    cow_forget_mm(first);cow_forget_mm(second);
    if(cow_record_count)return 1;
    puts("PASS: COW retirement preserves colliding MM histories and normalized page keys");
    return 0;
}

static int cow_allocation_failure(void)
{
    for(unsigned arm=0;arm<2;arm++) {
        pid_t child=fork();if(child<0)return 2;
        if(!child) {
            struct rlimit zero={0,0};setrlimit(RLIMIT_CORE,&zero);alarm(5);
            if(arm)cow_give_note(7,0x10000);
            fail_table_alloc=1;
            if(arm) {
                cow_prot_set(7,0x10000,COW_PROT_OWED);
                if(!cow_was_given(7,0x10000) || !cow_is_protected(7,0x10000))_exit(1);
                cow_prot_set(7,0x20000,COW_PROT_OWED);
            } else cow_give_note(7,0x10000);
            _exit(1);
        }
        int status;pid_t got;do {got=waitpid(child,&status,0);}while(got<0 && errno==EINTR);
        if(got!=child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGABRT)return 1;
    }
    puts("PASS: COW allocation failure ends execution while existing records remain usable without allocation");
    return 0;
}

static pthread_barrier_t cow_barrier;
static void *cow_worker(void *opaque)
{
    uint64_t mm=((uintptr_t)opaque+1)<<32|7;
    const uint64_t shared=UINT64_C(0xff000000007),page=0x10000000;
    int failed=0;
    for(unsigned i=0;i<4096;i++) {cow_give_note(mm,page+i*4096);cow_prot_set(mm,page+i*4096,COW_PROT_OWED);}
    pthread_barrier_wait(&cow_barrier);
    cow_give_note(shared,page);
    for(unsigned i=0;i<4096;i++) {
        if(!cow_was_given(mm,page+i*4096) || !cow_is_protected(mm,page+i*4096))failed=1;
        cow_prot_set(mm,page+i*4096,COW_PROT_SPENT);
    }
    cow_forget_mm(mm);
    return (void *)(uintptr_t)failed;
}

static int cow_concurrent(void)
{
    pthread_t threads[8];int pass=1;
    if(pthread_barrier_init(&cow_barrier,NULL,8))return 2;
    for(unsigned i=0;i<8;i++)if(pthread_create(&threads[i],NULL,cow_worker,(void *)(uintptr_t)i))return 2;
    for(unsigned i=0;i<8;i++) {void *result;if(pthread_join(threads[i],&result))return 2;pass &= result==NULL;}
    pthread_barrier_destroy(&cow_barrier);
    pass &= cow_record_count==1 && n_cow_given==8*4096+1 && cow_was_given(UINT64_C(0xff000000007),0x10000000);
    printf("%s: parallel COW publication, updates and retirement preserve every MM and publish shared completion once\n",pass?"PASS":"FAIL");
    return pass?0:1;
}

int main(int argc, char **argv)
{
    (void)test_target(7); /* Allocate identity before injected table failures. */
    if(argc==2 && !strcmp(argv[1],"transfer-old-target"))return transfer_busy_get(VMR_PG_GET,1);
    if (argc == 2 && !strcmp(argv[1], "cow-retirement")) return cow_retirement();
    if (argc == 2 && !strcmp(argv[1], "cow-alloc-failure")) return cow_allocation_failure();
    if (argc == 2 && !strcmp(argv[1], "cow-concurrent")) return cow_concurrent();
    if (argc == 2 && !strcmp(argv[1], "cow-collisions")) return cow_records(128,1);
    if (argc == 2 && !strcmp(argv[1], "cow-capacity")) return cow_records(131072,0);
    if (argc == 2 && !strcmp(argv[1], "mm-cache-identity")) return mm_cache_identity();
    if (argc == 2 && !strcmp(argv[1], "execution-descriptors")) return execution_descriptors();
    if (argc == 2 && !strcmp(argv[1], "execution-object-stdio")) return execution_object_stdio();
    if (argc == 2 && !strcmp(argv[1], "acknowledgement-transport")) return acknowledgement_transport();
    if (argc == 2 && !strcmp(argv[1], "transfer-busy-gets")) return transfer_busy_get(VMR_PG_GETS,0);
    if (argc == 2 && !strcmp(argv[1], "transfer-busy-cow")) return transfer_busy_get(VMR_PG_COWBREAK,0);
    if (argc == 2 && !strcmp(argv[1], "transfer-busy-get")) return transfer_busy_get(VMR_PG_GET,0);
    if (argc == 2 && !strcmp(argv[1], "mapping-effects")) return mapping_effects();
    if (argc == 2 && !strcmp(argv[1], "construction-capacity")) return construction_capacity();
    if (argc == 2 && !strcmp(argv[1], "construction-partial")) return construction_partial();
    if (argc == 2 && !strcmp(argv[1], "construction-overlap")) return construction_overlap();
    if (argc == 2 && !strcmp(argv[1], "construction-alloc-failure")) return construction_alloc_failure();
    if (argc == 2 && !strcmp(argv[1], "construction-concurrent")) return construction_concurrent();
    if (argc == 2 && !strcmp(argv[1], "loan-delete"))
        return loan_delete();
    if (argc == 2 && !strcmp(argv[1], "loan-identity"))
        return loan_identity();
    if (argc == 2 && !strcmp(argv[1], "installed-delete"))
        return installed_delete();
    if (argc == 2 && !strcmp(argv[1], "installed-identity")) return installed_identity();
    if (argc == 2 && !strcmp(argv[1], "installed-normalize")) return installed_normalize();
    if (argc == 2 && !strcmp(argv[1], "installed-capacity")) return installed_capacity_test();
    if (argc == 2 && !strcmp(argv[1], "installed-collisions")) return installed_collisions();
    if (argc == 2 && !strcmp(argv[1], "installed-alloc-failure")) return installed_alloc_failure();
    if (argc == 2 && !strcmp(argv[1], "installed-scale")) return installed_scale();
    if (argc == 2 && !strcmp(argv[1], "installed-retirement")) return installed_retirement();
    if (argc == 2 && !strcmp(argv[1], "installed-concurrent")) return installed_concurrent();
    if (argc == 2 && !strcmp(argv[1], "loan-collisions")) return loan_collisions();
    if (argc == 2 && !strcmp(argv[1], "loan-capacity")) return loan_capacity();
    if (argc == 2 && !strcmp(argv[1], "loan-concurrent")) return loan_concurrent();
    if (argc == 2 && !strcmp(argv[1], "loan-alloc-failure")) return loan_alloc_failure();
    if (argc == 2 && !strcmp(argv[1], "loan-snapshot")) return loan_snapshot_lifetime();
    if (argc == 2 && !strcmp(argv[1], "transfer-exclusion")) return transfer_exclusion();
    if (argc == 2 && !strcmp(argv[1], "loan-scale")) return loan_scale();
    fprintf(stderr, "usage: %s CASE\n"
            "  loan-delete loan-identity loan-collisions loan-capacity loan-concurrent\n"
            "  loan-alloc-failure loan-snapshot loan-scale transfer-exclusion\n"
            "  installed-delete installed-identity installed-normalize installed-capacity\n"
            "  installed-collisions installed-alloc-failure installed-retirement\n"
            "  installed-concurrent installed-scale\n"
            "  construction-capacity construction-partial construction-overlap\n"
            "  construction-alloc-failure construction-concurrent\n", argv[0]);
    return 2;
}
