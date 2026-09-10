/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VMCTX_MONITOR_OWNER_H
#define VMCTX_MONITOR_OWNER_H
/* Linux binds a context to the task that attaches, while permitting its whole
 * monitor thread group to service it. Keep that owner alive for the session.
 * Guest service threads may finish independently; no detach/reattach window
 * should interrupt another thread's memory operation or a child's entry gate. */
struct monitor_attach_request {
    pid_t pid;
    long result;
    int error, done;
    pthread_cond_t completed;
    struct monitor_attach_request *next;
};
static pthread_once_t monitor_owner_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t monitor_owner_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t monitor_owner_work = PTHREAD_COND_INITIALIZER;
static struct monitor_attach_request *monitor_owner_head, *monitor_owner_tail;
static int monitor_owner_error;

static void *monitor_owner_worker(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&monitor_owner_lock);
    for (;;) {
        while (!monitor_owner_head)
            pthread_cond_wait(&monitor_owner_work, &monitor_owner_lock);
        struct monitor_attach_request *request = monitor_owner_head;
        monitor_owner_head = request->next;
        if (!monitor_owner_head) monitor_owner_tail = NULL;
        pthread_mutex_unlock(&monitor_owner_lock);
        /* No queue lock across a kernel operation. The waiting caller keeps
         * this request alive until completion is published under the lock. */
        /* This queue accepts only fresh, owned, unreaped native children.
         * Initial attach is a birth operation; published context controls
         * never regain the authority to resolve a numeric native PID. */
        long result = execution_control_raw(request->pid, VMCTX_CTL_ATTACH, NULL);
        int error = result < 0 ? errno : 0;
        pthread_mutex_lock(&monitor_owner_lock);
        request->result = result;
        request->error = error;
        request->done = 1;
        pthread_cond_signal(&request->completed);
    }
    return NULL;
}
static void monitor_owner_start(void)
{
    pthread_t thread;
    monitor_owner_error = pthread_create(&thread, NULL, monitor_owner_worker, NULL);
    if (!monitor_owner_error) pthread_detach(thread);
}
static long monitor_attach(pid_t pid)
{
    struct monitor_attach_request request = {.pid = pid};
    int error = pthread_once(&monitor_owner_once, monitor_owner_start);
    if (!error) error = monitor_owner_error;
    if (!error) error = pthread_cond_init(&request.completed, NULL);
    if (error) { errno = error; return -1; }
    pthread_mutex_lock(&monitor_owner_lock);
    if (monitor_owner_tail) monitor_owner_tail->next = &request;
    else monitor_owner_head = &request;
    monitor_owner_tail = &request;
    pthread_cond_signal(&monitor_owner_work);
    while (!request.done)
        pthread_cond_wait(&request.completed, &monitor_owner_lock);
    pthread_mutex_unlock(&monitor_owner_lock);
    pthread_cond_destroy(&request.completed);
    errno = request.error;
    return request.result;
}
#endif
