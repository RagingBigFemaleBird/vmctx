/* SPDX-License-Identifier: GPL-2.0 */
/* An old source lineage request cannot re-arm an already spent snapshot. */
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
    int spent=!strcmp(argv[1],"spent"),missing=!strcmp(argv[1],"missing");
    assert(spent || missing || !strcmp(argv[1],"owed"));
    memory_target parent=test_target(7);
    struct execution_mm *child=execution_mm_resolve(&executor_mms,8,7,test_mm_object,NULL);
    assert(child);cow_admit_mm(child);
    unsigned char original[4096],copy[4096];memset(original,0x63,sizeof(original));
    if(!missing)assert(pwrite(parent->view.mm->backing_fd,original,4096,0x4000)==4096);
    cow_prot_set(7,0x4000,spent ? COW_PROT_SPENT:COW_PROT_OWED);
    coh_enter();int result=cow_break_copies(parent,0x4000);coh_leave();
    int pass=result==!missing && !coh_depth;
    if(spent || missing)pass &= !cow_was_given(8,0x4000) && !mm_backing_has(8,0x4000);
    else {
        pass &= cow_was_given(8,0x4000) && pread(child->backing_fd,copy,4096,0x4000)==4096;
        pass &= !memcmp(original,copy,4096) && cow_is_protected(8,0x4000);
    }
    pass &= cow_prot_state(7,0x4000)==(spent ? COW_PROT_SPENT:COW_PROT_OWED);
    printf("%s: %s snapshot result=%d given=%d; spent requests cannot copy current bytes, missing owed pages still fail\n",
        pass?"PASS":"FAIL",argv[1],result,cow_was_given(8,0x4000));
    return pass?0:1;
}
