// SPDX-License-Identifier: GPL-2.0
/*
 * homeapp — an application that is installed ONLY on the home machine.
 *
 * It is never copied to the remote machine. vmremote fetches this binary over
 * the network, maps it into a VM context on the remote machine and runs it
 * there; its syscalls come back here to be executed. So:
 *
 *   - the code and its memory live on the remote machine's CPU/RAM
 *   - every file it opens is this machine's file
 *   - everything it prints appears on this machine's terminal
 *
 * It reports both halves of that split: identity syscalls it makes that are
 * *not* forwarded (getpid) come from the remote kernel, while everything it
 * reads comes from home. It also does real work (a checksum loop) so there is
 * CPU time actually being spent on the remote machine.
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/syscall.h>

static void outs(const char *s, size_t n)
{
	syscall(SYS_write, 1, s, n);
}

static void out(const char *s)
{
	outs(s, strlen(s));
}

static void outnum(unsigned long v)
{
	char b[24];
	int i = 0, j;

	if (!v) {
		out("0");
		return;
	}
	while (v && i < 23) {
		b[i++] = '0' + (v % 10);
		v /= 10;
	}
	for (j = i - 1; j >= 0; j--)
		outs(&b[j], 1);
}

/* Print a file, and return how many bytes it had. */
static long show_file(const char *path, int max_lines)
{
	char buf[4096];
	long fd, total = 0, n;
	int lines = 0;

	fd = syscall(SYS_open, path, O_RDONLY, 0);
	if (fd < 0) {
		out("    <could not open ");
		out(path);
		out(">\n");
		return -1;
	}
	while ((n = syscall(SYS_read, fd, buf, sizeof(buf))) > 0) {
		long i, start = 0;

		total += n;
		for (i = 0; i < n && lines < max_lines; i++) {
			if (buf[i] == '\n') {
				outs("    ", 4);
				outs(buf + start, i - start + 1);
				start = i + 1;
				lines++;
			}
		}
		if (lines >= max_lines)
			break;
	}
	syscall(SYS_close, fd);
	return total;
}

int main(void)
{
	unsigned long sum = 0;
	long pid;

	out("========================================================\n");
	out(" homeapp: an application installed only on the home box\n");
	out("========================================================\n");

	pid = syscall(SYS_getpid);
	out("  running as pid ");
	outnum((unsigned long)pid);
	out(" (this pid belongs to the machine executing me)\n\n");

	out("  /etc/hostname as I see it:\n");
	show_file("/etc/hostname", 4);

	out("\n  /etc/os-release as I see it (first 4 lines):\n");
	show_file("/etc/os-release", 4);

	out("\n  doing real work on the CPU that hosts me...\n");
	for (unsigned long i = 1; i <= 40000000UL; i++)
		sum += i * 2654435761UL;
	out("  checksum = ");
	outnum(sum & 0xffffffffUL);
	out("\n\n  homeapp done.\n");
	return 0;
}
