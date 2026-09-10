/* SPDX-License-Identifier: GPL-2.0 */
/* Actual source registry, with a native INFO boundary controlled by barriers.
 * The delayed observation is already copied before the newer query completes;
 * no sleeps or probabilistic native exec timing determine the outcome. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vmctx_context.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL source record line %d: %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
static pthread_mutex_t model_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t copied,release_copy;
static struct vmctx_context model;
static int model_error, model_fd;
static int fail_binding;
static void *binding_alloc(size_t size)
{ if(fail_binding) {errno=ENOMEM;return NULL;}return malloc(size); }
#define SOURCE_BINDING_ALLOC(size) binding_alloc(size)
static _Thread_local int delay_copy;
static long source_control_raw(pid_t selector,unsigned command,void *argument)
{
	struct vmctx_context *query=argument;
	CHECK(selector==-model_fd-1 && command==VMCTX_CTL_CONTEXT);
	CHECK(query->version==VMCTX_CONTEXT_ABI && query->size==sizeof(*query) &&
	      query->op==VMCTX_CONTEXT_INFO);
	pthread_mutex_lock(&model_lock);
	int error=model_error;
	*query=model;
	pthread_mutex_unlock(&model_lock);
	if(delay_copy) {
		pthread_barrier_wait(&copied);
		pthread_barrier_wait(&release_copy);
	}
	if(error) {errno=error;return -1;}
	return 0;
}
#include "../../user/source-registry.h"

struct observation {source_id id;struct vmctx_context info;uint64_t epoch;int result,error;};
static void *observe(void *opaque)
{
	struct observation *observation=opaque;
	delay_copy=1;
	observation->result=source_record_snapshot(observation->id,&observation->info,&observation->epoch);
	observation->error=errno;
	return NULL;
}

int main(int argc,char **argv)
{
	CHECK(argc==2);
	int terminal=!strcmp(argv[1],"terminal"),errors=!strcmp(argv[1],"errors");
	int allocation=!strcmp(argv[1],"allocation"),history=!strcmp(argv[1],"history");
	CHECK(terminal || errors || allocation || history || !strcmp(argv[1],"monotonic"));
	CHECK(!pthread_barrier_init(&copied,NULL,2));
	CHECK(!pthread_barrier_init(&release_copy,NULL,2));
	model_fd=open("/dev/null",O_RDONLY|O_CLOEXEC);CHECK(model_fd>=0);
	model=(struct vmctx_context){.version=VMCTX_CONTEXT_ABI,.size=sizeof(model),
		.op=VMCTX_CONTEXT_INFO,.flags=VMCTX_CONTEXT_READY,.identity=17,
		.mm_identity=UINT64_C(0x100000007),.fd=-1,.native_pid=123,.native_tgid=123};
	struct source_context native={.fd=model_fd,.identity=model.identity,.pid=123,.tgid=123};
	source_id id=source_record_add(&native);CHECK(id==1 && native.fd==-1);
	struct vmctx_context accepted;
	uint64_t epoch;
	CHECK(!source_record_snapshot(id,&accepted,&epoch) && epoch==1);
	CHECK(source_record_has_binding(id,model.mm_identity,1));
	CHECK(!source_record_has_binding(id,model.mm_identity,2));
	if(allocation || history) {
		const uint64_t original=model.mm_identity;
		for(uint64_t i=1;i<=(history ? 1100 : 1);i++) {
			model.mm_identity=original+(i<<32);
			if(allocation) {
				fail_binding=1;
				CHECK(source_record_snapshot(id,&accepted,&epoch)==-1 && errno==ENOMEM);
				CHECK(source_records[0]->binding->epoch==1 && source_records[0]->info.mm_identity==original);
				CHECK(source_records[0]->refs==1 && fcntl(model_fd,F_GETFD)>=0);
				fail_binding=0;
			}
			CHECK(!source_record_snapshot(id,&accepted,&epoch) && epoch==i+1);
			CHECK(source_record_has_binding(id,model.mm_identity,epoch));
			CHECK(!source_record_snapshot(id,&accepted,&epoch) && epoch==i+1);
			CHECK(source_record_has_binding(id,original,1));
		}
		if(allocation) {
			struct source_mm_binding *current=source_records[0]->binding;
			current->epoch=UINT64_MAX;model.mm_identity++;
			CHECK(source_record_snapshot(id,&accepted,&epoch)==-1 && errno==EOVERFLOW);
			CHECK(source_records[0]->binding==current && current->previous->epoch==1);
			model.mm_identity=current->mm;current->epoch=2;
		}
		model.flags=VMCTX_CONTEXT_ENDED;source_record_release(id);
		CHECK(source_record_has_binding(id,original,1));
		CHECK(!source_record_has_binding(id,original,2));
		CHECK(!source_record_has_binding(id+1,original,1));
		CHECK(fcntl(model_fd,F_GETFD)==-1 && errno==EBADF);
		puts(allocation ? "PASS: binding allocation failure and generation exhaustion leave the accepted binding and descriptor ownership intact" :
		     "PASS: 1101 immutable source bindings retain full MM identity and exact generations after exec observations and terminal release");
	} else if(errors) {
		model_error=EIO;
		CHECK(source_record_info(id,&accepted)==-1 && errno==EIO);
		CHECK(source_records[0]->refs==1 && fcntl(model_fd,F_GETFD)>=0);
		source_record_release(id);
		CHECK(source_record_info(id,&accepted)==-1 && errno==EIO);
		CHECK(fcntl(model_fd,F_GETFD)==-1 && errno==EBADF);
		puts("PASS: source INFO failure stays unknown through final release; no cached live observation invents terminal state");
	} else {
		struct observation older={.id=id};pthread_t worker;
		CHECK(!pthread_create(&worker,NULL,observe,&older));
		pthread_barrier_wait(&copied);
		pthread_mutex_lock(&model_lock);
		if(terminal) {model.flags=VMCTX_CONTEXT_ENDED;model.exit_status=42<<8;}
		else {model.mm_identity=UINT64_C(0x8000000100000007);model.native_pid=321;}
		pthread_mutex_unlock(&model_lock);
		CHECK(!source_record_snapshot(id,&accepted,&epoch));
		CHECK(epoch==(terminal ? 1 : 2));
		CHECK(terminal ? accepted.flags==VMCTX_CONTEXT_ENDED :
		      accepted.mm_identity==UINT64_C(0x8000000100000007));
		pthread_barrier_wait(&release_copy);CHECK(!pthread_join(worker,NULL));
		CHECK(!older.result && older.epoch==epoch);
		CHECK(older.info.mm_identity==accepted.mm_identity &&
		      older.info.native_pid==accepted.native_pid &&
		      older.info.flags==accepted.flags && older.info.exit_status==accepted.exit_status);
		CHECK(source_records[0]->refs==1);
		model.flags=VMCTX_CONTEXT_ENDED;model.exit_status=42<<8;
		source_record_release(id);
		CHECK(!source_record_info(id,&accepted) && accepted.flags==VMCTX_CONTEXT_ENDED);
		CHECK(fcntl(model_fd,F_GETFD)==-1 && errno==EBADF);
		puts(terminal ? "PASS: delayed live INFO cannot undo a published terminal record" :
		     "PASS: delayed old-MM INFO returns the accepted newer binding with its native name and full 64-bit identity");
	}
	struct source_mm_binding *binding=source_records[0]->binding;
	while(binding) {struct source_mm_binding *previous=binding->previous;free(binding);binding=previous;}
	free(source_records[0]);free(source_records);
	pthread_barrier_destroy(&copied);pthread_barrier_destroy(&release_copy);
	return 0;
}
