/* SPDX-License-Identifier: GPL-2.0 */
/* Linux adapter for execution_binding_replace. control must address one
 * retained execution handle. Memory and CPU controls use the binding guard;
 * neither a numeric PID nor a new current binding can substitute for it. */
#ifndef VMR_LINUX_EXECUTION_OBJECT_H
#define VMR_LINUX_EXECUTION_OBJECT_H
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "vmctx_object.h"

struct linux_execution_object {
	long (*control)(void *,unsigned,void *);
	void *opaque;
};

static inline int linux_execution_object_identity(int fd,uint64_t *device,uint64_t *inode)
{
	struct stat object;
	if(fstat(fd,&object))return -1;
	uint64_t maj=major(object.st_dev),min=minor(object.st_dev);
	if(maj>0xfff || min>0xfffff) {errno=EOVERFLOW;return -1;}
	*device=(min&0xff)|(maj<<8)|((min&~UINT64_C(0xff))<<12);
	*inode=object.st_ino;return 0;
}

/* The monitor already owns this retained context; verify its private object
 * before publishing membership. A source MM record may never be backed by a
 * different native object, even when the descriptors have similar names. */
static inline int linux_execution_object_verify(struct linux_execution_object *native,
		int fd,uint64_t *epoch)
{
	uint64_t device,inode;
	if(linux_execution_object_identity(fd,&device,&inode))return -1;
	struct vmctx_object q={.version=VMCTX_OBJECT_ABI,.size=sizeof(q),
		.op=VMCTX_OBJECT_INFO,.backing_fd=-1};
	if(native->control(native->opaque,VMCTX_CTL_OBJECT,&q))return -1;
	unsigned required=VMCTX_OBJECT_RETAINED|VMCTX_OBJECT_REPLACE_MM;
	if(q.version!=VMCTX_OBJECT_ABI || q.size!=sizeof(q) || q.op!=VMCTX_OBJECT_INFO ||
	   q.expected_epoch || q.reserved || !q.epoch || q.backing_fd!=-1 ||
	   q.device!=device || q.inode!=inode || (q.features&required)!=required) {
		errno=EPROTO;return -1;
	}
	*epoch=q.epoch;return 0;
}

/* -1 proves ordinary refusal (or terminal context); -2 means the native
 * commit is unknown and the binding owner must end execution. A copyout fault
 * can follow a successful commit. Keep the exclusive guard while replaying
 * exactly that epoch and object; never return an apparently unchanged binding
 * to other callers merely because its result could not be copied out. */
static inline int linux_execution_object_replace(void *opaque,int fd,
		uint64_t expected,uint64_t *committed)
{
	struct linux_execution_object *native=opaque;
	uint64_t device,inode;
	if(linux_execution_object_identity(fd,&device,&inode))return -1;
	if(!expected || expected==UINT64_MAX) {
		errno=EOVERFLOW;return -1;
	}
	int uncertain=0;
	for(unsigned attempt=0;attempt<64;attempt++) {
		struct vmctx_object q={.version=VMCTX_OBJECT_ABI,.size=sizeof(q),
			.op=VMCTX_OBJECT_REPLACE,.backing_fd=fd,
			.expected_epoch=expected,.epoch=expected+1};
		if(!native->control(native->opaque,VMCTX_CTL_OBJECT,&q)) {
			if(q.version!=VMCTX_OBJECT_ABI || q.size!=sizeof(q) ||
			   q.op!=VMCTX_OBJECT_REPLACE || q.expected_epoch || q.reserved ||
			   q.epoch!=expected+1 || q.backing_fd!=-1 || q.device!=device ||
			   q.inode!=inode ||
			   (q.features&(VMCTX_OBJECT_RETAINED|VMCTX_OBJECT_REPLACE_MM))!=
			   (VMCTX_OBJECT_RETAINED|VMCTX_OBJECT_REPLACE_MM)) {
				errno=EPROTO;return -2;
			}
			*committed=q.epoch;return 0;
		}
		if(errno==ESRCH)return -1;
		if(errno==EFAULT)uncertain=1;
		else if(!uncertain)return -1;
		else if(errno!=EBUSY && errno!=EAGAIN && errno!=EINTR)return -2;
		usleep(1000);
	}
	errno=EIO;return -2;
}
#endif
