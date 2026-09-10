// SPDX-License-Identifier: GPL-2.0
/*
 * vmselftest — Milestone 1b step 0: drive VMCTX_IOC_SELFTEST, which enters
 * SVM guest mode with a tiny sandboxed guest and reports the #VMEXIT.
 */
#include "common.h"
#include <errno.h>

int main(void)
{
	struct vmctx_selftest st;
	int fd, rc;

	fd = vmctx_open();
	if (fd < 0)
		return 1;

	memset(&st, 0, sizeof(st));
	rc = ioctl(fd, VMCTX_IOC_SELFTEST, &st);
	close(fd);
	if (rc < 0) {
		fprintf(stderr, "VMCTX_IOC_SELFTEST: %s\n", strerror(errno));
		return 1;
	}

	printf("vmctx SVM guest-entry selftest\n");
	printf("  virt tech   : %s\n", vendor_name(st.vendor));
	/*
	 * This test is the SVM 16-bit sandbox: its own VMCB, its own NPT window,
	 * a payload that ends in VMMCALL. There is no VMX equivalent because the
	 * VMX backend implements only the mode that matters — the process's own
	 * user code — so on Intel the honest answer is "not this test", not
	 * "FAIL". Reporting a failure here sends someone looking for a bug in a
	 * backend that is working.
	 */
	if ((int)st.result == -524 /* ENOTSUPP, kernel-internal */ ||
	    (int)st.result == -EOPNOTSUPP) {
		printf("\n  RESULT: n/a — this selftest is SVM-only. On Intel the\n");
		printf("  VMX backend runs a context's code instead, so use dumpvm\n");
		printf("  to confirm a backend is there and run a program the only\n");
		printf("  way there is: vmremote against a source.\n");
		return 0;
	}
	printf("  VMCB pa     : 0x%016llx\n", (unsigned long long)st.vmcb_pa);
	printf("  NPT root pa : 0x%016llx\n", (unsigned long long)st.npt_pml4_pa);
	printf("  exit_code   : 0x%03x %s\n", st.exit_code,
	       st.exit_code == 0x081 ? "(VMMCALL)" :
	       st.exit_code == 0x400 ? "(NPF - nested page fault)" :
	       st.exit_code == 0x07f ? "(SHUTDOWN)" :
	       st.exit_code == 0xffffffff ? "(INVALID guest state)" : "");
	printf("  exit_info_1 : 0x%016llx\n", (unsigned long long)st.exit_info_1);
	printf("  exit_info_2 : 0x%016llx\n", (unsigned long long)st.exit_info_2);
	printf("  guest RAX   : 0x%016llx  (expect low16 0x1234)\n",
	       (unsigned long long)st.guest_rax);
	printf("  guest RIP   : 0x%016llx\n", (unsigned long long)st.guest_rip);
	printf("  mem[0x100]  : 0x%04llx  (expect 0x1234 — guest wrote via NPT)\n",
	       (unsigned long long)st.mem_marker);
	printf("\n");
	if (st.result == 0)
		printf("  RESULT: PASS — guest executed and exited via VMMCALL\n");
	else
		printf("  RESULT: FAIL (result=%d) — see fields above / dmesg\n",
		       (int)st.result);
	return st.result == 0 ? 0 : 2;
}
