/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "../../user/monitor-diagnostics.h"

#define CHECK(x) do {if(!(x)) {dprintf(1,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static _Atomic int loss_seen;
static void *read_sink(void *arg)
{
    int fd=*(int *)arg;
    char buf[4096],tail[8192]={0};size_t used=0;
    ssize_t n;
    while((n=read(fd,buf,sizeof(buf)))>0) {
        if(used+(size_t)n>=sizeof(tail))used=0;
        memcpy(tail+used,buf,(size_t)n);used+=(size_t)n;tail[used]=0;
        if(strstr(tail,"diagnostic loss:"))atomic_store(&loss_seen,1);
    }
    CHECK(n==0);return NULL;
}
/* Test-only quiescent shutdown: all source work and producers have ended.
 * A live monitor must never join a worker waiting for guest-held I/O locks. */
static void stop_diagnostics(void)
{
    struct monitor_diagnostics *d=&monitor_diagnostics;
    stderr=d->original;
    CHECK(!fclose(d->stream));
    CHECK(dup2(d->sink,2)==2);
    close(d->sender);
    CHECK(!pthread_join(d->worker,NULL));
    close(d->receiver);close(d->sink);
    d->sink=d->sender=d->receiver=-1;
}
int main(int argc,char **argv)
{
    alarm(15);signal(SIGPIPE,SIG_IGN);
    if(argc==5 && !strcmp(argv[1],"exec")) {
        for(int i=2;i<5;i++)CHECK(fcntl(atoi(argv[i]),F_GETFD)==-1 && errno==EBADF);
        CHECK(!(fcntl(2,F_GETFL)&O_NONBLOCK));
        CHECK(write(2,"guest stderr\n",13)==13);
        return 0;
    }
    CHECK(argc==2);
    int original=dup(2);CHECK(original>=0);
    int saturation=!strcmp(argv[1],"saturation"),failed=!strcmp(argv[1],"sink-error");
    int pipefd[2]={-1,-1},fd=-1;
    char path[]="/tmp/vmctx-diagnostics.XXXXXX";
    if(saturation) {
        CHECK(!pipe2(pipefd,O_CLOEXEC));fd=pipefd[1];
        CHECK(!fcntl(fd,F_SETFL,O_NONBLOCK));
        char fill[4096];memset(fill,'x',sizeof(fill));
        while(write(fd,fill,sizeof(fill))>0){}
        CHECK(errno==EAGAIN && !fcntl(fd,F_SETFL,0));
    } else if(failed)fd=open("/dev/full",O_WRONLY|O_CLOEXEC);
    else {CHECK(!strcmp(argv[1],"inherit"));fd=mkstemp(path);CHECK(fd>=0);unlink(path);}
    CHECK(fd>=0 && dup2(fd,2)==2);
    CHECK(!monitor_diagnostics_start());
    struct monitor_diagnostics *d=&monitor_diagnostics;
    CHECK((fcntl(2,F_GETFL)&O_NONBLOCK) && (fcntl(2,F_GETFD)&FD_CLOEXEC));
    CHECK(!(fcntl(d->sink,F_GETFL)&O_NONBLOCK));
    if(saturation) {
        /* The sink is full and nobody drains it yet. All producers must
         * complete anyway, including a direct fd write and stdio flush. */
        char data[8192];memset(data,'d',sizeof(data));
        for(unsigned i=0;i<4096;i++)CHECK(fwrite(data,1,sizeof(data),stderr)==sizeof(data));
        CHECK(atomic_load(&d->dropped_bytes)>0);
        CHECK(write(2,"raw",3)==-1 && errno==EAGAIN);
        CHECK(!fflush(stderr));
        pthread_t reader;CHECK(!pthread_create(&reader,NULL,read_sink,&pipefd[0]));
        stop_diagnostics();close(fd);CHECK(dup2(original,2)==2);
        CHECK(!pthread_join(reader,NULL) && atomic_load(&loss_seen));close(pipefd[0]);
    } else {
        errno=ENOENT;perror("cookie-perror");
        CHECK(errno==ENOENT);
        CHECK(fprintf(stderr,"[vmhome] diagnostic record\n")>0);
        CHECK(write(2,"raw record\n",11)==11);
        if(!failed) {
            char a[16],b[16],c[16];
            snprintf(a,sizeof(a),"%d",d->sink);snprintf(b,sizeof(b),"%d",d->sender);snprintf(c,sizeof(c),"%d",d->receiver);
            pid_t kid=fork();CHECK(kid>=0);
            if(!kid) {
                if(monitor_diagnostics_child())_exit(2);
                execl("/proc/self/exe",argv[0],"exec",a,b,c,NULL);_exit(3);
            }
            int status;CHECK(waitpid(kid,&status,0)==kid && WIFEXITED(status) && !WEXITSTATUS(status));
        }
        stop_diagnostics();CHECK(dup2(original,2)==2);
        if(failed)CHECK(atomic_load(&d->sink_errors)>0);
        else {
            char data[4096]={0};CHECK(pread(fd,data,sizeof(data)-1,0)>0);
            CHECK(strstr(data,"guest stderr\n") && strstr(data,"[vmhome] diagnostic record\n") &&
                  strstr(data,"cookie-perror: No such file or directory\n") && strstr(data,"raw record\n"));
            CHECK(!atomic_load(&d->sink_errors) && !atomic_load(&d->dropped_bytes));
        }
        close(fd);
    }
    close(original);
    printf("PASS: monitor diagnostics %s\n",argv[1]);return 0;
}
