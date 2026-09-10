/* SPDX-License-Identifier: GPL-2.0 */
/* An operation captures this value before callbacks or source work. Choosing
 * a native representative can change the task used for the call, never the
 * requested MM. Historical binding validation is separate from native MM
 * liveness; failure to find a representative does not prove MM retirement. */
#ifndef VMR_SOURCE_MEMORY_TARGET_H
#define VMR_SOURCE_MEMORY_TARGET_H
#include "source-registry.h"

struct source_memory_target {source_id context;uint64_t mm,epoch;};

/* Some MM metadata belongs to the task which performed the operation (for
 * example its last construction sequence). Another representative of the
 * same MM cannot supply that receipt. Pin the original retained context and
 * fence the native operation against its captured MM, without peer fallback. */
static inline long source_memory_target_origin_call(const struct source_memory_target *target,
		unsigned command,void *argument)
{
	if(!source_record_has_binding(target->context,target->mm,target->epoch)) {
		errno=ESTALE;return -1;
	}
	struct source_record *record=source_record_get(target->context);
	if(!record)return -1;
	long result=source_memory_call(&record->native,target->mm,command,argument);
	int saved=errno;
	source_record_put(record);
	errno=saved;return result;
}

static inline int source_memory_target_capture(source_id id,
		struct source_memory_target *target)
{
	struct vmctx_context info;
	uint64_t epoch;
	if(source_record_snapshot(id,&info,&epoch))return -1;
	if(!info.mm_identity || !epoch) {errno=EPROTO;return -1;}
	*target=(struct source_memory_target){id,info.mm_identity,epoch};
	return 0;
}

/* Bridge for a journal cursor which already names its MM. A zero cursor
 * identity captures the initial MM at request entry. An explicit identity
 * resolves only recorded history; it never falls back to the current MM. */
static inline int source_memory_target_for_mm(source_id id,uint64_t mm,
		struct source_memory_target *target)
{
	if(!mm)return source_memory_target_capture(id,target);
	int found=0;
	pthread_mutex_lock(&source_records_lock);
	if(id>0 && (size_t)id<=source_records_n)
		for(struct source_mm_binding *b=source_records[id-1]->binding;b;b=b->previous)
			if(b->mm==mm) {
				*target=(struct source_memory_target){id,b->mm,b->epoch};found=1;break;
			}
	pthread_mutex_unlock(&source_records_lock);
	if(!found) {errno=ESTALE;return -1;}
	return 0;
}

/* INFO only chooses a candidate. MEMORY captures and checks that candidate's
 * actual MM before consuming any inner argument. A changed/ended candidate
 * therefore fails closed. Invoke the memory command at most once: its error
 * might follow an inner commit, so replaying it on another peer is unsafe.
 * Borrowed descriptor ownership spans selection and the native call; no
 * registry mutex or exec/binding guard spans either native operation. */
static inline long source_memory_target_call(const struct source_memory_target *target,
		unsigned command,void *argument)
{
	if(!source_record_has_binding(target->context,target->mm,target->epoch)) {
		errno=ESTALE;return -1;
	}
	pthread_mutex_lock(&source_records_lock);
	size_t count=source_records_n;
	pthread_mutex_unlock(&source_records_lock);
	int unavailable=ESRCH;
	for(size_t pass=0;pass<=count;pass++) {
		source_id id=pass ? (source_id)pass : target->context;
		if(pass && id==target->context)continue;
		struct source_record *record=source_record_get(id);
		if(!record) {
			if(errno!=ESRCH)return -1;
			continue;
		}
		struct vmctx_context info;
		int error=source_context_info(&record->native,&info) ? errno : 0;
		if(!error && !(info.flags & VMCTX_CONTEXT_ENDED) && info.mm_identity==target->mm) {
			long result=source_memory_call(&record->native,target->mm,command,argument);
			int saved=errno;
			source_record_put(record);
			errno=saved;return result;
		}
		source_record_put(record);
		if(error && error!=ESRCH && error!=EAGAIN) {errno=error;return -1;}
		if(error==EAGAIN)unavailable=EAGAIN;
	}
	errno=unavailable;return -1;
}

/* Read-only proc adapters may select any retained representative of the saved
 * MM. The opener proves both task identity and MM identity; close mismatches
 * before considering another representative. Callers consume and close this
 * descriptor within a bounded local read, before any peer or native callback. */
static inline int source_memory_target_proc_open(const struct source_memory_target *target,
        const char *leaf,source_id *representative)
{
    if(!source_record_has_binding(target->context,target->mm,target->epoch)) {
        errno=ESTALE;return -1;
    }
    pthread_mutex_lock(&source_records_lock);
    size_t count=source_records_n;
    pthread_mutex_unlock(&source_records_lock);
    int unavailable=ESRCH;
    for(size_t pass=0;pass<=count;pass++) {
        source_id id=pass ? (source_id)pass : target->context;
        if(pass && id==target->context)continue;
        struct vmctx_context info;
        if(source_record_info(id,&info)) {
            if(errno!=ESRCH && errno!=EAGAIN)return -1;
            if(errno==EAGAIN)unavailable=EAGAIN;
            continue;
        }
        if(info.mm_identity!=target->mm || (info.flags&VMCTX_CONTEXT_ENDED))continue;
        uint64_t mm=0;
        int fd=source_record_proc_open(id,leaf,&mm);
        if(fd<0) {
            if(errno!=ESRCH && errno!=EAGAIN && errno!=ESTALE)return -1;
            unavailable=errno;continue;
        }
        if(mm!=target->mm) {close(fd);unavailable=ESTALE;continue;}
        if(representative)*representative=id;
        return fd;
    }
    errno=unavailable;return -1;
}
#endif
