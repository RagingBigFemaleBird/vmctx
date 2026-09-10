/* SPDX-License-Identifier: GPL-2.0 */
/* Host controls use real immutable MM objects and canonical retained targets.
 * They do not register a native executor or pretend an integer is a pointer.
 * Native controls require their own retained-descriptor fixture. */
#ifndef VMR_TEST_EXECUTION_FIXTURE_H
#define VMR_TEST_EXECUTION_FIXTURE_H
#include <assert.h>
#include <sys/mman.h>
#include "../../user/execution-target.h"
#ifndef TEST_MM_REGISTRY
static struct execution_mm_registry test_mm_registry=EXECUTION_MM_REGISTRY_INIT;
#define TEST_MM_REGISTRY test_mm_registry
#endif
struct test_execution_context {
    struct execution_context_binding binding;
    struct test_execution_context *next;
};
static struct test_execution_context *test_contexts;
static pthread_mutex_t test_contexts_lock=PTHREAD_MUTEX_INITIALIZER;
static int test_mm_object(void *unused)
{
    (void)unused;
    int fd=memfd_create("test-execution-mm",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    if(fd<0)return -1;
    if(ftruncate(fd,INT64_C(1)<<47)) {int saved=errno;close(fd);errno=saved;return -1;}
    return fd;
}
static inline const struct execution_target *test_target(uint64_t identity)
{
    assert(identity);
    pthread_mutex_lock(&test_contexts_lock);
    struct test_execution_context *c;
    for(c=test_contexts;c;c=c->next)if(c->binding.identity==identity)break;
    if(!c) {
        struct execution_mm *mm=execution_mm_resolve(&TEST_MM_REGISTRY,identity,0,test_mm_object,NULL);
        assert(mm);
        c=malloc(sizeof(*c));assert(c);
        struct vmr_mm_binding source={.context=identity,.mm=identity,.epoch=1};
        assert(!execution_context_binding_init(&c->binding,identity,mm,&source,1));
        c->next=test_contexts;test_contexts=c;
    }
    const struct execution_target *target=execution_target_capture(&c->binding);
    pthread_mutex_unlock(&test_contexts_lock);
    return target;
}
#endif
