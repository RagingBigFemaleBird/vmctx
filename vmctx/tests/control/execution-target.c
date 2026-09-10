/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
static _Atomic int fail_allocation;
static void *target_alloc(size_t size)
{if(atomic_load(&fail_allocation)) {errno=ENOMEM;return NULL;}return malloc(size);}
#define EXECUTION_TARGET_ALLOC target_alloc
#include "../../user/execution-target.h"
#include "../../user/linux-execution-fds.h"
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL execution target line %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)

static int make_object(void *unused)
{(void)unused;return linux_execution_object_fd("execution-target-test");}
static void put(struct execution_mm *mm,char byte)
{CHECK(pwrite(mm->backing_fd,&byte,1,4096)==1);}
static char get(struct execution_mm *mm)
{char byte;CHECK(pread(mm->backing_fd,&byte,1,4096)==1);return byte;}
struct native {
	int object,calls,fail;
	pthread_barrier_t *changed,*release;
};
static int replace(void *opaque,int fd,uint64_t expected,uint64_t *epoch)
{
	struct native *native=opaque;native->calls++;
	if(native->fail) {errno=EBUSY;return -1;}
	native->object=fd;
	if(native->changed) {
		pthread_barrier_wait(native->changed);
		pthread_barrier_wait(native->release);
	}
	*epoch=expected+1;return 0;
}
static long write_native(void *opaque)
{
	struct native *native=opaque;native->calls++;
	char byte='W';return pwrite(native->object,&byte,1,4096);
}
struct publication {
	struct execution_context_binding *context;
	struct execution_mm *mm;
	struct vmr_mm_binding source;
	struct native *native;
	const struct execution_target *result;
};
static void *publish(void *opaque)
{
	struct publication *p=opaque;
	p->result=execution_target_publish(p->context,p->mm,&p->source,replace,p->native);
	CHECK(p->result);return NULL;
}
struct capture {struct execution_context_binding *context;const struct execution_target *result;};
static void *capture(void *opaque)
{struct capture *c=opaque;c->result=execution_target_capture(c->context);return NULL;}

int main(void)
{
	struct execution_mm_registry registry=EXECUTION_MM_REGISTRY_INIT;
	struct execution_mm *a=execution_mm_resolve(&registry,UINT64_C(0x100000007),0,make_object,NULL);
	struct execution_mm *b=execution_mm_resolve(&registry,UINT64_C(0x8000000000000007),0,make_object,NULL);
	struct execution_mm *c=execution_mm_resolve(&registry,UINT64_C(0x7000000000000007),a->identity,make_object,NULL);
	CHECK(a && b && c);put(a,'A');put(b,'B');put(c,'C');
	struct vmr_mm_binding sa={.context=UINT64_C(0xabcdef1234567890),.mm=a->identity,.epoch=3};
	struct vmr_mm_binding sb={.context=sa.context,.mm=b->identity,.epoch=4};
	struct vmr_mm_binding sc={.context=sa.context,.mm=c->identity,.parent_mm=a->identity,.epoch=5};
	struct execution_context_binding moving,peer;
	CHECK(!execution_context_binding_init(&moving,1,a,&sa,7));
	struct vmr_mm_binding peer_source=sa;peer_source.context++;
	CHECK(!execution_context_binding_init(&peer,2,a,&peer_source,1));
	const struct execution_target *old=execution_target_capture(&moving);
	const struct execution_target *observed=execution_target_observe(&moving,b,&sb);
	CHECK(old && observed && !observed->view.epoch && observed->view.mm==b);
	CHECK(execution_target_capture(&moving)==old && old->source.epoch==3 && old->view.epoch==7);
	struct native native={.object=a->backing_fd};
	CHECK(execution_target_control(observed,write_native,&native)==-1 && errno==ESTALE && !native.calls);
	atomic_store(&fail_allocation,1);
	CHECK(execution_target_observe(&moving,b,&sb)==observed);
	CHECK(!execution_target_publish(&moving,b,&sb,replace,&native) && errno==ENOMEM);
	CHECK(execution_target_capture(&moving)==old && !native.calls);
	atomic_store(&fail_allocation,0);native.fail=1;
	CHECK(!execution_target_publish(&moving,b,&sb,replace,&native) && errno==EBUSY);
	CHECK(execution_target_capture(&moving)==old && native.calls==1 && native.object==a->backing_fd);
	native.fail=0;
	pthread_barrier_t changed,release;
	CHECK(!pthread_barrier_init(&changed,NULL,2));CHECK(!pthread_barrier_init(&release,NULL,2));
	native.changed=&changed;native.release=&release;
	struct publication publication={&moving,b,sb,&native,NULL};pthread_t publisher,reader;
	CHECK(!pthread_create(&publisher,NULL,publish,&publication));
	pthread_barrier_wait(&changed);
	/* The adapter has changed native storage but not returned its epoch.
	 * Capturing the still-old source publication must be excluded here. */
	CHECK(pthread_mutex_trylock(&moving.publication_lock)==EBUSY);
	CHECK(pthread_rwlock_tryrdlock(&moving.native.lock)==EBUSY);
	struct capture captured={.context=&moving};
	CHECK(!pthread_create(&reader,NULL,capture,&captured));
	pthread_barrier_wait(&release);
	CHECK(!pthread_join(publisher,NULL));CHECK(!pthread_join(reader,NULL));
	const struct execution_target *current=publication.result;
	CHECK(captured.result==current && current!=observed && current->source.epoch==4 &&
	      current->view.epoch==8 && current->view.mm==b && native.calls==2);
	CHECK(observed->view.epoch==0 && old->view.mm==a && old->view.epoch==7);
	CHECK(execution_target_observe(&moving,b,&sb)==current);
	CHECK(execution_target_publish(&moving,a,&sa,replace,&native)==old && native.calls==2);
	put(old->view.mm,'L');
	CHECK(execution_target_control(old,write_native,&native)==-1 && errno==ESTALE && native.calls==2);
	CHECK(get(a)=='L' && get(b)=='B' && get(c)=='C');
	CHECK(execution_target_control(current,write_native,&native)==1 && get(b)=='W');
	struct native peer_native={.object=a->backing_fd};
	CHECK(execution_target_control(execution_target_capture(&peer),write_native,&peer_native)==1 && get(a)=='W');
	struct vmr_mm_binding forged=sb;forged.context++;
	CHECK(!execution_target_observe(&moving,b,&forged) && errno==EPROTO);
	forged=sc;forged.epoch=sb.epoch;
	CHECK(!execution_target_publish(&moving,c,&forged,replace,&native) && errno==EPROTO);
	forged=sb;forged.epoch++;
	CHECK(!execution_target_publish(&moving,b,&forged,replace,&native) && errno==EPROTO);
	struct execution_mm alias=*b;
	CHECK(!execution_target_observe(&moving,&alias,&sb) && errno==EPROTO);
	forged=sa;forged.epoch=1;
	const struct execution_target *unseen=execution_target_publish(&moving,a,&forged,replace,&native);
	CHECK(unseen && unseen->view.epoch==0 && execution_target_capture(&moving)==current);
	CHECK(execution_target_control(unseen,write_native,&native)==-1 && errno==ESTALE);
	native.changed=native.release=NULL;
	unsigned before=native.calls;
	struct publication repeated[16];pthread_t threads[16];
	for(unsigned i=0;i<16;i++) {
		repeated[i]=(struct publication){&moving,c,sc,&native,NULL};
		CHECK(!pthread_create(&threads[i],NULL,publish,&repeated[i]));
	}
	for(unsigned i=0;i<16;i++) {
		CHECK(!pthread_join(threads[i],NULL));CHECK(repeated[i].result==repeated[0].result);
	}
	CHECK((unsigned)native.calls==before+1 && repeated[0].result->view.epoch==9 &&
	      repeated[0].result->source.epoch==5 && repeated[0].result->view.mm==c);
	struct execution_context_binding exhausted;
	CHECK(!execution_context_binding_init(&exhausted,3,a,&sa,UINT64_MAX));before=native.calls;
	CHECK(!execution_target_publish(&exhausted,b,&sb,replace,&native) && errno==EOVERFLOW);
	CHECK((unsigned)native.calls==before && execution_target_capture(&exhausted)->view.mm==a);
	execution_context_binding_destroy(&exhausted);
	execution_context_binding_destroy(&moving);execution_context_binding_destroy(&peer);
	pthread_barrier_destroy(&changed);pthread_barrier_destroy(&release);
	execution_mm_registry_destroy(&registry);
	puts("PASS: immutable execution targets publish source/native epochs atomically, retain old storage through callbacks, reject stale controls and contradictory history, and leave failed commits unpublished");
	return 0;
}
