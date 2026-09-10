// SPDX-License-Identifier: GPL-2.0
/* A writable memfd snapshot must survive sealing and a read-only remap while
 * another thread makes source calls. This is the SharedStringMap lifetime used
 * by Firefox, reduced to byte checks without browser dependencies. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif
#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif
#ifndef SNAPSHOT_OBSERVERS
#define SNAPSHOT_OBSERVERS 1
#endif
#ifndef SNAPSHOT_MUTATE
#define SNAPSHOT_MUTATE 0
#endif

static atomic_int stop, ready, worker_error;
static atomic_ulong calls;
static int mutate = SNAPSHOT_MUTATE;

static void *observer(void *unused)
{
	(void)unused;
	atomic_fetch_add(&ready, 1);
	while (!atomic_load(&stop)) {
		struct timespec now;
		if (mutate) {
			unsigned char *p = mmap(NULL, 16384, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (p == MAP_FAILED) { atomic_store(&worker_error, errno); break; }
			/* Mapping reuse must not resurrect an earlier shared snapshot
			 * or retained private page. Writing just p[0] masked that loss. */
			for (size_t i = 0; i < 16384; i++) if (p[i]) {
				fprintf(stderr, "FAIL: fresh private mapping byte=%zu got=%02x calls=%lu\n",
					i, p[i], atomic_load(&calls));
				atomic_store(&worker_error, EILSEQ);
				break;
			}
			if (atomic_load(&worker_error)) { munmap(p, 16384); break; }
			p[0] = 0x73;
			if (munmap(p, 16384)) { atomic_store(&worker_error, errno); break; }
		}
		if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &now)) {
			atomic_store(&worker_error, errno);
			break;
		}
		atomic_fetch_add(&calls, 1);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	const size_t length = 182;
	unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 128;
	unsigned observers = SNAPSHOT_OBSERVERS;
	pthread_t threads[64];
	unsigned char expected[182];
	if (argc >= 4) {
		if (strcmp(argv[3], "mutate")) return 2;
		mutate = 1;
	}
	if (argc >= 3) {
		char *end;
		if (!strcmp(argv[2], "single")) observers = 0;
		else {
			unsigned long n = strtoul(argv[2], &end, 10);
			if (!*argv[2] || *end || n > 64) return 2;
			observers = n;
		}
	}
	if (!rounds || rounds > 100000) return 2;
	if (observers) {
		for (unsigned i = 0; i < observers; i++)
			if (pthread_create(&threads[i], NULL, observer, NULL)) return 2;
		while ((unsigned)atomic_load(&ready) != observers || !atomic_load(&calls)) sched_yield();
	}
	int result = 0;
	for (unsigned round = 0; round < rounds; round++) {
		uint32_t magic = 0x9e3779b9;
		char path[64];
		for (size_t i = 0; i < length; i++)
			expected[i] = (unsigned char)(i * 37 + round * 19);
		memcpy(expected, &magic, sizeof(magic));
		int write_fd = memfd_create("snapshot", MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);
		if (write_fd < 0) { perror("memfd_create"); result = 2; break; }
		snprintf(path, sizeof(path), "/proc/self/fd/%d", write_fd);
		int read_fd = open(path, O_RDONLY | O_CLOEXEC);
		if (read_fd < 0 || ftruncate(write_fd, length)) {
			perror("prepare snapshot"); result = 2; close(write_fd);
			if (read_fd >= 0) close(read_fd);
			break;
		}
		unsigned char *write_map = mmap(NULL, length, PROT_READ | PROT_WRITE,
			MAP_SHARED, write_fd, 0);
		if (write_map == MAP_FAILED) { perror("mmap write"); result = 2; }
		else {
			memcpy(write_map, expected, length);
			if (munmap(write_map, length)) { perror("munmap write"); result = 2; }
		}
		if (!result && fcntl(write_fd, F_ADD_SEALS,
			F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_FUTURE_WRITE)) {
			perror("seal snapshot"); result = 2;
		}
		if (close(write_fd)) { perror("close write"); result = 2; }
		if (!result) {
			const unsigned char *read_map = mmap(NULL, length, PROT_READ, MAP_SHARED, read_fd, 0);
			if (read_map == MAP_FAILED) { perror("mmap read"); result = 2; }
			else {
				for (size_t i = 0; i < length; i++) {
					if (read_map[i] != expected[i]) {
						fprintf(stderr, "FAIL: snapshot round=%u byte=%zu got=%02x expected=%02x calls=%lu\n",
							round, i, read_map[i], expected[i], atomic_load(&calls));
						result = 1; break;
					}
				}
				if (munmap((void *)read_map, length)) { perror("munmap read"); result = 2; }
			}
		}
		if (close(read_fd)) { perror("close read"); result = 2; }
		if (result) break;
	}
	if (observers) {
		atomic_store(&stop, 1);
		for (unsigned i = 0; i < observers; i++)
			if (pthread_join(threads[i], NULL)) result = 2;
		if (atomic_load(&worker_error)) result = 2;
	}
	if (!result) printf("PASS: %u sealed shared-memory snapshots, sibling source calls=%lu\n",
		rounds, atomic_load(&calls));
	return result;
}
