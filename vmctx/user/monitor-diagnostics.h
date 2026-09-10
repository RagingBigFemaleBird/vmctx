/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VMCTX_MONITOR_DIAGNOSTICS_H
#define VMCTX_MONITOR_DIAGNOSTICS_H
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

/* A source write may hold the inherited stderr's file-position, inode, pipe
 * or tty lock while faulting on guest memory. No fault-service thread may
 * write to that sink. Only this drain worker does I/O there; it never serves
 * guest faults or takes monitor locks. Producers use nonblocking packets,
 * including raw writes to fd 2. Backpressure loses diagnostics, never custody.
 *
 * Initialize once, before starting monitor threads. The original stderr FILE
 * is left unused until the pre-exec child restores it. All auxiliary fds are
 * CLOEXEC. In particular dup() alone would NOT separate the sink's locks.
 */
struct monitor_diagnostics {
    int sink, sender, receiver;
    FILE *original, *stream;
    pthread_t worker;
    _Atomic uint64_t dropped_bytes, sink_errors;
};
static struct monitor_diagnostics monitor_diagnostics = {
    .sink=-1, .sender=-1, .receiver=-1,
};

static ssize_t monitor_diagnostics_write(void *cookie, const char *buf, size_t n)
{
    struct monitor_diagnostics *d=cookie;
    size_t at=0;
    int saved=errno;
    while(at<n) {
        size_t count=n-at>8192 ? 8192:n-at;
        ssize_t sent=send(d->sender,buf+at,count,MSG_DONTWAIT|MSG_NOSIGNAL);
        if(sent!=(ssize_t)count) {
            /* SOCK_SEQPACKET cannot publish a partial record. Do not spin
             * even on EINTR: diagnostics must not defer fault service. */
            atomic_fetch_add_explicit(&d->dropped_bytes,n-at,memory_order_relaxed);
            break;
        }
        at+=count;
    }
    errno=saved;
    /* The sink is explicitly lossy; returning short invites stdio retries.
     * The drain reports loss independently of this producer's next write. */
    return (ssize_t)n;
}

static int monitor_diagnostics_sink(struct monitor_diagnostics *d,
                                    const char *buf,size_t n)
{
    while(n) {
        ssize_t written=write(d->sink,buf,n);
        if(written<0 && errno==EINTR)continue;
        if(written<=0) {
            atomic_fetch_add_explicit(&d->sink_errors,1,memory_order_relaxed);
            return -1;
        }
        buf+=written;n-=(size_t)written;
    }
    return 0;
}

static void *monitor_diagnostics_drain(void *cookie)
{
    struct monitor_diagnostics *d=cookie;
    char buf[8192];
    uint64_t reported=0;
    for(;;) {
        ssize_t n=recv(d->receiver,buf,sizeof(buf),0);
        if(n<0 && errno==EINTR)continue;
        if(n>0)(void)monitor_diagnostics_sink(d,buf,(size_t)n);
        uint64_t dropped=atomic_load_explicit(&d->dropped_bytes,memory_order_relaxed);
        if(dropped!=reported) {
            char message[160];
            int len=snprintf(message,sizeof(message),
                "\n[vmhome] diagnostic loss: %llu bytes dropped (total)\n",
                (unsigned long long)dropped);
            if(!monitor_diagnostics_sink(d,message,(size_t)len))reported=dropped;
        }
        if(n<=0)break;
    }
    return NULL;
}

static int monitor_diagnostics_start(void)
{
    struct monitor_diagnostics *d=&monitor_diagnostics;
    int pair[2],error;
    d->original=stderr;
    d->sink=fcntl(STDERR_FILENO,F_DUPFD_CLOEXEC,3);
    if(d->sink<0)return -1;
    if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair))goto fail;
    d->sender=pair[0];d->receiver=pair[1];
    /* Raw descriptor writes must obey the same nonblocking boundary. */
    if(fcntl(d->sender,F_SETFL,O_NONBLOCK))goto fail;
    d->stream=fopencookie(d,"w",(cookie_io_functions_t){
        .write=monitor_diagnostics_write,
    });
    if(!d->stream)goto fail;
    if(setvbuf(d->stream,NULL,_IONBF,0)) {errno=EIO;goto fail;}
    if(dup3(d->sender,STDERR_FILENO,O_CLOEXEC)<0)goto fail;
    error=pthread_create(&d->worker,NULL,monitor_diagnostics_drain,d);
    if(error) {
        (void)dup2(d->sink,STDERR_FILENO);
        errno=error;goto fail;
    }
    stderr=d->stream;
    return 0;
fail:
    error=errno;
    if(d->stream)fclose(d->stream);
    if(d->sender>=0)close(d->sender);
    if(d->receiver>=0)close(d->receiver);
    close(d->sink);
    d->sink=d->sender=d->receiver=-1;d->stream=NULL;
    errno=error;return -1;
}

/* Only in the child between fork and exec: no stdio locks, allocation or
 * thread joins. Restore the caller's open description and descriptor flags;
 * never let a guest hold a diagnostic socket or the drain's duplicate sink. */
static int monitor_diagnostics_child(void)
{
    struct monitor_diagnostics *d=&monitor_diagnostics;
    if(d->sink<0)return 0; /* host controls that do not start a monitor */
    if(dup2(d->sink,STDERR_FILENO)<0)return -1;
    close(d->sink);close(d->sender);close(d->receiver);
    stderr=d->original;
    d->sink=d->sender=d->receiver=-1;
    return 0;
}
#endif
