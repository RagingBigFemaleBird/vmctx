// SPDX-License-Identifier: GPL-2.0
/* Source CPU timers must expire during instruction-only execution, while
 * elapsed transport and sleep time must not expire them. */
#define _GNU_SOURCE
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t hits;
static volatile uint64_t sink;
static void hit(int s) { (void)s;hits++; }
static uint64_t cpu(void) {
 struct timespec t;if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t))return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static int compute(void) {
 uint64_t x=1234567;
 for(uint64_t i=0;i<1000000000ULL&&!hits;i++) {
  x^=x>>12;x^=x<<25;x^=x>>27;x*=0x2545f4914f6cdd1dULL;
 }
 sink=x;return hits==1;
}
static int timer_case(int kind) {
 timer_t id;hits=0;uint64_t before=cpu();
 if(kind<2) {
  struct sigevent ev={.sigev_notify=SIGEV_SIGNAL,.sigev_signo=SIGUSR1};
  struct itimerspec t={.it_value={.tv_nsec=50000000}};
  if(timer_create(kind?CLOCK_THREAD_CPUTIME_ID:CLOCK_PROCESS_CPUTIME_ID,&ev,&id)||timer_settime(id,0,&t,NULL))return 0;
 } else {
  struct itimerval t={.it_value={.tv_usec=50000}};
  if(setitimer(ITIMER_VIRTUAL,&t,NULL))return 0;
 }
 struct timespec pause={.tv_nsec=150000000};
 if(nanosleep(&pause,NULL)||hits)return 0;
 int ok=compute();uint64_t after=cpu();
 if(kind<2&&timer_delete(id))ok=0;
 if(after<before+40000000||after>before+500000000)ok=0;
 printf("%s: CPU timer kind=%d hits=%d cpu_ns=%" PRIu64 "\n",ok?"PASS":"FAIL",kind,hits,after-before);
 return ok;
}
int main(void) {
 struct sigaction sa={.sa_handler=hit};
 if(sigaction(SIGUSR1,&sa,NULL)||sigaction(SIGVTALRM,&sa,NULL)||sigaction(SIGXCPU,&sa,NULL))return 2;
 for(int i=0;i<3;i++)if(!timer_case(i))return 1;
 pid_t child=fork();if(child<0)return 2;
 if(!child) {
  struct rlimit limit={.rlim_cur=1,.rlim_max=3};hits=0;
  if(setrlimit(RLIMIT_CPU,&limit))_exit(2);
  int ok=compute();uint64_t used=cpu();
  _exit(ok&&used>=900000000&&used<2500000000ULL?0:1);
 }
 int status;if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status)) {
  fprintf(stderr,"FAIL: source RLIMIT_CPU during pure computation\n");return 1;
 }
 puts("PASS: source process/thread/virtual CPU timers, sleep exclusion and RLIMIT_CPU without guest calls");return 0;
}
