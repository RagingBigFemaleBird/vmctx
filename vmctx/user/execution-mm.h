/* SPDX-License-Identifier: GPL-2.0 */
/* Executor MM lifetimes. The source supplies identities and ancestry; the
 * local adapter supplies storage and native binding operations. No native PID
 * is an MM key. Records and their objects live until session shutdown, including
 * after their last execution binding: descendants and delayed transfers can
 * still own the old bytes. Shutdown requires every worker to have joined.
 *
 * A transfer saves its explicit MM pointer before any callback. Binding guards
 * cover only native controls that touch a context's current mapping. Never hold
 * one across a network call or a wait for source work. Rebinding drains those
 * controls, then changes one context; it never clears an old MM's object/state.
 */
#ifndef VMR_EXECUTION_MM_H
#define VMR_EXECUTION_MM_H
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

struct execution_mm {
	uint64_t identity;
	struct execution_mm *parent; /* immutable source-declared ancestry */
	int backing_fd;
	struct execution_mm *next;
};
#define EXECUTION_MM_BUCKETS 1024
struct execution_mm_registry {
	pthread_mutex_t lock;
	struct execution_mm *bucket[EXECUTION_MM_BUCKETS];
	size_t count;
};
#define EXECUTION_MM_REGISTRY_INIT {.lock=PTHREAD_MUTEX_INITIALIZER}

static inline unsigned execution_mm_bucket(uint64_t identity)
{
	identity^=identity>>33;
	identity*=UINT64_C(0xff51afd7ed558ccd);
	identity^=identity>>33;
	return identity%EXECUTION_MM_BUCKETS;
}

/* Caller holds registry.lock. Returned records are stable until shutdown. */
static inline struct execution_mm *execution_mm_find_locked(
		struct execution_mm_registry *registry,uint64_t identity)
{
	for (struct execution_mm *mm=registry->bucket[execution_mm_bucket(identity)];
	     mm;mm=mm->next)
		if(mm->identity==identity)return mm;
	return NULL;
}

static inline struct execution_mm *execution_mm_find(
		struct execution_mm_registry *registry,uint64_t identity)
{
	pthread_mutex_lock(&registry->lock);
	struct execution_mm *mm=execution_mm_find_locked(registry,identity);
	pthread_mutex_unlock(&registry->lock);
	if(!mm)errno=ENOENT;
	return mm;
}

/* Idempotent publication requires identical ancestry. The factory runs with
 * no registry/binding lock; competing publication closes only its unused new
 * object. An unknown parent is an error, never an invented root relationship. */
static inline struct execution_mm *execution_mm_resolve(
		struct execution_mm_registry *registry,uint64_t identity,
		uint64_t parent_identity,int (*make_object)(void *),void *opaque)
{
	if(!identity || identity==parent_identity) {errno=EINVAL;return NULL;}
	pthread_mutex_lock(&registry->lock);
	struct execution_mm *parent=parent_identity ?
		execution_mm_find_locked(registry,parent_identity) : NULL;
	struct execution_mm *mm=execution_mm_find_locked(registry,identity);
	int error=parent_identity && !parent ? ENOENT : mm && mm->parent!=parent ? EPROTO : 0;
	pthread_mutex_unlock(&registry->lock);
	if(error) {errno=error;return NULL;}
	if(mm)return mm;
	struct execution_mm *next=calloc(1,sizeof(*next));
	if(!next)return NULL;
	int fd=make_object(opaque);
	if(fd<0) {int saved=errno;free(next);errno=saved;return NULL;}
	*next=(struct execution_mm){.identity=identity,.parent=parent,.backing_fd=fd};
	pthread_mutex_lock(&registry->lock);
	mm=execution_mm_find_locked(registry,identity);
	if(mm && mm->parent!=parent)error=EPROTO;
	else if(!mm) {
		if(registry->count==SIZE_MAX)error=EOVERFLOW;
		else {
			unsigned bucket=execution_mm_bucket(identity);
			next->next=registry->bucket[bucket];
			registry->bucket[bucket]=next;registry->count++;mm=next;next=NULL;
		}
	}
	pthread_mutex_unlock(&registry->lock);
	if(next) {close(next->backing_fd);free(next);}
	if(error) {errno=error;return NULL;}
	return mm;
}

struct execution_binding {
	pthread_rwlock_t lock;
	struct execution_mm *mm;
	uint64_t epoch;
};

static inline int execution_binding_init(struct execution_binding *binding,
		struct execution_mm *mm,uint64_t epoch)
{
	if(!mm || !epoch) {errno=EINVAL;return -1;}
	int error=pthread_rwlock_init(&binding->lock,NULL);
	if(error) {errno=error;return -1;}
	binding->mm=mm;binding->epoch=epoch;
	return 0;
}

struct execution_mm_view { struct execution_mm *mm;uint64_t epoch; };
static inline struct execution_mm_view execution_binding_view(
		struct execution_binding *binding)
{
	pthread_rwlock_rdlock(&binding->lock);
	struct execution_mm_view view={binding->mm,binding->epoch};
	pthread_rwlock_unlock(&binding->lock);
	return view;
}

/* A stale control is refused before the native adapter sees it. Its caller
 * still owns view.mm's storage, and may use another binding of that same MM.
 * There is deliberately no retry against the context's new binding. */
static inline long execution_binding_control(struct execution_binding *binding,
		struct execution_mm_view view,long (*control)(void *),void *opaque)
{
	pthread_rwlock_rdlock(&binding->lock);
	long result=-1;
	if(binding->mm!=view.mm || binding->epoch!=view.epoch)errno=ESTALE;
	else result=control(opaque);
	int saved=errno;
	pthread_rwlock_unlock(&binding->lock);
	errno=saved;return result;
}

/* The native adapter must replace by expected epoch and return the committed
 * new epoch. It must leave the old binding intact on ordinary failure and end
 * execution on partial mutation. An unresolved commit returns -2, which ends
 * the monitor before another control can use an uncertain binding. It may
 * not call back into this registry or
 * wait for source work while this exclusive binding guard is held. */
static inline int execution_binding_replace(struct execution_binding *binding,
		struct execution_mm_view previous,struct execution_mm *next,
		int (*replace)(void *,int,uint64_t,uint64_t *),void *opaque)
{
	if(!next || next==previous.mm) {errno=EINVAL;return -1;}
	pthread_rwlock_wrlock(&binding->lock);
	int result=-1;
	uint64_t committed=0;
	if(binding->mm!=previous.mm || binding->epoch!=previous.epoch)errno=ESTALE;
	else if(previous.epoch==UINT64_MAX)errno=EOVERFLOW;
	else {
		int replaced=replace(opaque,next->backing_fd,previous.epoch,&committed);
		if(replaced < -1)abort();
		if(!replaced) {
		/* A successful adapter response must describe its exact commit.
		 * An invalid success cannot be undone safely; end the monitor. */
		if(committed!=previous.epoch+1)abort();
		binding->mm=next;binding->epoch=committed;result=0;
		}
	}
	int saved=errno;
	pthread_rwlock_unlock(&binding->lock);
	errno=saved;return result;
}

static inline void execution_mm_registry_destroy(struct execution_mm_registry *registry)
{
	for(unsigned i=0;i<EXECUTION_MM_BUCKETS;i++) {
		struct execution_mm *mm=registry->bucket[i];
		while(mm) {struct execution_mm *next=mm->next;close(mm->backing_fd);free(mm);mm=next;}
		registry->bucket[i]=NULL;
	}
	registry->count=0;
	pthread_mutex_destroy(&registry->lock);
}
#endif
