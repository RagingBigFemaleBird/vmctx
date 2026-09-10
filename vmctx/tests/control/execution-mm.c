/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "../../user/execution-mm.h"
#include "../../user/linux-execution-fds.h"

#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL execution MM line %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static struct execution_mm_registry registry=EXECUTION_MM_REGISTRY_INIT;
static int make_object(void *unused)
{
	(void)unused;
	int fd=linux_execution_object_fd("execution-mm-control");
	if(fd>=0 && ftruncate(fd,8192)) {int saved=errno;close(fd);errno=saved;return -1;}
	return fd;
}
static int failed_object(void *unused) {(void)unused;errno=ENOMEM;return -1;}
static void put(int fd,char byte) {CHECK(pwrite(fd,&byte,1,4096)==1);}
static char get(int fd) {char byte;CHECK(pread(fd,&byte,1,4096)==1);return byte;}
struct native {int object,calls,fail;};
static int replace(void *opaque,int fd,uint64_t epoch,uint64_t *committed)
{
	struct native *native=opaque;
	native->calls++;
	if(native->fail) {errno=EIO;return -1;}
	native->object=fd;*committed=epoch+1;
	return 0;
}
static long native_write(void *opaque)
{struct native *native=opaque;native->calls++;put(native->object,'X');return 0;}

struct transfer {
	struct execution_binding *binding;
	struct execution_mm_view view;
	pthread_barrier_t before,after;
	struct native *native;
};
static void *late_transfer(void *opaque)
{
	struct transfer *transfer=opaque;
	/* Save a whole MM identity before the simulated wire round trip. */
	transfer->view=execution_binding_view(transfer->binding);
	pthread_barrier_wait(&transfer->before);
	pthread_barrier_wait(&transfer->after);
	put(transfer->view.mm->backing_fd,'L');
	CHECK(execution_binding_control(transfer->binding,transfer->view,native_write,transfer->native)==-1 && errno==ESTALE);
	return NULL;
}

struct resolve_arg {pthread_barrier_t *barrier;struct execution_mm *mm;};
static void *resolve_worker(void *opaque)
{
	struct resolve_arg *arg=opaque;
	pthread_barrier_wait(arg->barrier);
	arg->mm=execution_mm_resolve(&registry,UINT64_C(0xf123456789abcdef),0,make_object,NULL);
	CHECK(arg->mm);
	return NULL;
}

int main(void)
{
	const uint64_t first=UINT64_C(0x100000007),second=UINT64_C(0x8000000000000007);
	struct execution_mm *old=execution_mm_resolve(&registry,first,0,make_object,NULL);
	struct execution_mm *fresh=execution_mm_resolve(&registry,second,0,make_object,NULL);
	CHECK(old && fresh && old!=fresh && old->backing_fd!=fresh->backing_fd);
	CHECK(execution_mm_find(&registry,first)==old && execution_mm_find(&registry,second)==fresh);
	CHECK(execution_mm_resolve(&registry,first,0,failed_object,NULL)==old);
	CHECK(!execution_mm_resolve(&registry,first,second,make_object,NULL) && errno==EPROTO);
	CHECK(!execution_mm_resolve(&registry,99,101,make_object,NULL) && errno==ENOENT);
	CHECK(!execution_mm_resolve(&registry,99,0,failed_object,NULL) && errno==ENOMEM);
	CHECK(!execution_mm_find(&registry,99) && registry.count==2);
	struct execution_mm *copy=execution_mm_resolve(&registry,99,first,make_object,NULL);
	CHECK(copy && copy->parent==old);
	put(old->backing_fd,'O');put(fresh->backing_fd,'N');put(copy->backing_fd,'C');
	struct execution_binding moving,peer;
	CHECK(!execution_binding_init(&moving,old,7));CHECK(!execution_binding_init(&peer,old,1));
	struct native native={.object=old->backing_fd};
	struct transfer transfer={.binding=&moving,.native=&native};
	CHECK(!pthread_barrier_init(&transfer.before,NULL,2));CHECK(!pthread_barrier_init(&transfer.after,NULL,2));
	pthread_t thread;CHECK(!pthread_create(&thread,NULL,late_transfer,&transfer));
	pthread_barrier_wait(&transfer.before);
	struct execution_mm_view initial=transfer.view;
	native.fail=1;
	CHECK(execution_binding_replace(&moving,initial,fresh,replace,&native)==-1 && errno==EIO);
	CHECK(execution_binding_view(&moving).mm==old && native.object==old->backing_fd);
	native.fail=0;
	CHECK(!execution_binding_replace(&moving,initial,fresh,replace,&native));
	CHECK(execution_binding_view(&moving).mm==fresh && execution_binding_view(&moving).epoch==8);
	CHECK(execution_binding_view(&peer).mm==old && copy->parent==old);
	CHECK(get(old->backing_fd)=='O' && get(fresh->backing_fd)=='N' && get(copy->backing_fd)=='C');
	pthread_barrier_wait(&transfer.after);CHECK(!pthread_join(thread,NULL));
	CHECK(native.calls==2 && get(old->backing_fd)=='L' && get(fresh->backing_fd)=='N');
	CHECK(execution_binding_replace(&moving,initial,copy,replace,&native)==-1 && errno==ESTALE);
	CHECK(native.calls==2);
	CHECK(!execution_binding_control(&moving,execution_binding_view(&moving),native_write,&native));
	CHECK(get(fresh->backing_fd)=='X' && get(old->backing_fd)=='L');
	struct native peer_native={.object=old->backing_fd};
	CHECK(!execution_binding_control(&peer,execution_binding_view(&peer),native_write,&peer_native));
	CHECK(get(old->backing_fd)=='X' && get(copy->backing_fd)=='C');

	pthread_barrier_t barrier;CHECK(!pthread_barrier_init(&barrier,NULL,8));
	struct resolve_arg args[8];pthread_t workers[8];
	for(unsigned i=0;i<8;i++) {args[i]=(struct resolve_arg){.barrier=&barrier};CHECK(!pthread_create(&workers[i],NULL,resolve_worker,&args[i]));}
	for(unsigned i=0;i<8;i++) {CHECK(!pthread_join(workers[i],NULL));CHECK(args[i].mm==args[0].mm);}
	CHECK(registry.count==4);
	/* A chain grows beyond the old 1024-entry lineage limit; pointer ancestry
	 * and colliding low 32-bit identity portions remain distinct throughout. */
	struct execution_mm *parent=copy;
	for(uint64_t i=0;i<1100;i++) {
		struct execution_mm *mm=execution_mm_resolve(&registry,(i+100)<<32|99,parent->identity,make_object,NULL);
		CHECK(mm && mm->parent==parent);parent=mm;
	}
	CHECK(registry.count==1104 && get(old->backing_fd)=='X');
	int fd=old->backing_fd;
	pthread_rwlock_destroy(&moving.lock);pthread_rwlock_destroy(&peer.lock);
	pthread_barrier_destroy(&transfer.before);pthread_barrier_destroy(&transfer.after);pthread_barrier_destroy(&barrier);
	execution_mm_registry_destroy(&registry);
	CHECK(fcntl(fd,F_GETFD)==-1 && errno==EBADF);
	puts("PASS: immutable 64-bit MMs preserve old transfers, surviving peers and deep ancestry through context rebinding; stale controls and failed publication cannot mutate new storage");
	return 0;
}
