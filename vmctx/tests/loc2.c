#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/syscall.h>

/*
 * Does a locally-executed syscall that reads guest memory zero the page it
 * touches?
 *
 * The two probes are 256 KiB apart so that neither can be dragged in by the
 * other's 64 KiB fault chunk — that is what made the first two attempts at
 * this test pass for the wrong reason.
 *
 *   TEST    : only the kernel ever touches it, via a raw syscall
 *   CONTROL : read normally by the guest, so it must arrive from home
 */
#define MB (1024 * 1024)
#define TEST_OFF   (256 * 1024)
#define CTRL_OFF   (768 * 1024)

static const unsigned char blob[MB] __attribute__((aligned(4096))) = {
	[TEST_OFF + 0] = 0xA1, [TEST_OFF + 1] = 0xB2, [TEST_OFF + 2] = 0xC3,
	[TEST_OFF + 3] = 0xD4, [TEST_OFF + 4] = 0xE5, [TEST_OFF + 5] = 0xF6,
	[TEST_OFF + 6] = 0x17, [TEST_OFF + 7] = 0x28,
	[CTRL_OFF + 0] = 0x5A, [CTRL_OFF + 1] = 0x6B, [CTRL_OFF + 2] = 0x7C,
	[CTRL_OFF + 3] = 0x8D, [CTRL_OFF + 4] = 0x9E, [CTRL_OFF + 5] = 0xAF,
	[CTRL_OFF + 6] = 0xB0, [CTRL_OFF + 7] = 0xC1,
};

static void show(const char *tag, const unsigned char *p)
{
	char b[128];
	int i, n = snprintf(b, sizeof b, "%s:", tag);

	for (i = 0; i < 8; i++)
		n += snprintf(b + n, sizeof b - n, " %02x", p[i]);
	n += snprintf(b + n, sizeof b - n, "\n");
	if (write(1, b, (size_t)n) < 0)
		return;
}

int main(void)
{
	char b[96];
	long r;
	int n;

	/* Kernel reads it first; nothing in user space has touched it. */
	r = syscall(SYS_rt_sigaction, SIGUSR1, blob + TEST_OFF, NULL, 8);
	n = snprintf(b, sizeof b, "raw rt_sigaction -> %ld (errno %d)\n",
		     r, r < 0 ? errno : 0);
	if (write(1, b, (size_t)n) < 0)
		return 1;

	show("TEST    (kernel touched it first)", blob + TEST_OFF);
	show("CONTROL (guest reads it normally)", blob + CTRL_OFF);
	return 0;
}
