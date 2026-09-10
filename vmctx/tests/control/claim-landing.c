/* SPDX-License-Identifier: GPL-2.0 */
/* An existing backing folio is not a receipt for an unresolved transfer. */
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
    memory_target a=test_target(7),b=test_target(UINT64_C(0x100000007));
    uint64_t address=0x4000;
    unsigned char bytes[4096];memset(bytes,0x51,sizeof(bytes));
    assert(obj_write(a,address,bytes,sizeof(bytes))==4096);
    assert(obj_write(b,address,bytes,sizeof(bytes))==4096);
    page_lend(a,address,44);page_lend(b,address,45);
    for(unsigned i=0;i<2000;i++)assert(claim_yield_pid(a,address)==1);
    int identity=!strcmp(argv[1],"mm-identity");
    int result=claim_yield_pid(identity?b:a,address);
    int pass=result==1 && n_claim_gaveup==(identity?0:1) &&
        page_holder(a,address)==44 && page_holder(b,address)==45;
    printf("%s: %s unresolved claim result=%d throttles=%lu; preexisting folios confer no new ownership\n",
        pass?"PASS":"FAIL",argv[1],result,n_claim_gaveup);
    return pass?0:1;
}
