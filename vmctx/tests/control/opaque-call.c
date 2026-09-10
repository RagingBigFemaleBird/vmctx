// SPDX-License-Identifier: GPL-2.0
/* Guest call identifiers must never alias the transport's operation codes. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
	long pid = syscall(SYS_getpid);
	const long calls[] = {0xf02, 0x1000, 0x1001, 0x1002, 0x1003,
		0x1004, 0x1005, 0x1006, 0x1007, 0x1008, 0x1009, 0x100a,
		0x100b, 0x100c, 0x100d, 0x100e, 0x100f, 0x1010, 0xffffffffL, -1L};
	for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
		long nr = calls[i];
		errno = 0;
		long ret = syscall(nr, 0, 0, 0, 0, 0, 0);
		if (ret != -1 || errno != ENOSYS) {
			printf("FAIL: opaque call %lx returned %ld errno=%d\n", nr, ret, errno);
			return 1;
		}
	}
	if (syscall(SYS_getpid) != pid)
		return 1;
	puts("PASS: guest call identifiers cannot invoke transport operations");
	return 0;
}
