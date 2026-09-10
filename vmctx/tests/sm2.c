/*
 * sm2 — shared memory between two processes that share nothing else.
 *
 * sm1 shares by *inheritance*: one process makes the mapping and a fork carries
 * it, or one process maps the same file twice. Every case there has a single
 * origin, so the side that owns the program can see both ends of the sharing at
 * the moment it is created.
 *
 * This is the other way: two processes that each open the object by *name* and
 * map it independently. Neither mapping knows about the other. The only thing
 * that makes them one page is that the name resolves to the same file, and the
 * only machine that can know that is the one holding the file -- the
 * destination is handed an address, a length and an offset and is told nothing
 * about why.
 *
 * A fork is still used to get two processes, but the child does not inherit the
 * mapping: it closes and re-opens by name after the fork, so the sharing has to
 * be established from the name and not from the inheritance. The pipe carries
 * only sequencing, never data.
 *
 *   file-by-name   both open the same /tmp file and mmap MAP_SHARED. Child
 *                  writes, parent sees it; parent writes, child sees it.
 *   shm-by-name    the same through shm_open(3), which is a different path in
 *                  the kernel and a different one here.
 *   sysv-by-key    shmget with one key in both, attached separately. The
 *                  destination is never told System V exists; it is handed an
 *                  address, a length and an offset like anything else.
 *   file-writeback a MAP_SHARED store must reach the *file*, so a plain read(2)
 *                  of it afterwards sees the new bytes. This is the case that
 *                  separates real sharing from a private copy that happens to
 *                  be visible to both because both were filled from the same
 *                  place.
 *
 * Each direction is checked, because one-way visibility is a real and
 * distinguishable failure: a private copy-on-write mapping shows the writer its
 * own store and shows the other side nothing.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>

#define PGSZ 4096

static int fails;

static void say(const char *s)
{
	if (write(1, s, strlen(s)) < 0)
		_exit(1);
}

static void sayf(const char *fmt, ...)
{
	char b[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	say(b);
}

static void check(const char *name, int ok, const char *fmt, ...)
{
	char b[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (ok) {
		sayf("%-14s ok    %s\n", name, b);
		return;
	}
	fails++;
	sayf("%-14s FAIL  %s\n", name, b);
}

/* One byte down the pipe, and one back: enough to order two processes without
 * carrying any of the data under test. */
static int tick(int fd)
{
	char c = 'x';

	return write(fd, &c, 1) == 1 ? 0 : -1;
}

static int wait_tick(int fd)
{
	char c;

	return read(fd, &c, 1) == 1 ? 0 : -1;
}

/*
 * The shape every case uses.
 *
 * open_fn is called separately in each process and must return a mapping of the
 * same underlying object. The parent writes 0xA1 at offset 0 and the child must
 * see it; the child writes 0xC2 at offset 2048 and the parent must see that.
 * Two directions, because a private mapping passes a one-directional test.
 */
static void two_process_case(const char *name, void *(*open_fn)(int child),
			     void (*cleanup)(void))
{
	int down[2], up[2];
	pid_t kid;
	volatile unsigned char *p;
	int st = 0;
	int saw_parent_write = 0;

	if (pipe(down) != 0 || pipe(up) != 0) {
		check(name, 0, "pipe: %s", strerror(errno));
		return;
	}
	kid = fork();
	if (kid < 0) {
		check(name, 0, "fork: %s", strerror(errno));
		return;
	}

	if (kid == 0) {
		close(down[1]);
		close(up[0]);
		/* Map it here, by name, after the fork: nothing is inherited. */
		p = open_fn(1);
		if (!p)
			_exit(2);
		if (wait_tick(down[0]) != 0)		/* parent has written */
			_exit(3);
		if (p[0] != 0xA1)
			_exit(4);			/* did not see it */
		p[2048] = 0xC2;
		if (tick(up[1]) != 0)			/* child has written */
			_exit(5);
		if (wait_tick(down[0]) != 0)		/* parent is done */
			_exit(6);
		_exit(0);
	}

	close(down[0]);
	close(up[1]);
	p = open_fn(0);
	if (!p) {
		check(name, 0, "parent could not map it: %s", strerror(errno));
		kill(kid, SIGKILL);
		waitpid(kid, &st, 0);
		return;
	}
	p[0] = 0xA1;
	if (tick(down[1]) != 0 || wait_tick(up[0]) != 0) {
		check(name, 0, "sequencing failed");
		kill(kid, SIGKILL);
		waitpid(kid, &st, 0);
		return;
	}
	saw_parent_write = (p[2048] == 0xC2);
	(void)tick(down[1]);
	waitpid(kid, &st, 0);
	close(down[1]);
	close(up[0]);
	if (cleanup)
		cleanup();

	if (!WIFEXITED(st)) {
		check(name, 0, "child died (status 0x%x)", st);
		return;
	}
	if (WEXITSTATUS(st) == 4) {
		check(name, 0, "the child did not see the parent's write "
		      "(parent->child sharing is broken)");
		return;
	}
	if (WEXITSTATUS(st) != 0) {
		check(name, 0, "child exited %d", WEXITSTATUS(st));
		return;
	}
	check(name, saw_parent_write,
	      saw_parent_write ? "both directions visible"
			       : "the parent did not see the child's write "
				 "(child->parent sharing is broken)");
}

/* ---- file by name ---------------------------------------------------- */

static char file_path[] = "/tmp/sm2f.XXXXXX";

static void *open_file(int child)
{
	int fd = open(file_path, O_RDWR);
	void *p;

	(void)child;
	if (fd < 0)
		return NULL;
	p = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	return p == MAP_FAILED ? NULL : p;
}

/* No cleanup hook: main() unlinks file_path once, after the writeback case has
 * finished with it too. */

/* ---- POSIX shm by name ----------------------------------------------- */

static char shm_name[64];

static void *open_shm(int child)
{
	int fd = shm_open(shm_name, O_RDWR, 0600);
	void *p;

	(void)child;
	if (fd < 0)
		return NULL;
	p = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	return p == MAP_FAILED ? NULL : p;
}

static void cleanup_shm(void)
{
	shm_unlink(shm_name);
}

/* ---- System V by key -------------------------------------------------- */

static key_t sysv_key;

static void *open_sysv(int child)
{
	int id = shmget(sysv_key, PGSZ, 0600);
	void *p;

	(void)child;
	if (id < 0)
		return NULL;
	p = shmat(id, NULL, 0);
	return p == (void *)-1 ? NULL : p;
}

static void cleanup_sysv(void)
{
	int id = shmget(sysv_key, PGSZ, 0600);

	if (id >= 0)
		shmctl(id, IPC_RMID, NULL);
}

/*
 * A MAP_SHARED store has to reach the file, not just the other mapping. If the
 * destination quietly turned the mapping into anonymous memory, two mappings
 * could still agree with each other (both filled from the same place) while the
 * file never changes -- so this is what tells real sharing from a shared
 * illusion.
 */
static void case_writeback(void)
{
	int fd;
	unsigned char *p;
	unsigned char buf[4] = { 0 };
	ssize_t n;

	fd = open(file_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		check("file-writeback", 0, "open: %s", strerror(errno));
		return;
	}
	if (ftruncate(fd, PGSZ) != 0) {
		close(fd);
		check("file-writeback", 0, "ftruncate: %s", strerror(errno));
		return;
	}
	p = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		check("file-writeback", 0, "mmap: %s", strerror(errno));
		return;
	}
	memcpy(p, "\xde\xad\xbe\xef", 4);
	if (msync(p, PGSZ, MS_SYNC) != 0) {
		munmap(p, PGSZ);
		close(fd);
		check("file-writeback", 0, "msync: %s", strerror(errno));
		return;
	}
	munmap(p, PGSZ);

	if (lseek(fd, 0, SEEK_SET) != 0) {
		close(fd);
		check("file-writeback", 0, "lseek: %s", strerror(errno));
		return;
	}
	n = read(fd, buf, 4);
	close(fd);
	unlink(file_path);
	check("file-writeback", n == 4 && memcmp(buf, "\xde\xad\xbe\xef", 4) == 0,
	      "read=%zd got=%02x%02x%02x%02x (want 4, deadbeef)",
	      n, buf[0], buf[1], buf[2], buf[3]);
}

int main(void)
{
	int fd;

	/* file */
	fd = mkstemp(file_path);
	if (fd < 0) {
		sayf("mkstemp: %s\nFAIL\n", strerror(errno));
		return 1;
	}
	if (ftruncate(fd, PGSZ) != 0) {
		sayf("ftruncate: %s\nFAIL\n", strerror(errno));
		return 1;
	}
	close(fd);
	two_process_case("file-by-name", open_file, NULL);

	/* POSIX shm */
	snprintf(shm_name, sizeof(shm_name), "/sm2-%d", (int)getpid());
	fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		check("shm-by-name", 0, "shm_open: %s", strerror(errno));
	} else {
		if (ftruncate(fd, PGSZ) != 0)
			check("shm-by-name", 0, "ftruncate: %s", strerror(errno));
		close(fd);
		two_process_case("shm-by-name", open_shm, cleanup_shm);
	}

	/* System V */
	sysv_key = (key_t)(0x736d3200 ^ getpid());
	{
		int id = shmget(sysv_key, PGSZ, IPC_CREAT | IPC_EXCL | 0600);

		if (id < 0)
			check("sysv-by-key", 0, "shmget: %s", strerror(errno));
		else
			two_process_case("sysv-by-key", open_sysv, cleanup_sysv);
	}

	case_writeback();
	unlink(file_path);

	say(fails == 0 ? "PASS\n" : "FAIL\n");
	return fails == 0 ? 0 : 1;
}
