// SPDX-License-Identifier: GPL-2.0
/*
 * dumpvm — inspect vmctx capabilities and dump a context's full state.
 *
 *   dumpvm            show CPU virtualization capabilities (global)
 *   dumpvm <id>       dump the state of context <id> for debugging
 */
#include "common.h"

static int dump_info(int fd)
{
	struct vmctx_info info = { 0 };

	if (ioctl(fd, VMCTX_IOC_INFO, &info) < 0) {
		fprintf(stderr, "VMCTX_IOC_INFO: %s\n", strerror(errno));
		return 1;
	}

	printf("vmctx capabilities\n");
	printf("  abi         : %u\n", info.abi_version);
	printf("  cpu vendor  : %s\n", info.vendor_str);
	printf("  virt tech   : %s\n", vendor_name(info.vendor));
	print_caps(info.caps);
	printf("  contexts    : %u live\n", info.nr_contexts);
	printf("  IA32_EFER   : 0x%016llx\n", (unsigned long long)info.efer);
	if (info.vendor == VMCTX_VENDOR_INTEL) {
		printf("  VMX_BASIC   : 0x%016llx (rev id %u)\n",
		       (unsigned long long)info.vmx_basic,
		       (unsigned)(info.vmx_basic & 0x7fffffff));
		printf("  IA32_FEAT_CTL: 0x%016llx%s%s\n",
		       (unsigned long long)info.feat_ctl,
		       (info.feat_ctl & 1) ? " locked" : " UNLOCKED",
		       (info.feat_ctl & 4) ? " vmxon-outside-smx" : "");
	} else if (info.vendor == VMCTX_VENDOR_AMD) {
		printf("  VM_CR       : 0x%016llx%s\n",
		       (unsigned long long)info.vm_cr,
		       (info.vm_cr & (1u << 4)) ? " SVMDIS(disabled-in-BIOS)" : "");
	}
	if (!(info.caps & VMCTX_CAP_PRESENT))
		printf("  -> no hardware virtualization on this CPU\n");
	else if (!(info.caps & VMCTX_CAP_ENABLED))
		printf("  -> present but disabled in firmware/BIOS\n");
	else if (!(info.caps & VMCTX_CAP_BACKEND))
		/*
		 * The CPU having the feature is not the same as this driver
		 * being able to use it. Claiming otherwise sends whoever reads
		 * this off to debug a run that answers ENODEV for a reason
		 * nothing has told them.
		 */
		printf("  -> detected, but no backend for this CPU is "
		       "implemented: vmremote will fail with ENODEV\n");
	else
		printf("  -> ready: vmremote can stand up contexts on this CPU\n");
	return 0;
}

static int dump_ctx(int fd, unsigned id)
{
	struct vmctx_dump d = { 0 };
	d.id = id;

	if (ioctl(fd, VMCTX_IOC_DUMP, &d) < 0) {
		fprintf(stderr, "VMCTX_IOC_DUMP id=%u: %s\n", id, strerror(errno));
		return 1;
	}

	printf("context %u\n", d.id);
	printf("  owner pid   : %d\n", d.owner_pid);
	printf("  state       : %s\n", state_name(d.state));
	printf("  virt tech   : %s\n", vendor_name(d.vendor));
	print_caps(d.flags);
	printf("  VMCS/VMCB pa: 0x%016llx\n", (unsigned long long)d.vmcs_pa);
	printf("  VMCS rev id : 0x%llx\n", (unsigned long long)d.vmcs_rev);
	printf("  host CR0    : 0x%016llx\n", (unsigned long long)d.host_cr0);
	printf("  host CR3    : 0x%016llx\n", (unsigned long long)d.host_cr3);
	printf("  host CR4    : 0x%016llx%s\n", (unsigned long long)d.host_cr4,
	       (d.host_cr4 & (1ull << 13)) ? " VMXE" : "");
	printf("  host EFER   : 0x%016llx\n", (unsigned long long)d.host_efer);
	if (d.last_exit_reason == ~0ull)
		printf("  last VMEXIT : none (guest entry is Milestone 1b)\n");
	else
		printf("  last VMEXIT : reason=0x%llx qual=0x%llx\n",
		       (unsigned long long)d.last_exit_reason,
		       (unsigned long long)d.last_exit_qual);
	return 0;
}

int main(int argc, char **argv)
{
	int fd, ret;

	fd = vmctx_open();
	if (fd < 0)
		return 1;

	if (argc < 2)
		ret = dump_info(fd);
	else
		ret = dump_ctx(fd, (unsigned)strtoul(argv[1], NULL, 0));

	close(fd);
	return ret;
}
