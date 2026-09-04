/*
 * dma1 — a page pinned by real device I/O while the other machine claims it.
 *
 * Every page test in this tree moves pages through the CPU: a syscall reads or
 * writes a buffer, the monitor faults, the page changes hands. None of them
 * ever hands a page to a *device*, and that is the one case where taking the
 * page away does not stop the machine writing it.
 *
 * A read with O_DIRECT does not copy through the kernel. It pins the user
 * buffer with get_user_pages(), gives its physical address to the storage
 * driver, and returns when the device has written it. Between those two moments
 * the page's page tables say nothing about what is about to happen to it:
 * unmapping the page, or handing it to the other machine, leaves the pin and
 * the descriptor exactly as they were, and the device writes the page anyway.
 *
 * So the source pins the buffer and waits for the disk; the guest's second
 * thread touches the same page and the far side claims it; the disk completes
 * and writes into memory the other machine now owns and is writing itself.
 * Nothing in either page table records that this could happen.
 *
 * The test makes the window as wide as it can and then checks the only thing
 * that matters -- that the bytes the device delivered are the bytes the file
 * holds:
 *
 *   - the buffer is one page, mapped and page-aligned, which O_DIRECT needs;
 *   - a second thread writes the *second half* of that page continuously, so
 *     the far side has a live reason to claim it for a write while the read is
 *     outstanding;
 *   - the file's first half is a known pattern, checked after every read.
 *
 * A failure here is not a wrong answer from a syscall. It is memory written by
 * a device into a page that had been given away, which is the one kind of
 * corruption no amount of care in the page-table paths can prevent -- it has to
 * be refused before the page is handed over, by asking whether anything can
 * still reach it. See vmctx_folio_may_move() in the kernel.
 *
 * O_DIRECT is refused by some filesystems (tmpfs among them). That is reported
 * and the case is skipped rather than passed: a test that quietly turns into a
 * buffered read is testing nothing at all and would say PASS for ever.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>

#define ROUNDS 200
#define HALF   2048

static char *page;
static volatile int stop;

static void *stamper(void *p)
{
	unsigned long v = 0;

	(void)p;
	/*
	 * The second half only. The first half is the device's, and a test that
	 * scribbled on it could not tell corruption from its own writes.
	 */
	while (!stop) {
		v++;
		*(volatile unsigned long *)(page + HALF) = v;
	}
	return NULL;
}

int main(void)
{
	static char want[HALF];
	const char *path = "/var/tmp/vmctx-dma1.bin";
	pthread_t t;
	int fd, i, bad = 0, first_bad = -1;
	unsigned long stamps;

	page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) {
		printf("dma1: mmap failed\n");
		return 1;
	}
	madvise(page, 4096, MADV_NOHUGEPAGE);

	/* A file with a known first half, written normally. */
	memset(want, 'D', sizeof(want));
	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0) {
		printf("dma1: cannot create %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (write(fd, want, sizeof(want)) != (ssize_t)sizeof(want)) {
		printf("dma1: short write creating the file\n");
		close(fd);
		return 1;
	}
	close(fd);

	fd = open(path, O_RDONLY | O_DIRECT);
	if (fd < 0) {
		/*
		 * Said out loud and not passed. Without O_DIRECT the read is
		 * copied through the kernel and pins nothing, so the hazard this
		 * exists for is not present and a PASS would be a lie.
		 */
		printf("dma1: O_DIRECT unavailable on %s (%s) -- the device "
		       "path is not exercised\nSKIP\n", path, strerror(errno));
		return 0;
	}

	if (pthread_create(&t, NULL, stamper, NULL) != 0) {
		printf("dma1: pthread_create failed\n");
		close(fd);
		return 1;
	}

	for (i = 0; i < ROUNDS; i++) {
		ssize_t n;

		if (lseek(fd, 0, SEEK_SET) != 0)
			break;
		memset(page, 0, HALF);
		/* The device writes the first half of this page directly. */
		n = read(fd, page, HALF);
		if (n != (ssize_t)HALF) {
			if (first_bad < 0)
				first_bad = i;
			bad++;
			continue;
		}
		if (memcmp(page, want, HALF) != 0) {
			if (first_bad < 0)
				first_bad = i;
			bad++;
		}
	}
	stop = 1;
	pthread_join(t, NULL);
	stamps = *(volatile unsigned long *)(page + HALF);
	close(fd);
	unlink(path);

	printf("dma1: %d rounds, %d bad, first at %d, stamp reached %lu\n",
	       ROUNDS, bad, first_bad, stamps);
	printf("%s\n", (bad == 0 && stamps > 0) ? "PASS" : "FAIL");
	return (bad == 0 && stamps > 0) ? 0 : 1;
}
