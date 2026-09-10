// SPDX-License-Identifier: GPL-2.0
/* Nested fork snapshots must survive user and source-kernel writes to their
 * ancestors. The second child is born after the first snapshot has diverged. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGES 16
#define WORDS (PAGES * 4096 / sizeof(uint64_t))
static volatile uint64_t *memory;
static uint64_t expected(unsigned generation, size_t i)
{
	return UINT64_C(0x9e3779b97f4a7c15) * (generation + 1) ^ (i * 131 + (i >> 8));
}
static void transfer(int fd, void *p, size_t n, int output)
{
	while (n) {
		ssize_t r = output ? write(fd, p, n) : read(fd, p, n);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) {
			dprintf(2, "FAIL: nested transfer fd=%d output=%d remaining=%zu result=%zd errno=%d\n",
				fd, output, n, r, errno);
			_exit(2);
		}
		p = (char *)p + r; n -= r;
	}
}
static int verify(unsigned generation)
{
	for (size_t i = 0; i < WORDS; i++)
		if (memory[i] != expected(generation, i)) {
			dprintf(2, "FAIL: nested snapshot generation=%u word=%zu got=%016llx want=%016llx\n",
				generation, i, (unsigned long long)memory[i],
				(unsigned long long)expected(generation, i));
			return 1;
		}
	return 0;
}
static void replace(unsigned generation)
{
	int p[2]; uint64_t page[512];
	if (pipe(p)) _exit(2);
	for (size_t j = 0; j < PAGES; j++) {
		for (size_t i = 0; i < 512; i++) page[i] = expected(generation, j * 512 + i);
		if (j & 1) {
			/* One page fits into an empty pipe without a helper. */
			transfer(p[1], page, sizeof(page), 1);
			transfer(p[0], (void *)(memory + j * 512), sizeof(page), 0);
		} else {
			for (size_t i = 0; i < 512; i++) memory[j * 512 + i] = page[i];
		}
	}
	close(p[0]); close(p[1]);
}
static int branch(unsigned generation, unsigned depth)
{
	if (verify(generation)) return 1;
	if (!depth) return 0;
	pid_t children[2]; int gates[2][2];
	for (unsigned k = 0; k < 2; k++) {
		if (pipe(gates[k])) return 2;
		children[k] = fork();
		if (children[k] < 0) return 2;
		if (!children[k]) {
			close(gates[k][1]);
			char gate; transfer(gates[k][0], &gate, 1, 0); close(gates[k][0]);
			_exit(branch(generation + k, depth - 1));
		}
		close(gates[k][0]);
		replace(generation + k + 1);
	}
	/* Both children now outlive two writes in their parent. Their private
	 * snapshots differ, so a current ancestor page cannot satisfy both. */
	int bad = verify(generation + 2);
	for (unsigned k = 0; k < 2; k++) {
		char gate = 1; transfer(gates[k][1], &gate, 1, 1); close(gates[k][1]);
	}
	for (unsigned k = 0; k < 2; k++) {
		int status = 0; pid_t p;
		do { p = waitpid(children[k], &status, 0); } while (p < 0 && errno == EINTR);
		if (p != children[k] || !WIFEXITED(status) || WEXITSTATUS(status)) {
			dprintf(2, "FAIL: nested child generation=%u depth=%u child=%u wait=%d status=%x errno=%d\n",
				generation, depth, k, p, status, errno);
			bad = 1;
		}
	}
	return bad || verify(generation + 2);
}
int main(void)
{
	alarm(45);
	memory = mmap(NULL, PAGES * 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED) return 2;
	replace(0);
	if (branch(0, 3)) return 1;
	munmap((void *)memory, PAGES * 4096);
	puts("PASS: nested fork snapshots preserve distinct generations through user and kernel writes");
	return 0;
}
