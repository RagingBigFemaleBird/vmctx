/* SPDX-License-Identifier: GPL-2.0 */
/* One operation retains one target across callbacks. Targets, their context,
 * and MM objects live until all workers join. Only native memory/CPU controls
 * take the binding guard; object access uses target->view.mm directly.
 *
 * Source observations can precede the executor's stopped-context publication.
 * Such targets own storage but have native epoch zero: they may select another
 * current binding of that exact MM, never the context's unrelated current MM.
 */
#ifndef VMR_EXECUTION_TARGET_H
#define VMR_EXECUTION_TARGET_H
#include "execution-mm.h"
#include "memory-identity.h"

struct execution_context_binding;
struct execution_target {
	struct execution_context_binding *context;
	struct vmr_mm_binding source;
	struct execution_mm_view view;
	struct execution_target *previous; /* immutable history, including observations */
};

struct execution_context_binding {
	uint64_t identity;
	pthread_mutex_t publication_lock;
	struct execution_binding native;
	struct execution_target *current,*history;
};

#ifndef EXECUTION_TARGET_ALLOC
#define EXECUTION_TARGET_ALLOC malloc
#endif

static inline int execution_target_matches_mm(const struct vmr_mm_binding *source,
		const struct execution_mm *mm)
{
	return vmr_binding_valid(source) && mm && source->mm==mm->identity &&
		source->parent_mm==(mm->parent ? mm->parent->identity : 0);
}

/* Caller verified the initial native object and epoch before publication.
 * Neither this context nor a partially initialized record may be visible yet. */
static inline int execution_context_binding_init(struct execution_context_binding *context,
		uint64_t identity,struct execution_mm *mm,
		const struct vmr_mm_binding *source,uint64_t native_epoch)
{
	if(!identity || !native_epoch || !execution_target_matches_mm(source,mm)) {
		errno=EINVAL;return -1;
	}
	struct execution_target *first=EXECUTION_TARGET_ALLOC(sizeof(*first));
	if(!first)return -1;
	int error=pthread_mutex_init(&context->publication_lock,NULL);
	if(error) {free(first);errno=error;return -1;}
	if(execution_binding_init(&context->native,mm,native_epoch)) {
		int saved=errno;
		pthread_mutex_destroy(&context->publication_lock);free(first);errno=saved;return -1;
	}
	*first=(struct execution_target){.context=context,.source=*source,
		.view={mm,native_epoch}};
	context->identity=identity;context->current=context->history=first;
	return 0;
}

static inline const struct execution_target *execution_target_capture(
		struct execution_context_binding *context)
{
	pthread_mutex_lock(&context->publication_lock);
	const struct execution_target *target=context->current;
	pthread_mutex_unlock(&context->publication_lock);
	return target;
}

/* Caller holds publication_lock. Prefer a native binding if this source epoch
 * was subsequently installed; an earlier returned observation stays immutable. */
static inline struct execution_target *execution_target_find_locked(
		struct execution_context_binding *context,const struct vmr_mm_binding *source)
{
	for(struct execution_target *t=context->history;t;t=t->previous)
		if(t->source.epoch==source->epoch)return t;
	return NULL;
}

/* Resolve the MM/object before this call, outside all publication guards.
 * This only records an observed source binding; it never changes native state. */
static inline const struct execution_target *execution_target_observe(
		struct execution_context_binding *context,struct execution_mm *mm,
		const struct vmr_mm_binding *source)
{
	if(!execution_target_matches_mm(source,mm)) {errno=EPROTO;return NULL;}
	struct execution_target *spare=EXECUTION_TARGET_ALLOC(sizeof(*spare));
	pthread_mutex_lock(&context->publication_lock);
	struct execution_target *found=execution_target_find_locked(context,source);
	int error=source->context!=context->current->source.context ||
		(found && (!vmr_binding_equal(&found->source,source) || found->view.mm!=mm)) ? EPROTO : 0;
	if(!error && !found) {
		if(!spare)error=ENOMEM;
		else {
			*spare=(struct execution_target){.context=context,.source=*source,
				.view={mm,0},.previous=context->history};
			context->history=found=spare;spare=NULL;
		}
	}
	pthread_mutex_unlock(&context->publication_lock);
	free(spare);
	if(error) {errno=error;return NULL;}
	return found;
}

/* A stopped-context owner installs a newer source image. Source publication
 * and native epoch become visible together. The adapter may perform bounded
 * local controls only: no callbacks, network, or source-work waits under either
 * guard. Old replies resolve to their old targets without rolling back state.
 * Allocation precedes native mutation, so it cannot strand a committed image. */
static inline const struct execution_target *execution_target_publish(
		struct execution_context_binding *context,struct execution_mm *mm,
		const struct vmr_mm_binding *source,
		int (*replace)(void *,int,uint64_t,uint64_t *),void *opaque)
{
	if(!execution_target_matches_mm(source,mm)) {errno=EPROTO;return NULL;}
	struct execution_target *spare=EXECUTION_TARGET_ALLOC(sizeof(*spare));
	pthread_mutex_lock(&context->publication_lock);
	struct execution_target *current=context->current;
	struct execution_target *found=execution_target_find_locked(context,source);
	int error=0;
	if(source->context!=current->source.context ||
	   (found && (!vmr_binding_equal(&found->source,source) || found->view.mm!=mm)))error=EPROTO;
	else if(source->epoch<=current->source.epoch) {
		if(!found) {
			if(!spare)error=ENOMEM;
			else {
				*spare=(struct execution_target){.context=context,.source=*source,
					.view={mm,0},.previous=context->history};
				context->history=found=spare;spare=NULL;
			}
		}
	} else if(source->mm==current->source.mm)error=EPROTO;
	else if(!spare)error=ENOMEM;
	else if(execution_binding_replace(&context->native,current->view,mm,replace,opaque))error=errno;
	else {
		*spare=(struct execution_target){.context=context,.source=*source,
			.view={mm,current->view.epoch+1},.previous=context->history};
		context->current=context->history=found=spare;spare=NULL;
	}
	pthread_mutex_unlock(&context->publication_lock);
	free(spare);
	if(error) {errno=error;return NULL;}
	return found;
}

static inline long execution_target_control(const struct execution_target *target,
		long (*control)(void *),void *opaque)
{
	if(!target || !target->view.epoch) {errno=ESTALE;return -1;}
	return execution_binding_control(&target->context->native,target->view,control,opaque);
}

/* No outstanding target/control/lookup is allowed at session shutdown. */
static inline void execution_context_binding_destroy(struct execution_context_binding *context)
{
	struct execution_target *t=context->history;
	while(t) {struct execution_target *previous=t->previous;free(t);t=previous;}
	context->current=context->history=NULL;
	pthread_rwlock_destroy(&context->native.lock);
	pthread_mutex_destroy(&context->publication_lock);
}
#endif
