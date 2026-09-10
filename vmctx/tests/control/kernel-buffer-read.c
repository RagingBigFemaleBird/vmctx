// SPDX-License-Identifier: GPL-2.0
/* A successful /dev/null write does not establish a kernel buffer read.
 * This control distinguishes it from a pipe using inaccessible user memory.
 * cc -O2 -Wall -Wextra -static kernel-buffer-read.c -o kernel-buffer-read
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
	void *page = mmap(NULL, 4096, PROT_NONE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int nullfd = open("/dev/null", O_WRONLY | O_CLOEXEC);
	int pipefd[2];
	if (page == MAP_FAILED || nullfd < 0 || pipe2(pipefd, O_CLOEXEC | O_NONBLOCK))
		return 2;

	errno = 0;
	ssize_t null_result = write(nullfd, page, 64);
	int null_error = errno;
	errno = 0;
	ssize_t pipe_result = write(pipefd[1], page, 64);
	int pipe_error = errno;
	char byte;
	errno = 0;
	ssize_t read_result = read(pipefd[0], &byte, 1);
	int read_error = errno;

	int pass = null_result == 64 && pipe_result == -1 && pipe_error == EFAULT &&
		   read_result == -1 && read_error == EAGAIN;
	printf("%s PROT_NONE buffer: null=%zd/%d pipe=%zd/%d pipe_read=%zd/%d\n",
	       pass ? "PASS" : "FAIL", null_result, null_error,
	       pipe_result, pipe_error, read_result, read_error);
	close(nullfd);
	close(pipefd[0]);
	close(pipefd[1]);
	munmap(page, 4096);
	return pass ? 0 : 1;
}
