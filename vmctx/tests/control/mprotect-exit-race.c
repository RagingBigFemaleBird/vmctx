// SPDX-License-Identifier: GPL-2.0
/* A permission update racing sibling teardown must update every live mm,
 * while retaining a stable proof that a disappeared execution task has ended. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
static atomic_int started;
static void *worker(void *unused)
{
    (void)unused;
    atomic_store(&started,1);
    return NULL;
}
int main(void)
{
    unsigned char *p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED) return 2;
    *p=0x63;
    for(unsigned round=0;round<24;round++) {
        pthread_t thread;
        atomic_store(&started,0);
        if(pthread_create(&thread,NULL,worker,NULL)) return 2;
        while(!atomic_load(&started)) asm volatile("pause");
        for(unsigned i=0;i<16;i++)
            if(mprotect(p,4096,PROT_READ|(i&1?PROT_WRITE:0))) return 2;
        if(pthread_join(thread,NULL) || *p!=0x63) return 1;
    }
    if(munmap(p,4096)) return 2;
    puts("PASS: 24 sibling exits race 384 permission updates without losing live context state");
    return 0;
}
