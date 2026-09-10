/* SPDX-License-Identifier: GPL-2.0 */
/* New mappings cannot inherit retained bytes or loan history at a reused VA. */
#define _GNU_SOURCE
#include <assert.h>
#define main vmremote_program_main
#ifndef VMREMOTE_SOURCE
#define VMREMOTE_SOURCE "../../user/vmremote.c"
#endif
#include VMREMOTE_SOURCE
#undef main
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"

int main(int argc,char **argv)
{
    assert(argc==2);alarm(10);coh_lock_init();
    memory_target target=test_target(7),other=test_target(8);
    const uint64_t page=0x4000;
    unsigned char old[4096],incoming[4096],expected[4096];
    memset(old,0x31,sizeof(old));memset(incoming,0xa6,sizeof(incoming));
    memcpy(expected,incoming,sizeof(expected));
    assert(pgen_bump(target,page)==1);retain_put(target,page,old);
    if(!strcmp(argv[1],"diagnostic")) {
        /* After the trace quota is exhausted, the same production decision
         * still runs; no source endpoint is needed for this byte oracle. */
        n_gen_stale=16;
        pull_gen_check(target,page,incoming,0,31);
        int pass=!memcmp(incoming,expected,sizeof(expected)) && n_gen_stale==17 &&
            !n_gen_repaired && !n_gen_repaired_obj;
        printf("%s: generation disagreement is reported without substituting retained bytes\n",pass?"PASS":"FAIL");
        return !pass;
    }
    assert(!strcmp(argv[1],"replace"));
    assert(obj_write(target,page,old,sizeof(old))==4096);
    page_lend_at(target,page,91,0,__func__,__LINE__);
    assert(pgen_bump(other,page)==1);retain_put(other,page,old);
    retain_put(target,page-4096,old);retain_put(target,page+4096,old);
    struct vmr_rsp response={.magic=VMR_MAGIC,.mmseq=9,
        .mapping={.kind=VMR_MAP_SET,.address=page,.length=4096,
            .object_offset=page,.protection=VMR_PROT_READ|VMR_PROT_WRITE}};
    struct vmctx_reply reply={0};
    pthread_mutex_lock(&layout_order_lock);
    int result=reply_note_map(target,&reply,&response);
    pthread_mutex_unlock(&layout_order_lock);
    int pass=!result && reply.map_op==VMCTX_MAP_SET && reply.map_addr==page &&
        reply.map_len==4096 && !backing_has(target,page) &&
        !retain_get(target,page,NULL) && !pgen_get(target,page) &&
        page_lent_state(target,page)==LENT_UNSEEN &&
        retain_get(target,page-4096,NULL) && retain_get(target,page+4096,NULL) &&
        retain_get(other,page,NULL) && pgen_get(other,page)==1;
    printf("%s: private mapping publication retires old bytes and loans only in its MM and range\n",pass?"PASS":"FAIL");
    return !pass;
}
