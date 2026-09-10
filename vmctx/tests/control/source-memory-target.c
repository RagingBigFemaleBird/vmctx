/* SPDX-License-Identifier: GPL-2.0 */
/* Actual target/registry helpers; a controlled native boundary forces a
 * representative to exec between INFO and MEMORY and counts inner commits. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "vmctx_context.h"
#include "vmctx_memory.h"
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL source target line %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static struct model {int fd;struct vmctx_context info;uint64_t value;} native[2];
static unsigned memory_calls,inner_commits,info_calls;
static int last_native,move_at_memory,fail_after_commit;
enum { READ_VALUE=12345 };
static long source_control_raw(pid_t selector,unsigned command,void *argument)
{
	int i;
	for(i=0;i<2;i++)if(selector==-native[i].fd-1)break;
	CHECK(i<2);
	if(command==VMCTX_CTL_CONTEXT) {
		struct vmctx_context *query=argument;
		CHECK(query->op==VMCTX_CONTEXT_INFO);
		*query=native[i].info;info_calls++;return 0;
	}
	CHECK(command==VMCTX_CTL_MEMORY);
	struct vmctx_memory *request=argument;
	CHECK(request->version==VMCTX_MEMORY_ABI && request->size==sizeof(*request) &&
	      request->op==VMCTX_MEMORY_CALL && request->command==READ_VALUE);
	memory_calls++;last_native=i;
	if(move_at_memory) {native[i].info.mm_identity++;move_at_memory=0;}
	if(request->mm_identity!=native[i].info.mm_identity) {errno=ESTALE;return -1;}
	inner_commits++;
	*(uint64_t *)(uintptr_t)request->argument=native[i].value;
	if(fail_after_commit) {errno=EFAULT;return -1;}
	return sizeof(uint64_t);
}
#include "../../user/source-memory-target.h"

int main(void)
{
	const uint64_t old=UINT64_C(0x100000007),fresh=UINT64_C(0x8000000100000007);
	source_id ids[2];
	for(unsigned i=0;i<2;i++) {
		native[i].fd=open("/dev/null",O_RDONLY|O_CLOEXEC);CHECK(native[i].fd>=0);
		native[i].info=(struct vmctx_context){.version=VMCTX_CONTEXT_ABI,
			.size=sizeof(struct vmctx_context),.op=VMCTX_CONTEXT_INFO,
			.identity=30+i,.mm_identity=old,.flags=VMCTX_CONTEXT_READY,.fd=-1};
		native[i].value=100+i;
		struct source_context handle={.fd=native[i].fd,.identity=native[i].info.identity};
		ids[i]=source_record_add(&handle);CHECK(ids[i]==(int)i+1 && handle.fd==-1);
	}
	struct source_memory_target target,lookup,current;
	CHECK(!source_memory_target_capture(ids[0],&target) && target.mm==old && target.epoch==1);
	uint64_t value=0;
	CHECK(source_memory_target_call(&target,READ_VALUE,&value)==8 && last_native==0 && value==100);
	/* The saved operation remains about the old MM after a callback execs
	 * its originating context. A surviving CLONE_VM peer can represent it. */
	native[0].info.mm_identity=fresh;
	CHECK(!source_memory_target_capture(ids[0],&current) && current.mm==fresh && current.epoch==2);
	CHECK(!source_memory_target_for_mm(ids[0],old,&lookup) && lookup.mm==target.mm && lookup.epoch==target.epoch);
	CHECK(source_memory_target_call(&target,READ_VALUE,&value)==8 && last_native==1 && value==101);
	CHECK(source_memory_target_call(&current,READ_VALUE,&value)==8 && last_native==0 && value==100);
	unsigned commits=inner_commits,calls=memory_calls;
	move_at_memory=1;value=99;
	CHECK(source_memory_target_call(&target,READ_VALUE,&value)==-1 && errno==ESTALE);
	CHECK(memory_calls==calls+1 && inner_commits==commits && value==99 && target.mm==old);
	native[1].info.mm_identity=old;fail_after_commit=1;calls=memory_calls;
	CHECK(source_memory_target_call(&target,READ_VALUE,&value)==-1 && errno==EFAULT);
	CHECK(memory_calls==calls+1 && inner_commits==commits+1 && value==101);
	fail_after_commit=0;
	native[1].info.flags=VMCTX_CONTEXT_ENDED;calls=memory_calls;
	CHECK(source_memory_target_call(&target,READ_VALUE,&value)==-1 && errno==ESRCH);
	CHECK(memory_calls==calls); /* No eligible task is not an MM retirement query. */
	struct source_memory_target forged=target;forged.epoch++;
	unsigned infos=info_calls;
	CHECK(source_memory_target_call(&forged,READ_VALUE,&value)==-1 && errno==ESTALE);
	CHECK(info_calls==infos && memory_calls==calls);
	CHECK(source_memory_target_for_mm(ids[0],old+99,&lookup)==-1 && errno==ESTALE);
	for(unsigned i=0;i<2;i++) {
		native[i].info.flags=VMCTX_CONTEXT_ENDED;source_record_release(ids[i]);
		CHECK(fcntl(native[i].fd,F_GETFD)==-1 && errno==EBADF);
		struct source_mm_binding *b=source_records[i]->binding;
		while(b) {struct source_mm_binding *previous=b->previous;free(b);b=previous;}
		free(source_records[i]);
	}
	free(source_records);
	puts("PASS: saved source MM targets select surviving peers, fence a raced exec, and never replay an uncertain native commit or refresh an old target");
	return 0;
}
