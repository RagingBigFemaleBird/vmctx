/* SPDX-License-Identifier: GPL-2.0 */
/* Native execution storage binding; contains no source OS semantics. */
#ifndef VMCTX_OBJECT_H
#define VMCTX_OBJECT_H
#include <linux/types.h>
#define VMCTX_CTL_OBJECT 47
#define VMCTX_OBJECT_ABI 1
#define VMCTX_OBJECT_CAPS 0
#define VMCTX_OBJECT_INFO 1
#define VMCTX_OBJECT_REPLACE 2
#define VMCTX_OBJECT_RETAINED (1U << 0)
#define VMCTX_OBJECT_REPLACE_MM (1U << 1)
struct vmctx_object {
	__u32 version, size, op, features;
	__u64 expected_epoch, epoch;
	__s32 backing_fd;
	__u32 reserved;
	__u64 device, inode;
};
/* REPLACE is valid only at a monitor-owned execution stop. It removes this
 * native MM's old private/shared object mappings and installs a new retained
 * private object. It never punches the old object, which may have other users.
 * The native MM must have no other task or outstanding MM lease/reference;
 * EBUSY leaves it unchanged. The old private object must exist. The caller
 * must exclude old-epoch page operations before replacing a binding: this
 * operation serializes individual native memory controls, not a distributed
 * transfer spanning several controls. epoch must be expected_epoch+1.
 * Retrying the same epoch and file after bad
 * result copyout is idempotent and cannot remove subsequently installed maps.
 * INFO and CAPS require zero input fields except version/size/op/backing_fd=-1.
 * Output backing_fd is always -1; device/inode identify the retained file. */
#endif
