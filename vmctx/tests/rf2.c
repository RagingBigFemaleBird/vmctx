// SPDX-License-Identifier: GPL-2.0
/*
 * rf2 — a path string in a private file mapping, written by the program,
 * mprotected read-only, then handed to open(2). The Firefox shape, made
 * deterministic.
 *
 * What Firefox actually did: its loader wrote relocations into a private file
 * mapping (the page was handed to the machine running the instructions, and
 * the owner's record says THEIRS), mprotected the range read-only (RELRO),
 * and later passed a pointer into that range to a forwarded openat(2). The
 * owner's kernel refilled its absent page from the file -- the pristine,
 * unrelocated file -- because a read-only private mapping was not a class its
 * fault hook asked the monitor about. The string the syscall read was the one
 * the program had long since overwritten, and a file that exists answered
 * ENOENT. Silently: ENOENT is a legitimate answer, and a page from the right
 * file always looks right.
 *
 * The test distinguishes three outcomes, not two, and that is the point. The
 * mapped file's ORIGINAL content is itself a valid path to a decoy file, so a
 * kernel that serves the stale copy does not just fail -- it opens the decoy,
 * which names the defect in one line: the syscall read the file's bytes, not
 * the program's.
 *
 *	1. create /tmp/rf2.target ("target") and /tmp/rf2.decoy ("decoy");
 *	2. create /tmp/rf2.map whose first bytes are "/tmp/rf2.decoy";
 *	3. mmap rf2.map MAP_PRIVATE, PROT_READ|PROT_WRITE;
 *	4. overwrite the page with "/tmp/rf2.target" -- the store happens on
 *	   the machine running the instructions, so the page changes hands and
 *	   the owner's copy is now stale;
 *	5. mprotect the page PROT_READ -- the class the old hook ignored;
 *	6. open(page) and read what it opens.
 *
 * target  -> PASS: the syscall saw the program's current bytes.
 * decoy   -> FAIL: the owner refilled the page from its own file and the
 *            syscall read a string the program had overwritten.
 * ENOENT  -> FAIL: same defect, garbage instead of the decoy.
 *
 * Every store is volatile for the same reason as rf1's: the page must really
 * be written and really be read, not held in a register.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#define PGSZ 4096

static const char *tgt   = "/tmp/rf2.target";
static const char *decoy = "/tmp/rf2.decoy";
static const char *mapf  = "/tmp/rf2.map";

static int put(const char *path, const void *buf, size_t n)
{
	int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);

	if (fd < 0)
		return -1;
	if (write(fd, buf, n) != (ssize_t)n) {
		close(fd);
		return -1;
	}
	return close(fd);
}

int main(void)
{
	char page[PGSZ];
	char back[64];
	volatile char *m;
	ssize_t got;
	int fd, i;

	if (put(tgt, "target\n", 7) || put(decoy, "decoy\n", 6)) {
		printf("rf2: could not create the two files\nFAIL\n");
		return 1;
	}
	memset(page, 'X', sizeof(page));
	memcpy(page, decoy, strlen(decoy) + 1);
	if (put(mapf, page, sizeof(page))) {
		printf("rf2: could not create the mapped file\nFAIL\n");
		return 1;
	}

	fd = open(mapf, O_RDONLY);
	if (fd < 0) {
		printf("rf2: could not reopen the mapped file\nFAIL\n");
		return 1;
	}
	m = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	close(fd);
	if ((void *)m == MAP_FAILED) {
		printf("rf2: mmap failed\nFAIL\n");
		return 1;
	}

	/* The store that moves the page: the owner's copy is now stale. */
	for (i = 0; tgt[i]; i++)
		m[i] = tgt[i];
	m[i] = '\0';

	/* The class the old hook never asked about. */
	if (mprotect((void *)m, PGSZ, PROT_READ) != 0) {
		printf("rf2: mprotect failed\nFAIL\n");
		return 1;
	}

	fd = open((const char *)m, O_RDONLY);
	if (fd < 0) {
		printf("rf2: open(mapped path) failed: %s -- the owner read a "
		       "stale page where the program had written \"%s\"\nFAIL\n",
		       strerror(errno), tgt);
		return 1;
	}
	got = read(fd, back, sizeof(back) - 1);
	close(fd);
	back[got > 0 ? got : 0] = '\0';

	unlink(tgt);
	unlink(decoy);
	unlink(mapf);

	if (got == 7 && !strncmp(back, "target\n", 7)) {
		printf("rf2: the syscall saw the program's current bytes\nPASS\n");
		return 0;
	}
	if (got > 0 && !strncmp(back, "decoy\n", 6)) {
		printf("rf2: the syscall opened the DECOY -- the owner refilled "
		       "the page from its own file over the program's write\nFAIL\n");
		return 1;
	}
	printf("rf2: opened something unexpected (%zd bytes: \"%s\")\nFAIL\n",
	       got, back);
	return 1;
}
