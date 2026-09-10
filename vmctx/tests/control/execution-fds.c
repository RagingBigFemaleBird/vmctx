/* SPDX-License-Identifier: GPL-2.0 */
/* Exercise the real post-fork descriptor helpers in private child tables. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include "../../user/linux-execution-fds.h"

static void ranges(int private, int shared)
{
	const int probes[] = {3,4,5,7,100,1023,1024,4096,8192,8193};
	if (syscall(SYS_close_range,3,UINT_MAX,0)) _exit(10);
	int original=linux_execution_object_fd("descriptor-control");
	if (original!=3 || !(fcntl(original,F_GETFD)&FD_CLOEXEC)) _exit(11);
	for (unsigned i=0;i<sizeof(probes)/sizeof(*probes);i++)
		if (probes[i]!=original && dup2(original,probes[i])!=probes[i]) _exit(12);
	int standard[3];
	for (int i=0;i<3;i++) standard[i]=fcntl(i,F_GETFD);
	if (linux_execution_close_fds(private,shared)) _exit(13);
	for (unsigned i=0;i<sizeof(probes)/sizeof(*probes);i++) {
		int fd=probes[i],present=fcntl(fd,F_GETFD)>=0;
		if (present!=(fd==private || fd==shared)) _exit(14);
	}
	for (int i=0;i<3;i++) if (fcntl(i,F_GETFD)!=standard[i]) _exit(15);
	_exit(0);
}

static void objects(unsigned closed)
{
	for (int i=0;i<3;i++) if (closed&(1U<<i)) close(i);
	int fd=linux_execution_object_fd("object-control");
	if (fd<3 || !(fcntl(fd,F_GETFD)&FD_CLOEXEC)) _exit(20);
	char byte=42,result=0;
	if (pwrite(fd,&byte,1,0)!=1 || pread(fd,&result,1,0)!=1 || result!=byte) _exit(21);
	for (int i=0;i<3;i++)
		if ((closed&(1U<<i)) && (fcntl(i,F_GETFD)!=-1 || errno!=EBADF)) _exit(22);
	_exit(0);
}

int main(void)
{
	const int pairs[][2]={{3,4},{4,3},{4096,8192},{8192,4096},
		{4,4},{-1,-1},{2,4096},{3,-1},{-1,8192},{8192,8192}};
	unsigned ranges_count=sizeof(pairs)/sizeof(*pairs);
	for (unsigned i=0;i<ranges_count+8;i++) {
		pid_t child=fork();
		if (child<0) return 1;
		if (!child) {
			if(i<ranges_count) ranges(pairs[i][0],pairs[i][1]);
			else objects(i-ranges_count);
		}
		int status;
		if (waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)) {
			fprintf(stderr,"FAIL execution descriptors case %u\n",i);
			return 1;
		}
	}
	puts("PASS: execution children preserve exact object descriptors across range order, duplicates, high FDs and all standard-descriptor closures");
	return 0;
}
