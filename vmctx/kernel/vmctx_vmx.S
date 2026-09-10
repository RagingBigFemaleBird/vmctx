/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <asm/nospec-branch.h>
#include <asm/unwind_hints.h>
#include "vmctx_vmx_offsets.h"

/* A separate function keeps the compiler's frame out of the guest register
 * exchange. No C code runs until every host callee-saved register is restored.
 * HOST_RSP points at the context pointer on this function's host stack. */
.section .noinstr.text, "ax"
SYM_FUNC_START(vmctx_vmx_enter)
	push %rbp
	mov %rsp, %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %rbx
	push %rdi
	mov %rdi, %rax
	mov $VMCTX_VMX_HOST_RSP, %edx
	vmwrite %rsp, %rdx
	lea .Lvmctx_exit(%rip), %rcx
	mov $VMCTX_VMX_HOST_RIP, %edx
	vmwrite %rcx, %rdx
	mov VMCTX_VMX_RBX(%rax), %rbx
	mov VMCTX_VMX_RCX(%rax), %rcx
	mov VMCTX_VMX_RDX(%rax), %rdx
	mov VMCTX_VMX_RSI(%rax), %rsi
	mov VMCTX_VMX_RDI(%rax), %rdi
	mov VMCTX_VMX_RBP(%rax), %rbp
	mov VMCTX_VMX_R8(%rax), %r8
	mov VMCTX_VMX_R9(%rax), %r9
	mov VMCTX_VMX_R10(%rax), %r10
	mov VMCTX_VMX_R11(%rax), %r11
	mov VMCTX_VMX_R12(%rax), %r12
	mov VMCTX_VMX_R13(%rax), %r13
	mov VMCTX_VMX_R14(%rax), %r14
	mov VMCTX_VMX_R15(%rax), %r15
	cmpb $0, VMCTX_VMX_LAUNCHED(%rax)
	mov VMCTX_VMX_RAX(%rax), %rax
	/* VM exit returns to HOST_RIP without a software branch. */
	UNWIND_HINT_SAVE
	jne .Lvmctx_resume
	vmlaunch
	jmp .Lvmctx_fail
.Lvmctx_resume:
	vmresume
.Lvmctx_fail:
	mov (%rsp), %rax
	movq $1, VMCTX_VMX_FAIL(%rax)
	jmp .Lvmctx_restore
.Lvmctx_exit:
	UNWIND_HINT_RESTORE
	ENDBR
	push %rax
	mov 8(%rsp), %rax
	mov %rbx, VMCTX_VMX_RBX(%rax)
	mov %rcx, VMCTX_VMX_RCX(%rax)
	mov %rdx, VMCTX_VMX_RDX(%rax)
	mov %rsi, VMCTX_VMX_RSI(%rax)
	mov %rdi, VMCTX_VMX_RDI(%rax)
	mov %rbp, VMCTX_VMX_RBP(%rax)
	mov %r8, VMCTX_VMX_R8(%rax)
	mov %r9, VMCTX_VMX_R9(%rax)
	mov %r10, VMCTX_VMX_R10(%rax)
	mov %r11, VMCTX_VMX_R11(%rax)
	mov %r12, VMCTX_VMX_R12(%rax)
	mov %r13, VMCTX_VMX_R13(%rax)
	mov %r14, VMCTX_VMX_R14(%rax)
	mov %r15, VMCTX_VMX_R15(%rax)
	pop VMCTX_VMX_RAX(%rax)
	movq $0, VMCTX_VMX_FAIL(%rax)
.Lvmctx_restore:
	/* A guest can poison the return predictor. Use the native VM-exit
	 * mitigation before returning into the C caller. */
	FILL_RETURN_BUFFER %rcx, RSB_CLEAR_LOOPS, X86_FEATURE_RSB_VMEXIT, X86_FEATURE_RSB_VMEXIT_LITE
	pop %rdi
	pop %rbx
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %rbp
	RET
SYM_FUNC_END(vmctx_vmx_enter)
.section .note.GNU-stack,"",@progbits
