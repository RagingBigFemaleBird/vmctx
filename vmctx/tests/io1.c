// SPDX-License-Identifier: GPL-2.0
/*
 * io1 -- forwarded-I/O buffer integrity oracle.
 *
 * The corner this holds shut was found in session 40, by gzip, once in seven
 * runs: a page of a read(2) buffer read back ONE BLOCK STALE, because the
 * source's readahead landed a page into a RUNNING context and overwrote the
 * bytes the call had just copied there. Nothing in the suite watched the
 * CONTENT of a forwarded read's buffer across many reads through one buffer;
 * hx2 watches one contended page, rd1 watches a shared file's first bytes,
 * and neither would have seen this.
 *
 * The arrangement makes staleness identifiable, not just detectable:
 *   - A file is written once, block by block, every byte a function of its
 *     (block, offset): byte(b, off) = (37*b + off) & 0xff. 37 is odd, so at
 *     any fixed offset all 64 blocks carry distinct bytes -- a page served
 *     from the WRONG block names which block it came from.
 *   - For RUN_SECS the file is read back through ONE static buffer (the
 *     hand-over pressure: every read makes the source want the buffer's
 *     pages, every verify makes this side want them back), the buffer
 *     POISONED with 0xAA before each read so "the read never landed" cannot
 *     alias "the bytes happen to match".
 *   - Every byte of every block is verified. A mismatch is classified: all
 *     poison = the read did not write; matching some other block b' = a
 *     stale page, b' says whose; anything else = corruption. Any mismatch is
 *     a FAIL, counted, and the run keeps going -- one run measures a rate.
 *
 * Wall-clock bounded; always finishes with a verdict. Native this passes
 * trivially: one kernel, one buffer, no hand-overs.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

#define BLK		(32 * 1024)
#define NBLK		64		/* a 2 MiB file                       */
#define RUN_SECS	10

static unsigned char buf[BLK];		/* bss: one fixed buffer, always      */

static unsigned char expect(int b, int off)
{
	return (unsigned char)(37 * b + off);
}

static unsigned long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)ts.tv_sec * 1000000ul +
	       (unsigned long)ts.tv_nsec / 1000;
}

int main(void)
{
	char path[64];
	int fd, b, off;
	unsigned long loops = 0, blocks = 0;
	unsigned long stale = 0, unwritten = 0, garbled = 0;
	unsigned long t_end;

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	snprintf(path, sizeof(path), "/tmp/io1.%d.dat", (int)getpid());
	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (fd < 0) {
		perror("io1: create");
		return 2;
	}
	for (b = 0; b < NBLK; b++) {
		for (off = 0; off < BLK; off++)
			buf[off] = expect(b, off);
		if (write(fd, buf, BLK) != BLK) {
			perror("io1: write");
			unlink(path);
			return 2;
		}
	}
	if (close(fd) != 0) {
		perror("io1: close");
		unlink(path);
		return 2;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("io1: reopen");
		unlink(path);
		return 2;
	}

	t_end = now_us() + RUN_SECS * 1000000ul;
	while (now_us() < t_end) {
		if (lseek(fd, 0, SEEK_SET) != 0) {
			perror("io1: lseek");
			break;
		}
		for (b = 0; b < NBLK && now_us() < t_end; b++) {
			long got;

			memset(buf, 0xAA, BLK);
			got = read(fd, buf, BLK);
			/*
			 * The instrument's own proof (measure-with-controls):
			 * IO1_SELFTEST=1 forges exactly the failure this case
			 * exists to catch -- one page of one early block
			 * replaced with the PREVIOUS block's bytes after the
			 * read -- and the run must then FAIL and name block
			 * b-1 as the stale source. A selftest that passes
			 * PASS is the broken instrument.
			 */
			if (b == 3 && loops == 0 && getenv("IO1_SELFTEST"))
				for (off = 4096; off < 8192; off++)
					buf[off] = expect(b - 1, off);
			if (got != BLK) {
				fprintf(stderr, "io1: read of block %d "
					"returned %ld\n", b, got);
				garbled++;
				break;
			}
			blocks++;
			for (off = 0; off < BLK; off++) {
				int bad_from = off, poison = 1, b2, hit = -1;
				int run;

				if (buf[off] == expect(b, off))
					continue;
				/*
				 * Classify the first 64 bytes of the damage:
				 * whose bytes are these? Page-aligned checks
				 * would presume the failure's shape; a run of
				 * 64 identifies a block without presuming.
				 */
				for (run = 0; run < 64 && off + run < BLK; run++)
					if (buf[off + run] != 0xAA)
						poison = 0;
				if (poison) {
					unwritten++;
					fprintf(stderr, "io1: block %d bytes "
						"%d.. still hold the POISON: "
						"the read never wrote them "
						"(loop %lu)\n", b, off, loops);
				} else {
					for (b2 = 0; b2 < NBLK; b2++) {
						int all = 1;

						if (b2 == b)
							continue;
						for (run = 0; run < 64 &&
						     off + run < BLK; run++)
							if (buf[off + run] !=
							    expect(b2, off + run))
								all = 0;
						if (all) {
							hit = b2;
							break;
						}
					}
					if (hit >= 0) {
						stale++;
						fprintf(stderr, "io1: block %d "
							"bytes %d.. hold BLOCK "
							"%d's bytes: a STALE "
							"page (loop %lu)\n",
							b, bad_from, hit, loops);
					} else {
						garbled++;
						fprintf(stderr, "io1: block %d "
							"bytes %d.. hold "
							"neither poison nor any "
							"block: garbled (loop "
							"%lu, got %02x want "
							"%02x)\n", b, bad_from,
							loops, buf[off],
							expect(b, off));
					}
				}
				/* One report per page of damage, then move on. */
				off = ((off >> 12) + 1) << 12;
				if (off > 0)
					off--;	/* the loop's ++ lands on it */
			}
		}
		loops++;
	}
	close(fd);
	unlink(path);

	fprintf(stderr, "io1: %lu pass(es) over %d x %d KiB, %lu block reads; "
		"stale %lu, unwritten %lu, garbled %lu\n",
		loops, NBLK, BLK / 1024, blocks, stale, unwritten, garbled);
	if (stale + unwritten + garbled) {
		printf("FAIL\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
