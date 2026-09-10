// SPDX-License-Identifier: GPL-2.0
/* CPU ownership across threads, fork, exec, and wait/rusage. */
#define _GNU_SOURCE
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static volatile uint64_t sink;
static uint64_t worker_cpu;
static uint64_t ns(clockid_t id) {
 struct timespec t;if(clock_gettime(id,&t))return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static void burn(void) {
 uint64_t x=0x1827364554637281ULL;
 for(uint64_t i=0;i<50000000;i++) {x^=x>>12;x^=x<<25;x^=x>>27;x*=0x2545f4914f6cdd1dULL;}
 sink=x;
}
static void *worker(void *unused) {
 (void)unused;uint64_t before=ns(CLOCK_THREAD_CPUTIME_ID);burn();
 worker_cpu=ns(CLOCK_THREAD_CPUTIME_ID)-before;return NULL;
}
int main(int argc,char **argv) {
 if(argc==3&&!strcmp(argv[1],"after-exec")) {
  uint64_t before=strtoull(argv[2],NULL,10),now=ns(CLOCK_PROCESS_CPUTIME_ID);
  return now>=before&&now<before+1000000000?0:1;
 }
 uint64_t t0=ns(CLOCK_THREAD_CPUTIME_ID),p0=ns(CLOCK_PROCESS_CPUTIME_ID);
 pthread_t thread;if(pthread_create(&thread,NULL,worker,NULL)||pthread_join(thread,NULL))return 2;
 uint64_t own=ns(CLOCK_THREAD_CPUTIME_ID)-t0,group=ns(CLOCK_PROCESS_CPUTIME_ID)-p0;
 if(worker_cpu<30000000||group+1000000<worker_cpu||own>worker_cpu/2) {
  fprintf(stderr,"FAIL: CPU ownership main=%" PRIu64 " worker=%" PRIu64 " group=%" PRIu64 "\n",own,worker_cpu,group);return 1;
 }
 pid_t child=fork();if(child<0)return 2;
 if(!child) {
  uint64_t initial=ns(CLOCK_PROCESS_CPUTIME_ID);if(initial>30000000)_exit(3);
  burn();uint64_t before=ns(CLOCK_PROCESS_CPUTIME_ID);if(before-initial<30000000)_exit(4);
  char value[32];snprintf(value,sizeof(value),"%" PRIu64,before);
  execl("/proc/self/exe",argv[0],"after-exec",value,NULL);_exit(5);
 }
 int status;struct rusage usage;
 if(wait4(child,&status,0,&usage)!=child||!WIFEXITED(status)||WEXITSTATUS(status)) {
  fprintf(stderr,"FAIL: fork reset or exec persistence status=%x\n",status);return 1;
 }
 uint64_t child_cpu=(uint64_t)usage.ru_utime.tv_sec*1000000000ULL+usage.ru_utime.tv_usec*1000ULL;
 if(child_cpu<30000000) {fprintf(stderr,"FAIL: wait/rusage lost child CPU time\n");return 1;}
 printf("PASS: CPU lifetime thread=%" PRIu64 " group=%" PRIu64 " child=%" PRIu64 " fork resets and exec preserves\n",worker_cpu,group,child_cpu);return 0;
}
