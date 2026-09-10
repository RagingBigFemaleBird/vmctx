// SPDX-License-Identifier: GPL-2.0
/* Native execution storage: independent MM replacement and copyout replay.
 * Run under the owned native-test runner, with run/ctl syscall numbers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include "../../kernel/vmctx_object.h"
#include "../../kernel/vmctx_context.h"
#include "../../kernel/vmctx_quiesce.h"
#include "../../user/execution-mm.h"
#include "../../user/linux-execution-object.h"

_Static_assert(sizeof(struct vmctx_object) == 56, "object ABI size");
#define BASE UINT64_C(0x3000000000)
extern char object_probe[], object_spin[];
asm(".text\n.global object_probe,object_spin\n"
    "object_probe: mov (%rdi),%rsi; mov 4096(%rdi),%rdx; mov $0xfeed,%eax; syscall\n"
    "object_spin: pause; jmp object_spin\n");
static volatile sig_atomic_t children[2], expired;
static int held_event[2], handle_mode, registry_mode;
static int context_fd[2] = {-1, -1};
static uint64_t context_id[2], context_mm[2];
static pid_t context_selector(pid_t target)
{
	for (unsigned i=0;i<2;i++)
		if (target>0 && target==children[i] && context_fd[i]>=0)
			return -(context_fd[i]+1);
	return target;
}
static long ctl_nr;
static void deadline(int sig)
{
	(void)sig; expired = 1;
	for (unsigned i = 0; i < 2; i++)
		if (children[i] > 0) kill(children[i], SIGKILL);
}
static int ctl(pid_t target, unsigned cmd, void *arg)
{
	for (unsigned i = 0; i < 20000 && !expired; i++) {
		if (!i) fprintf(stderr, "object control begin target=%d cmd=%u\n", target, cmd);
		int result = syscall(ctl_nr, context_selector(target), cmd, arg);
		if (!result || errno != EAGAIN) {
			fprintf(stderr, "object control end target=%d cmd=%u result=%d errno=%d\n", target, cmd, result, result ? errno : 0);
			return result;
		}
		usleep(100);
	}
	errno = ETIMEDOUT; return -1;
}
struct replace_adapter {pid_t target;void *bad;int inject_copyout,calls;};
static long adapter_control(void *opaque,unsigned command,void *arg)
{
	struct replace_adapter *adapter=opaque;
	adapter->calls++;
	if(adapter->inject_copyout) {
		adapter->inject_copyout=0;
		if(mprotect(adapter->bad,4096,PROT_READ|PROT_WRITE))return -1;
		memcpy(adapter->bad,arg,sizeof(struct vmctx_object));
		if(mprotect(adapter->bad,4096,PROT_READ))return -1;
		return ctl(adapter->target,command,adapter->bad);
	}
	return ctl(adapter->target,command,arg);
}
static int registry_object(void *opaque)
{return fcntl(*(int *)opaque,F_DUPFD_CLOEXEC,3);}
static long guarded_write(void *opaque)
{
	pid_t target=*(pid_t *)opaque;
	uint64_t byte=99;
	struct vmctx_mem write={.addr=BASE,.len=sizeof(byte),.buf=(uintptr_t)&byte};
	return ctl(target,VMCTX_CTL_POKE,&write);
}
static struct vmctx_object request(unsigned op, int fd, uint64_t expected)
{
	return (struct vmctx_object){.version = VMCTX_OBJECT_ABI,
		.size = sizeof(struct vmctx_object), .op = op, .backing_fd = fd,
		.expected_epoch = expected, .epoch = expected ? expected + 1 : 0};
}
static int object(const char *name, uint64_t first, uint64_t second)
{
	int fd = memfd_create(name, MFD_CLOEXEC);
	if (fd < 0) return -1;
	if (ftruncate(fd, BASE + 8192) ||
	    pwrite(fd, &first, sizeof(first), BASE) != sizeof(first) ||
	    pwrite(fd, &second, sizeof(second), BASE + 4096) != sizeof(second)) {
		close(fd); return -1;
	}
	return fd;
}
static int mapping(pid_t target, uint64_t address, uint64_t *inode)
{
	char path[80], line[512];
	snprintf(path, sizeof(path), "/proc/%d/maps", target);
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	int found = 0;
	while (fgets(line, sizeof(line), f)) {
		unsigned long long start, end, value;
		if (sscanf(line, "%llx-%llx %*s %*s %*s %llu", &start, &end, &value) == 3 &&
		    start <= address && address < end) {
			*inode = value; found = 1; break;
		}
	}
	if (ferror(f)) found = -1;
	if (fclose(f)) return -1;
	return found;
}
static int probe(pid_t target, uint64_t first, uint64_t second)
{
	struct vmctx_reply reply = {.action = VMCTX_ACT_SELF};
	if (ctl(target, VMCTX_CTL_RESUME, &reply)) return -1;
	for (unsigned i = 0; i < 128 && !expired; i++) {
		struct vmctx_event event;
		if (ctl(target, VMCTX_CTL_WAIT, &event)) return -1;
		unsigned index = target == children[0] ? 0 : 1;
		held_event[index] = 1;
		fprintf(stderr, "object probe target=%d type=%u nr=%llu addr=%llx rip=%llx values=%llu/%llu\n",
			target, event.type, (unsigned long long)event.nr, (unsigned long long)event.fault_addr,
			(unsigned long long)event.rip, (unsigned long long)event.args[1], (unsigned long long)event.args[2]);
		if (event.type == VMCTX_EV_SYSCALL)
			return event.nr == 0xfeed && event.args[1] == first &&
			       event.args[2] == second ? 0 : -1;
		if (event.type != VMCTX_EV_FAULT || event.nr != 14) return -1;
		reply = (struct vmctx_reply){.action = VMCTX_ACT_SELF};
		if (event.fault_addr >= BASE && event.fault_addr < BASE + 8192) {
			/* This fixture owns the already populated object. MAPOBJ
			 * installs that folio without allocating a substitute page.
			 * SELF would decline the fault and undo the newly backed VMA. */
			struct vmctx_mem page = {.addr = event.fault_addr & ~UINT64_C(4095), .len = 4096};
			long mapped = syscall(ctl_nr, context_selector(target), VMCTX_CTL_MAPOBJ, &page);
			if (mapped != 4096) {
				fprintf(stderr, "probe MAPOBJ addr=%llx result=%ld errno=%d\n",
					(unsigned long long)page.addr, mapped, errno);
				return -1;
			}
			reply.action = VMCTX_ACT_DONE;
		}
		if (ctl(target, VMCTX_CTL_RESUME, &reply)) return -1;
		held_event[index] = 0;
	}
	return -1;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL object-binding line %d: %s errno=%d expired=%d\n", __LINE__, #x, errno, expired); goto done; } } while (0)
int main(int argc, char **argv)
{
	if (argc != 3 && argc != 4) return 2;
	if (argc == 4) {
		registry_mode=!strcmp(argv[3],"registry");
		if (!registry_mode && strcmp(argv[3], "handles")) return 2;
		handle_mode=1;
	}
	long run_nr = strtol(argv[1], NULL, 10); ctl_nr = strtol(argv[2], NULL, 10);
	int pass = 0, old = -1, shared = -1, alias = -1, next = -1, retry = -1, third = -1;
	void *stack = MAP_FAILED, *bad = MAP_FAILED, *private_map = MAP_FAILED, *shared_map = MAP_FAILED;
	uint64_t inode = 0, retained_inode = 0;
	struct execution_mm_registry registry=EXECUTION_MM_REGISTRY_INIT;
	struct execution_binding moving,peer;
	struct execution_mm *old_mm=NULL,*new_mm=NULL;
	struct execution_mm_view old_view={0};
	unsigned bindings=0;
	struct vmctx_object q = request(VMCTX_OBJECT_CAPS, -1, 0);
	struct sigaction sa = {.sa_handler = deadline};
	CHECK(!sigaction(SIGALRM, &sa, NULL)); alarm(25);
	CHECK(!ctl(0, VMCTX_CTL_OBJECT, &q));
	CHECK(q.features == (VMCTX_OBJECT_RETAINED | VMCTX_OBJECT_REPLACE_MM));
	old = object("binding-old", 11, 12); shared = object("binding-shared", 21, 22);
	CHECK(old > 0 && shared > 0);
	char path[80]; snprintf(path, sizeof(path), "/proc/self/fd/%d", old);
	alias = open(path, O_RDWR | O_CLOEXEC); CHECK(alias > 0);
	/* A different struct file for the same inode must also be unmapped. */
	private_map = mmap((void *)BASE, 4096, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_FIXED_NOREPLACE, alias, BASE);
	shared_map = mmap((void *)(BASE + 4096), 4096, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_FIXED_NOREPLACE, shared, BASE + 4096);
	stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	bad = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(private_map != MAP_FAILED && shared_map != MAP_FAILED && stack != MAP_FAILED && bad != MAP_FAILED);
	CHECK(*(uint64_t *)private_map == 11 && *(uint64_t *)shared_map == 22);
	struct vmctx_run_config cfg = {.flags = VMCTX_FLAG_USERCODE | VMCTX_FLAG_WAIT_MONITOR |
		VMCTX_FLAG_RESTORE | VMCTX_FLAG_REDIRECT_FAULT | VMCTX_FLAG_REDIRECT_SYSCALL,
		.backing_fd = old, .shared_fd = shared, .max_exits = 1024};
	struct vmctx_cpu_model model;
	CHECK(!ctl(0, VMCTX_CTL_CPU_CAPS, &model));
	for (unsigned i = 0; i < 2; i++) {
		pid_t parent = getpid(), pid = fork(); CHECK(pid >= 0);
		if (!pid) {
			if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
			syscall(run_nr, &cfg); _exit(126);
		}
		children[i] = pid;
		int attached = 0;
		for (unsigned j = 0; j < 2000 && !expired; j++) {
			if (!ctl(pid, VMCTX_CTL_ATTACH, NULL)) { attached = 1; break; }
			usleep(1000);
		}
		CHECK(attached);
		if (handle_mode) {
			struct vmctx_context c = {.version=VMCTX_CONTEXT_ABI,.size=sizeof(c),.op=VMCTX_CONTEXT_OPEN};
			CHECK(!ctl(pid,VMCTX_CTL_CONTEXT,&c)); context_fd[i]=c.fd;context_id[i]=c.identity;
			c=(struct vmctx_context){.version=VMCTX_CONTEXT_ABI,.size=sizeof(c),.op=VMCTX_CONTEXT_INFO};
			CHECK(!ctl(pid,VMCTX_CTL_CONTEXT,&c));context_mm[i]=c.mm_identity;
			CHECK(context_id[i] && context_mm[i] && !c.flags);
		}
		struct vmctx_cpu_state cpu;
		CHECK(!ctl(pid, VMCTX_CTL_CPU_MODEL, &model));
		CHECK(!ctl(pid, VMCTX_CTL_GETCPU, &cpu));
		cpu.regs.rip = (uintptr_t)object_probe; cpu.regs.rsp = (uintptr_t)stack + 65536 - 256;
		cpu.regs.rdi = BASE; cpu.regs.rflags = 0x202; cpu.regs.orig_rax = UINT64_MAX;
		CHECK(!ctl(pid, VMCTX_CTL_SETCPU, &cpu));
		q = request(VMCTX_OBJECT_INFO, -1, 0); CHECK(!ctl(pid, VMCTX_CTL_OBJECT, &q));
		CHECK(q.epoch == 1 && q.inode && (!i || q.inode == retained_inode));
		retained_inode = q.inode;
	}
	/* Created after both children: no execution task has this descriptor. */
	next = object("binding-new", 33, 44); third = object("binding-third", 55, 66);
	CHECK(next > 0 && third > 0);
	if(registry_mode) {
		old_mm=execution_mm_resolve(&registry,UINT64_C(0x100000001),0,registry_object,&old);
		new_mm=execution_mm_resolve(&registry,UINT64_C(0x200000001),0,registry_object,&next);
		CHECK(old_mm && new_mm);
		CHECK(!execution_binding_init(&moving,old_mm,1));bindings++;
		CHECK(!execution_binding_init(&peer,old_mm,1));bindings++;
		old_view=execution_binding_view(&moving);
	}
	q = request(VMCTX_OBJECT_REPLACE, alias, 1);
	CHECK(ctl(children[0], VMCTX_CTL_OBJECT, &q) == -1 && errno == EINVAL);
	struct vmctx_quiesce lease = {.version = VMCTX_QUIESCE_ABI,
		.size = sizeof(lease), .op = VMCTX_QUIESCE_BEGIN};
	CHECK(!ctl(children[0], VMCTX_CTL_QUIESCE, &lease));
	q = request(VMCTX_OBJECT_REPLACE, next, 1);
	CHECK(ctl(children[0], VMCTX_CTL_OBJECT, &q) == -1 && errno == EBUSY);
	lease.op = VMCTX_QUIESCE_END; CHECK(!ctl(0, VMCTX_CTL_QUIESCE, &lease));
	CHECK(mapping(children[0], BASE, &inode) == 1 && inode == retained_inode);
	if(registry_mode) {
		struct replace_adapter adapter={.target=children[0],.bad=bad,.inject_copyout=1};
		struct linux_execution_object native={.control=adapter_control,.opaque=&adapter};
		CHECK(!execution_binding_replace(&moving,old_view,new_mm,linux_execution_object_replace,&native));
		CHECK(adapter.calls==2 && execution_binding_view(&moving).mm==new_mm &&
		      execution_binding_view(&moving).epoch==2 && execution_binding_view(&peer).mm==old_mm);
	} else {
		*(struct vmctx_object *)bad = request(VMCTX_OBJECT_REPLACE, next, 1);
		CHECK(!mprotect(bad, 4096, PROT_READ));
		CHECK(ctl(children[0], VMCTX_CTL_OBJECT, bad) == -1 && errno == EFAULT);
	}
	CHECK(mapping(children[0], BASE, &inode) == 0);
	CHECK(mapping(children[0], BASE + 4096, &inode) == 0);
	CHECK(mapping(children[1], BASE, &inode) == 1 && inode == retained_inode);
	CHECK(*(uint64_t *)private_map == 11 && *(uint64_t *)shared_map == 22);
	CHECK(!probe(children[0], 33, 44));
	CHECK(!probe(children[1], 11, 22));
	q = request(VMCTX_OBJECT_INFO, -1, 0); CHECK(!ctl(children[0], VMCTX_CTL_OBJECT, &q));
	CHECK(q.epoch == 2 && q.inode != retained_inode); retained_inode = q.inode;
	CHECK(mapping(children[0], BASE, &inode) == 1 && inode == retained_inode);
	snprintf(path, sizeof(path), "/proc/self/fd/%d", next);
	retry = open(path, O_RDWR | O_CLOEXEC); CHECK(retry > 0);
	CHECK(!close(next)); next = -1;
	q = request(VMCTX_OBJECT_REPLACE, retry, 1);
	CHECK(!ctl(children[0], VMCTX_CTL_OBJECT, &q) && q.epoch == 2);
	CHECK(mapping(children[0], BASE, &inode) == 1 && inode == retained_inode);
	q = request(VMCTX_OBJECT_REPLACE, third, 1);
	CHECK(ctl(children[0], VMCTX_CTL_OBJECT, &q) == -1 && errno == ESTALE);
	CHECK(mapping(children[0], BASE, &inode) == 1 && inode == retained_inode);
	if(registry_mode) {
		/* A delayed transfer still owns old_view's object. Its mapping
		 * control must not target the context that has since rebound. */
		uint64_t late=77,old_value=0,new_value=0;
		CHECK(pwrite(old_view.mm->backing_fd,&late,sizeof(late),BASE)==sizeof(late));
		pid_t target=children[0];
		CHECK(execution_binding_control(&moving,old_view,guarded_write,&target)==-1 && errno==ESTALE);
		struct vmctx_mem read={.addr=BASE,.len=sizeof(old_value),.buf=(uintptr_t)&old_value};
		CHECK(syscall(ctl_nr,context_selector(children[1]),VMCTX_CTL_PEEK,&read)==sizeof(old_value));
		read.buf=(uintptr_t)&new_value;
		CHECK(syscall(ctl_nr,context_selector(children[0]),VMCTX_CTL_PEEK,&read)==sizeof(new_value));
		CHECK(old_value==77 && new_value==33 && *(uint64_t *)private_map==77);
	}
	/* A running task cannot have its storage rebound by the monitor. */
	struct vmctx_reply reply = {.action = VMCTX_ACT_DONE};
	CHECK(!ctl(children[0], VMCTX_CTL_RESUME, &reply));
	held_event[0] = 0;
	q = request(VMCTX_OBJECT_REPLACE, third, 2);
	CHECK(ctl(children[0], VMCTX_CTL_OBJECT, &q) == -1 && errno == EBUSY);
	CHECK(mapping(children[0], BASE, &inode) == 1 && inode == retained_inode);
	pass = 1;
done:
	/* A taken syscall event is a reply owed by this monitor. Release both
	 * test tasks before waiting for either, then kill both before reaping.
	 * Waiting with the other event still held can strand native teardown. */
	for (unsigned i = 0; i < 2; i++) if (children[i] > 0) {
		if (held_event[i]) {
			struct vmctx_reply finish = {.action = VMCTX_ACT_DONE};
			(void)syscall(ctl_nr, context_selector(children[i]), VMCTX_CTL_RESUME, &finish);
		}
		kill(children[i], SIGKILL);
	}
	for (unsigned i = 0; i < 2; i++) if (children[i] > 0) {
		pid_t pid = children[i], got; int status;
		do { got = waitpid(pid, &status, 0); } while (got < 0 && errno == EINTR);
		if (got != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) pass = 0;
		children[i] = 0;
	}
	alarm(0);
	for (unsigned i=0;i<2;i++) if (context_fd[i]>=0) {
		struct vmctx_context c={.version=VMCTX_CONTEXT_ABI,.size=sizeof(c),.op=VMCTX_CONTEXT_INFO};
		if (syscall(ctl_nr,-(context_fd[i]+1),VMCTX_CTL_CONTEXT,&c) ||
		    c.identity!=context_id[i] || c.mm_identity!=context_mm[i] ||
		    c.flags!=VMCTX_CONTEXT_ENDED || c.exit_status) pass=0;
		close(context_fd[i]);context_fd[i]=-1;
	}
	int files[] = {old, shared, alias, next, retry, third};
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) if (files[i] >= 0) close(files[i]);
	if (private_map != MAP_FAILED) munmap(private_map, 4096);
	if (shared_map != MAP_FAILED) munmap(shared_map, 4096);
	if (stack != MAP_FAILED) munmap(stack, 65536);
	if (bad != MAP_FAILED) munmap(bad, 4096);
	if(bindings>0)pthread_rwlock_destroy(&moving.lock);
	if(bindings>1)pthread_rwlock_destroy(&peer.lock);
	execution_mm_registry_destroy(&registry);
	if(pass && registry_mode)puts("PASS: MM registry commits through native copyout failure and fences late old-MM controls while preserving a surviving peer's bytes");
	if (pass) puts("PASS: retained object replacement preserves other MMs, rejects shared-MM leases and stale epochs, and replays copyout without removing new maps");
	return pass ? 0 : 1;
}
