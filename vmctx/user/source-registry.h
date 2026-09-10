/* SPDX-License-Identifier: GPL-2.0 */
/* Source IDs are monitor-local, monotonically allocated context references.
 * They are neither native PIDs, descriptors, nor MM identities. A connection
 * or pending birth owns the initial reference; its fault service and transient
 * control operations take their own references. Closing the last reference
 * releases the native descriptor. Small terminal records keep old IDs from
 * aliasing a later task and allow lineage to outlive a task representative. */
#ifndef VMR_SOURCE_REGISTRY_H
#define VMR_SOURCE_REGISTRY_H
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include "source-context.h"
#include "source-memory.h"

typedef int source_id;
/* Each observation of a new native MM publishes one immutable binding.
 * Keep its history after exec and task retirement so delayed operations can
 * validate their saved context/MM/generation without consulting current MM.
 * These are monitor generations, independent of native MM IDs and PIDs. */
struct source_mm_binding {
	uint64_t mm, epoch;
	struct source_mm_binding *previous;
};
struct source_record {
	source_id id;
	struct source_context native;
	struct vmctx_context info;
	struct source_mm_binding *binding;
	unsigned refs;
	int retiring, info_error, connection_claimed;
};
static struct source_record **source_records;
static size_t source_records_n, source_records_capacity;
static pthread_mutex_t source_records_lock=PTHREAD_MUTEX_INITIALIZER;
#ifndef SOURCE_BINDING_ALLOC
#define SOURCE_BINDING_ALLOC(size) malloc(size)
#endif

/* Called under the registry lock. An older concurrent INFO result cannot
 * undo a newer MM binding or a published terminal record. */
static inline int source_record_cache(struct source_record *record,
		const struct vmctx_context *info)
{
	if(record->info.flags & VMCTX_CONTEXT_ENDED)return 0;
	/* Native MM IDs are allocated monotonically and a task never rebinds
	 * to an earlier MM. A terminal query describes its final binding. */
	if(info->mm_identity<record->info.mm_identity) {
		if(info->flags & VMCTX_CONTEXT_ENDED) {errno=EPROTO;return -1;}
		return 0;
	}
	if(info->mm_identity && (!record->binding ||
	   info->mm_identity!=record->binding->mm)) {
		if(record->binding && record->binding->epoch==UINT64_MAX) {
			errno=EOVERFLOW;return -1;
		}
		struct source_mm_binding *binding=SOURCE_BINDING_ALLOC(sizeof(*binding));
		if(!binding)return -1;
		*binding=(struct source_mm_binding){.mm=info->mm_identity,
			.epoch=record->binding ? record->binding->epoch+1 : 1,
			.previous=record->binding};
		record->binding=binding;
	}
	record->info=*info;
	return 0;
}

/* Consumes the native descriptor on every outcome. */
static inline source_id source_record_add(struct source_context *native)
{
	struct vmctx_context info;
	struct source_record *record=NULL;
	source_id id=0;
	int saved;
	if (source_context_info(native,&info)) goto done;
	if (!(info.flags & VMCTX_CONTEXT_ENDED) && info.native_tgid)
		native->tgid=(pid_t)info.native_tgid;
	record=calloc(1,sizeof(*record));
	if (!record) goto done;
	pthread_mutex_lock(&source_records_lock);
	if (source_records_n>=INT_MAX) { errno=EOVERFLOW; goto unlock; }
	if (source_records_n==source_records_capacity) {
		size_t capacity=source_records_capacity ? source_records_capacity*2 : 64;
		if (capacity>INT_MAX) capacity=INT_MAX;
		if (capacity>SIZE_MAX/sizeof(*source_records)) { errno=EOVERFLOW; goto unlock; }
		struct source_record **grown=realloc(source_records,capacity*sizeof(*grown));
		if (!grown) goto unlock;
		source_records=grown;
		source_records_capacity=capacity;
	}
	if(source_record_cache(record,&info))goto unlock;
	id=(source_id)source_records_n+1;
	record->id=id;record->native=*native;record->refs=1;
	source_records[source_records_n++]=record;
	*native=(struct source_context){.fd=-1};
unlock:
	pthread_mutex_unlock(&source_records_lock);
done:
	saved=errno;
	if (!id) free(record);
	source_context_close(native);
	errno=saved;
	return id;
}

static inline struct source_record *source_record_get(source_id id)
{
	struct source_record *record=NULL;
	pthread_mutex_lock(&source_records_lock);
	if (id<=0 || (size_t)id>source_records_n) { errno=ESRCH; goto done; }
	record=source_records[id-1];
	if (!record->refs || record->retiring) { record=NULL; errno=ESRCH; goto done; }
	if (record->refs==UINT_MAX) { record=NULL; errno=EOVERFLOW; goto done; }
	record->refs++;
done:
	pthread_mutex_unlock(&source_records_lock);
	return record;
}

/* Transfer the pending birth's initial reference to exactly one connection.
 * Other channels may borrow the record, but cannot claim the same birth. */
static inline int source_record_claim(source_id id)
{
	int result=-1;
	pthread_mutex_lock(&source_records_lock);
	if (id<=0 || (size_t)id>source_records_n) errno=ESRCH;
	else {
		struct source_record *record=source_records[id-1];
		if (!record->refs || record->retiring) errno=ESRCH;
		else if (record->connection_claimed) errno=EALREADY;
		else { record->connection_claimed=1; result=0; }
	}
	pthread_mutex_unlock(&source_records_lock);
	return result;
}

static inline void source_record_put(struct source_record *record)
{
	if (!record) return;
	pthread_mutex_lock(&source_records_lock);
	if (!record->refs || record->retiring) abort();
	if (--record->refs) { pthread_mutex_unlock(&source_records_lock); return; }
	record->retiring=1;
	pthread_mutex_unlock(&source_records_lock);
	/* No native operation runs while holding the registry lock. refs==0
	 * excludes new borrowers until this terminal snapshot is published. */
	struct vmctx_context info;
	int error=source_context_info(&record->native,&info) ? errno : 0;
	source_context_close(&record->native);
	pthread_mutex_lock(&source_records_lock);
	if (!error && source_record_cache(record,&info))error=errno;
	record->info_error=error;
	record->retiring=0;
	pthread_mutex_unlock(&source_records_lock);
}

static inline int source_record_snapshot(source_id id, struct vmctx_context *info,
		uint64_t *binding_epoch)
{
	struct source_record *record=source_record_get(id);
	if (!record) {
		int error=ESRCH;
		pthread_mutex_lock(&source_records_lock);
		if (id>0 && (size_t)id<=source_records_n) {
			record=source_records[id-1];
			error=record->refs ? EOVERFLOW : record->retiring ? EAGAIN : record->info_error;
			if (!error && !(record->info.flags & VMCTX_CONTEXT_ENDED))
				error=ESHUTDOWN; /* no terminal fact was obtained */
			if (!error) {
				*info=record->info;
				if(binding_epoch)*binding_epoch=record->binding ? record->binding->epoch : 0;
			}
		}
		pthread_mutex_unlock(&source_records_lock);
		if (error) { errno=error; return -1; }
		return 0;
	}
	int result=source_context_info(&record->native,info), saved=errno;
	if (!result) {
		pthread_mutex_lock(&source_records_lock);
		result=source_record_cache(record,info);
		if(result)saved=errno;
		/* Return the accepted snapshot under the same lock. A concurrent
		 * newer query or terminal observation can have won while this
		 * native result was in flight; returning that older raw result
		 * would let callers publish an already superseded binding. */
		if(!result) {
			*info=record->info;
			if(binding_epoch)*binding_epoch=record->binding ? record->binding->epoch : 0;
		}
		pthread_mutex_unlock(&source_records_lock);
	}
	source_record_put(record);
	errno=saved;
	return result;
}

static inline int source_record_info(source_id id, struct vmctx_context *info)
{ return source_record_snapshot(id,info,NULL); }

/* Validation of an already published binding is independent of task life.
 * A subsequent native memory operation must still fence its expected MM;
 * this history alone does not authorize access to a context's current MM. */
static inline int source_record_has_binding(source_id id,uint64_t mm,uint64_t epoch)
{
	int found=0;
	pthread_mutex_lock(&source_records_lock);
	if(id>0 && (size_t)id<=source_records_n && mm && epoch)
		for(struct source_mm_binding *b=source_records[id-1]->binding;b;b=b->previous)
			if(b->mm==mm && b->epoch==epoch) {found=1;break;}
	pthread_mutex_unlock(&source_records_lock);
	return found;
}

/* Release a reference that this caller already owns (the initial reference
 * from add, or one explicitly retained by get). A numeric wire request alone
 * does not confer such ownership. */
static inline void source_record_release(source_id id)
{
	struct source_record *record;
	pthread_mutex_lock(&source_records_lock);
	if (id<=0 || (size_t)id>source_records_n) abort();
	record=source_records[id-1];
	pthread_mutex_unlock(&source_records_lock);
	source_record_put(record);
}

static inline long source_record_ctl(source_id id, unsigned command, void *arg)
{
	if (!id) return source_control_raw(0,command,arg);
	struct source_record *record=source_record_get(id);
	if (!record) return -1;
	long result=source_control_raw(source_context_selector(&record->native),command,arg);
	int saved=errno;
	source_record_put(record);
	errno=saved;
	return result;
}

/* Keep the caller's MM identity intact while borrowing a retained context.
 * Selecting a live peer is separate from authorizing the operation: the
 * kernel still checks expected_mm after taking that peer's actual MM ref. */
static inline long source_record_memory(source_id id,uint64_t expected_mm,
		unsigned command,void *argument)
{
	if(!id || !expected_mm) {errno=EINVAL;return -1;}
	struct source_record *record=source_record_get(id);
	if(!record)return -1;
	long result=source_memory_call(&record->native,expected_mm,command,argument);
	int saved=errno;
	source_record_put(record);
	errno=saved;
	return result;
}

static inline int source_record_signal(source_id id, int sig)
{
	struct source_record *record=source_record_get(id);
	if (!record) return -1;
	int result=source_context_signal(&record->native,sig), saved=errno;
	source_record_put(record);
	errno=saved;
	return result;
}

static inline int source_record_proc_open(source_id id, const char *leaf,
		uint64_t *mm)
{
	struct source_record *record=source_record_get(id);
	if (!record) return -1;
	pid_t named;
	int fd=source_context_proc_open(&record->native,leaf,mm,&named), saved=errno;
	source_record_put(record);
	errno=saved;
	return fd;
}

static inline ssize_t source_record_read_proc(source_id id, const char *leaf,
		char *out, size_t capacity)
{
	struct source_record *record=source_record_get(id);
	if (!record) return -1;
	ssize_t result=source_context_read_proc(&record->native,leaf,out,capacity);
	int saved=errno;
	source_record_put(record);
	errno=saved;
	return result;
}
#endif
