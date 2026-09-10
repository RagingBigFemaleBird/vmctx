/* SPDX-License-Identifier: GPL-2.0 */
/* Source adapter identity domains: a source_id owns a task handle, while a
 * source_mm_id names one native MM lifetime. Exec can change their binding;
 * CLONE_VM can share an MM between unrelated native thread groups. */
#ifndef VMR_SOURCE_ADDRESS_SPACE_H
#define VMR_SOURCE_ADDRESS_SPACE_H
typedef unsigned long long source_mm_id;

static _Noreturn void source_adapter_failed(const char *operation)
{
	int saved=errno;
	fprintf(stderr,"[vmhome] source adapter failed during %s: %s\n",
		operation,strerror(saved));
	pthread_mutex_lock(&source_records_lock);
	size_t n=source_records_n;
	pthread_mutex_unlock(&source_records_lock);
	for (size_t i=0;i<n;i++) (void)source_record_signal((source_id)i+1,SIGKILL);
	fflush(stderr);
	_exit(98);
}

static source_mm_id as_of(source_id id)
{
	struct vmctx_context q;
	if (!id) return 0;
	while (source_record_info(id,&q)) {
		if (errno!=EAGAIN) source_adapter_failed("resolving MM identity");
		usleep(1000);
	}
	if (!q.mm_identity) { errno=EPROTO; source_adapter_failed("missing MM identity"); }
	return q.mm_identity;
}

static size_t source_record_count(void)
{
	pthread_mutex_lock(&source_records_lock);
	size_t n=source_records_n;
	pthread_mutex_unlock(&source_records_lock);
	return n;
}

/* Native references cover pending births and native memory operations before
 * a userspace callback exists. An INFO error means unknown, never ended. */
static unsigned long n_as_ended;
static int as_has_ended(source_mm_id mm)
{
	if (!mm || source_mm_live(mm)!=0) return 0;
	__atomic_add_fetch(&n_as_ended,1,__ATOMIC_RELAXED);
	return 1;
}

static source_id as_live_mm(source_mm_id mm, source_id avoid, source_id like)
{
	if (!mm) return 0;
	size_t n=source_record_count();
	for (size_t i=0;i<n;i++) {
		source_id id=(source_id)i+1;
		struct vmctx_context q;
		if (id==avoid || id==like || source_record_info(id,&q) ||
		    q.mm_identity!=mm || (q.flags & VMCTX_CONTEXT_ENDED)) continue;
		/* The caller's eventual operation still resolves the exact retained
		 * context. If that context exits meanwhile it returns ESRCH. */
		return id;
	}
	return 0;
}

static inline source_id as_live_ctx(source_id like, source_id avoid)
{ return as_live_mm(as_of(like),avoid,like); }
#endif
