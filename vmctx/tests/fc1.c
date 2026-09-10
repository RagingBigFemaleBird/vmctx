// SPDX-License-Identifier: GPL-2.0
/*
 * fc1 -- fontconfig's mapping pattern: mmap a file, read it, munmap, and let
 * the kernel hand the NEXT file the same hole -- while another thread's
 * forwarded calls compete for the change-log drain.
 *
 * The reduction of netsurf's session-42 wall: the font scan munmapped one
 * font, mmapped the next, was handed the recycled hole, and the first read
 * of the new mapping answered -14 (exit 242) -- the old mapping's vacate had
 * spilled onto a later reply, arrived stamped NEWER than the new mapping's
 * construction (the stamp was assigned at the DRAIN, a userspace race
 * between connection threads), and the destination's ordering rule then
 * legally punched the live mapping. Kernel #168 moves the stamp to the
 * event itself -- the hook stamps under the log's own lock -- and this test
 * is the net over that ordering.
 *
 * Three files of one size but distinct bytes (byte(f, off) is a function of
 * BOTH), so the same hole carries different content each round: a punched
 * mapping shows as SIGSEGV/242, and any stale page names the file it
 * belongs to by its own bytes. Thread B floods small mmap/munmap cycles so
 * more vacates pend per reply than one reply can carry (8) -- the spillover
 * that made the killer vacate ride a later reply -- and its calls drain the
 * shared change log concurrently, which is the stamp race itself.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define NFILES	3
#define FSIZE	(2u << 20)	/* 2 MiB: big enough to make hole reuse the
				 * kernel's natural choice, small enough to
				 * scan hundreds of rounds in seconds */
#define SECONDS	8

static char path[NFILES][64];
static volatile int stop;
static unsigned long reused, rounds;

static unsigned char byte_of(int f, size_t off)
{
	return (unsigned char)((37u * (unsigned)(f + 1)) ^ (off * 17u));
}

static void *churn(void *arg)
{
	(void)arg;
	/*
	 * Small map/unmap cycles: each munmap is one hook event, so a burst
	 * of them leaves more pending vacates than one reply ships (8), and
	 * the spillover rides later replies -- the killer's vehicle. The
	 * getppid keeps a steady stream of cheap forwarded calls draining
	 * the log from this connection, racing the scanner's own drains.
	 */
	while (!stop) {
		void *m[4];
		int i;

		for (i = 0; i < 4; i++)
			m[i] = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		for (i = 0; i < 4; i++)
			if (m[i] != MAP_FAILED) {
				*(volatile char *)m[i] = 1;
				munmap(m[i], 8192);
			}
		getppid();
	}
	return NULL;
}

int main(void)
{
	pthread_t th;
	void *prev = NULL;
	time_t t0;
	int f, bad = 0;
	size_t off;

	for (f = 0; f < NFILES; f++) {
		int fd;
		size_t o;
		static char buf[65536];

		snprintf(path[f], sizeof(path[f]), "/tmp/fc1.%d.%d",
			 (int)getpid(), f);
		fd = open(path[f], O_CREAT | O_TRUNC | O_WRONLY, 0644);
		if (fd < 0) {
			printf("FAIL create %s: %s\n", path[f], strerror(errno));
			return 2;
		}
		for (o = 0; o < FSIZE; o += sizeof(buf)) {
			size_t i;

			for (i = 0; i < sizeof(buf); i++)
				buf[i] = (char)byte_of(f, o + i);
			if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
				printf("FAIL write %s: %s\n", path[f],
				       strerror(errno));
				close(fd);
				return 2;
			}
		}
		close(fd);
	}

	if (pthread_create(&th, NULL, churn, NULL) != 0) {
		printf("FAIL pthread_create: %s\n", strerror(errno));
		return 2;
	}

	t0 = time(NULL);
	for (f = 0; !stop && time(NULL) - t0 < SECONDS; f = (f + 1) % NFILES) {
		int fd = open(path[f], O_RDONLY);
		unsigned char *p;

		if (fd < 0) {
			printf("FAIL open %s: %s\n", path[f], strerror(errno));
			bad++;
			break;
		}
		p = mmap(NULL, FSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
		close(fd);
		if (p == MAP_FAILED) {
			printf("FAIL mmap %s: %s\n", path[f], strerror(errno));
			bad++;
			break;
		}
		if ((void *)p == prev)
			reused++;
		prev = (void *)p;
		/*
		 * First, middle and last page, plus a stride across the rest:
		 * the death was the FIRST read of the fresh mapping, and a
		 * stale page anywhere names its file by its own bytes.
		 */
		for (off = 0; off < FSIZE; off += 4096 * 37) {
			unsigned char want = byte_of(f, off), got = p[off];

			if (got != want) {
				int sf;

				for (sf = 0; sf < NFILES; sf++)
					if (got == byte_of(sf, off))
						break;
				printf("FAIL round %lu file %d offset 0x%zx: "
				       "read 0x%02x want 0x%02x (%s)\n",
				       rounds, f, off, got, want,
				       sf < NFILES && sf != f
				       ? "a STALE page of the previous file"
				       : "neither file's bytes");
				if (++bad > 5)
					break;
			}
		}
		if (p[FSIZE - 1] != byte_of(f, FSIZE - 1)) {
			printf("FAIL round %lu file %d: last byte 0x%02x want "
			       "0x%02x\n", rounds, f, p[FSIZE - 1],
			       byte_of(f, FSIZE - 1));
			bad++;
		}
		munmap(p, FSIZE);
		rounds++;
		if (bad > 5)
			break;
	}
	stop = 1;
	pthread_join(th, NULL);
	for (f = 0; f < NFILES; f++)
		unlink(path[f]);

	fprintf(stderr, "fc1: %lu rounds, %lu with the hole reused\n",
		rounds, reused);
	if (rounds < 10) {
		/* A machine too slow for the pattern is not a pass. */
		printf("FAIL only %lu rounds in %d seconds\n", rounds, SECONDS);
		return 1;
	}
	if (bad) {
		printf("FAIL %d bad read(s)\n", bad);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
