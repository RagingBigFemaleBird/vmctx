/* SPDX-License-Identifier: GPL-2.0 */
/* Bounded native memory inspection. The caller holds its saved execution
 * binding's read guard through these complete operations. No proc descriptor
 * escapes: pagemap returns one word and maps returns sealed, detached bytes.
 * A retained task identity alone does not fence OBJECT replacement, which
 * changes the execution image without changing the task's native MM.
 */
#ifndef VMR_LINUX_EXECUTION_PROC_H
#define VMR_LINUX_EXECUTION_PROC_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include "linux-execution-context.h"

static inline int linux_execution_pagemap_read(
		const struct linux_execution_context *native,uint64_t address,
		uint64_t *word)
{
	int fd=linux_execution_proc_open(native,native->pid,"pagemap");
	if(fd<0)return -1;
	uint64_t value;
	ssize_t got;
	do {got=pread(fd,&value,sizeof(value),(off_t)((address>>12)*8));}
	while(got<0 && errno==EINTR);
	int saved=got<0 ? errno : got!=(ssize_t)sizeof(value) ? EIO : 0;
	close(fd);
	if(saved) {errno=saved;return -1;}
	*word=value;return 0;
}

static inline int linux_execution_maps_snapshot(
		const struct linux_execution_context *native)
{
	int proc=linux_execution_proc_open(native,native->pid,"maps");
	if(proc<0)return -1;
	int snapshot=linux_execution_private_fd(
		memfd_create("vmctx-maps-snapshot",MFD_CLOEXEC|MFD_ALLOW_SEALING));
	int saved=errno;
	if(snapshot<0) {close(proc);errno=saved;return -1;}
	char bytes[4096];
	for(;;) {
		ssize_t got;
		do {got=read(proc,bytes,sizeof(bytes));}while(got<0 && errno==EINTR);
		if(got<0)goto failed;
		if(!got)break;
		for(ssize_t at=0;at<got;) {
			ssize_t put=write(snapshot,bytes+at,(size_t)(got-at));
			if(put<0 && errno==EINTR)continue;
			if(put<=0) {if(!put)errno=EIO;goto failed;}
			at+=put;
		}
	}
	close(proc);proc=-1;
	if(lseek(snapshot,0,SEEK_SET)<0 ||
	   fcntl(snapshot,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)<0)
		goto failed;
	return snapshot;
 failed:
	saved=errno;
	if(proc>=0)close(proc);
	close(snapshot);errno=saved;return -1;
}
#endif
