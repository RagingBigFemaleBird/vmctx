/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <asm/nospec-branch.h>
#include "vmctx_svm_offsets.h"

/* RAX/RSP/RIP are exchanged by VMRUN itself; all other guest registers are
 * exchanged here. Restore the host frame before STGI can deliver an NMI. */
.section .noinstr.text, "ax"
SYM_FUNC_START(vmctx_svm_enter)
	push %rbp
	mov %rsp, %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %rbx
	push %rdi
	mov VMCTX_SVM_HOST_VMCB_PA(%rdi), %rax
	vmsave %rax
	clgi
	mov (%rsp), %rax
	mov VMCTX_SVM_RBX(%rax), %rbx
	mov VMCTX_SVM_RCX(%rax), %rcx
	mov VMCTX_SVM_RDX(%rax), %rdx
	mov VMCTX_SVM_RSI(%rax), %rsi
	mov VMCTX_SVM_RDI(%rax), %rdi
	mov VMCTX_SVM_RBP(%rax), %rbp
	mov VMCTX_SVM_R8(%rax), %r8
	mov VMCTX_SVM_R9(%rax), %r9
	mov VMCTX_SVM_R10(%rax), %r10
	mov VMCTX_SVM_R11(%rax), %r11
	mov VMCTX_SVM_R12(%rax), %r12
	mov VMCTX_SVM_R13(%rax), %r13
	mov VMCTX_SVM_R14(%rax), %r14
	mov VMCTX_SVM_R15(%rax), %r15
	mov VMCTX_SVM_VMCB_PA(%rax), %rax
	vmload %rax
	vmrun %rax
	vmsave %rax
	mov (%rsp), %rax
	mov %rbx, VMCTX_SVM_RBX(%rax)
	mov %rcx, VMCTX_SVM_RCX(%rax)
	mov %rdx, VMCTX_SVM_RDX(%rax)
	mov %rsi, VMCTX_SVM_RSI(%rax)
	mov %rdi, VMCTX_SVM_RDI(%rax)
	mov %rbp, VMCTX_SVM_RBP(%rax)
	mov %r8, VMCTX_SVM_R8(%rax)
	mov %r9, VMCTX_SVM_R9(%rax)
	mov %r10, VMCTX_SVM_R10(%rax)
	mov %r11, VMCTX_SVM_R11(%rax)
	mov %r12, VMCTX_SVM_R12(%rax)
	mov %r13, VMCTX_SVM_R13(%rax)
	mov %r14, VMCTX_SVM_R14(%rax)
	mov %r15, VMCTX_SVM_R15(%rax)
	mov VMCTX_SVM_HOST_VMCB_PA(%rax), %rax
	vmload %rax
	FILL_RETURN_BUFFER %rax, RSB_CLEAR_LOOPS, X86_FEATURE_RSB_VMEXIT
	pop %rdi
	pop %rbx
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %rbp
	stgi
	RET
SYM_FUNC_END(vmctx_svm_enter)
.section .note.GNU-stack,"",@progbits
