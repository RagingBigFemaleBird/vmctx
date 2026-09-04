/*
 * sm1 — memory shared by something other than a thread.
 *
 * Threads already share memory here, and they do it through the backing object:
 * two contexts started with the same object see the same page at the same
 * address, which is what sharing an address space means on a machine that has
 * no CLONE_VM. Everything else that shares memory arrives by a different route
 * and has to be made to work by the *same* rule -- a syscall that maps memory
 * on the machine owning the program must map it on the machine running it.
 *
 * Four kinds, from the one already handled to the ones that are not:
 *
 *   MAP_SHARED file    two mappings of one file in one process. A store
 *                      through the first must be visible through the second,
 *                      which is a property of the file, not of the mapping.
 *   POSIX shm          shm_open plus mmap: the same, with the file living in
 *                      /dev/shm on the machine that owns it.
 *   MAP_SHARED across  a fork, with both parent and child writing one page and
 *   a fork             each reading what the other wrote. Two contexts, two
 *                      address spaces, one page.
 *   SysV shm           shmget/shmat, which maps memory without going through
 *                      mmap at all -- the case the mmap path cannot cover by
 *                      construction.
 *
 * The child cases use a pipe rather than a shared word for sequencing, so a
 * failure to share memory shows up as wrong *data* and never as a hang.
 *
 * Each case prints before the next begins.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#define PGSZ 4096

static int fails;

static void say(const char *s)
{
	if (write(1, s, strlen(s)) < 0)
		_exit(1);
}

static void sayf(const char *fmt, ...)
{
	char b[384];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n > 0)
		say(b);
}

static void check(const char *what, int ok, const char *fmt, ...)
{
	char b[288];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (!ok)
		fails++;
	sayf("sm1 %s: %s -> %s\n", what, b, ok ? "ok" : "BAD");
}

/* Wait for one byte, so nothing here can deadlock on shared memory working. */
static int wait_byte(int fd)
{
	char c;

	return read(fd, &c, 1) == 1;
}

static void poke_byte(int fd)
{
	char c = 1;

	if (write(fd, &c, 1) != 1)
		_exit(1);
}

int main(void)
{
	char path[] = "/tmp/sm1.XXXXXX";
	int fd;

	fd = mkstemp(path);
	if (fd < 0 || ftruncate(fd, PGSZ) != 0) {
		say("sm1: mkstemp failed\nFAIL\n");
		return 1;
	}

	/* --- two MAP_SHARED mappings of one file, in one process. */
	{
		char *a, *b;

		say("sm1 file-shared: mapping one file twice\n");
		a = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		b = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (a == MAP_FAILED || b == MAP_FAILED) {
			check("file-shared", 0, "mmap failed");
		} else {
			strcpy(a, "written through the first mapping");
			check("file-shared",
			      strcmp(b, "written through the first mapping") == 0,
			      "second mapping reads |%.40s|", b);
			b[0] = 'W';
			check("file-shared back",
			      a[0] == 'W',
			      "a store through the second is seen by the first "
			      "as '%c'", a[0]);
		}
		if (a != MAP_FAILED)
			munmap(a, PGSZ);
		if (b != MAP_FAILED)
			munmap(b, PGSZ);
	}

	/* --- POSIX shared memory. */
	{
		const char *nm = "/sm1-posix";
		int sfd;
		char *p, *q;

		say("sm1 posix-shm: shm_open\n");
		shm_unlink(nm);
		sfd = shm_open(nm, O_CREAT | O_RDWR, 0600);
		if (sfd < 0 || ftruncate(sfd, PGSZ) != 0) {
			check("posix-shm", 0, "shm_open/ftruncate failed (%d)", sfd);
		} else {
			p = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE, MAP_SHARED,
				 sfd, 0);
			q = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE, MAP_SHARED,
				 sfd, 0);
			if (p == MAP_FAILED || q == MAP_FAILED) {
				check("posix-shm", 0, "mmap failed");
			} else {
				memcpy(p, "posix", 6);
				check("posix-shm", memcmp(q, "posix", 6) == 0,
				      "second mapping reads |%.5s|", q);
				munmap(p, PGSZ);
				munmap(q, PGSZ);
			}
			close(sfd);
			shm_unlink(nm);
		}
	}

	/* --- one page, two processes, both writing. */
	{
		int up[2], down[2];
		char *p;
		pid_t kid;

		say("sm1 fork-shared: one page across a fork\n");
		p = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED || pipe(up) < 0 || pipe(down) < 0) {
			check("fork-shared", 0, "setup failed");
		} else {
			memset(p, 0, 64);
			kid = fork();
			if (kid == 0) {
				if (!wait_byte(down[0]))
					_exit(1);
				/* The parent's bytes must be here. */
				if (memcmp(p, "from the parent", 16) != 0)
					memcpy(p + 32, "PARENT-NOT-SEEN", 16);
				else
					memcpy(p + 32, "from the child ", 16);
				poke_byte(up[1]);
				_exit(0);
			}
			memcpy(p, "from the parent", 16);
			poke_byte(down[1]);
			if (!wait_byte(up[0])) {
				check("fork-shared", 0, "the child said nothing");
			} else {
				int st = 0;

				waitpid(kid, &st, 0);
				check("fork-shared",
				      memcmp(p + 32, "from the child ", 16) == 0,
				      "the child wrote |%.15s| where the parent "
				      "can read it", p + 32);
			}
			munmap(p, PGSZ);
		}
	}

	/* --- SysV shared memory, which does not go through mmap. */
	{
		int id;
		char *p, *q;

		say("sm1 sysv-shm: shmget/shmat\n");
		id = shmget(IPC_PRIVATE, PGSZ, IPC_CREAT | 0600);
		if (id < 0) {
			check("sysv-shm", 0, "shmget failed");
		} else {
			p = shmat(id, NULL, 0);
			q = shmat(id, NULL, 0);
			if (p == (char *)-1 || q == (char *)-1) {
				check("sysv-shm", 0,
				      "shmat failed (p=%p q=%p)",
				      (void *)p, (void *)q);
			} else {
				memcpy(p, "sysv", 5);
				check("sysv-shm", memcmp(q, "sysv", 5) == 0,
				      "second attachment reads |%.4s|", q);
				shmdt(p);
				shmdt(q);
			}
			shmctl(id, IPC_RMID, NULL);
		}
	}

	close(fd);
	unlink(path);
	sayf("sm1: %d case(s) bad\n", fails);
	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
