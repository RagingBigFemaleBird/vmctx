// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/vmctx_cpu_model.h"

int main(void)
{
	struct vmctx_cpu_model amd = { .leaf1_ecx = ~0U, .leaf1_edx = ~0U,
		.leaf7_ebx = ~0U, .leaf7_ecx = ~0U, .leaf7_edx = ~0U,
		.ext1_ecx = ~0U, .ext1_edx = ~0U, .xcr0 = ~0ULL };
	vmctx_cpu_model_trim(&amd);
	struct vmctx_cpu_model intel = amd, result;
	intel.leaf7_ebx &= ~VMCTX_CPU_BIT(29); /* SHA absent on the lab Intel */
	vmctx_cpu_model_intersect(&result, &amd, &intel);
	assert(vmctx_cpu_model_valid(&result));
	assert(vmctx_cpu_model_subset(&result, &amd) && vmctx_cpu_model_subset(&result, &intel));
	assert(!(result.leaf7_ebx & VMCTX_CPU_BIT(29)));
	assert(result.xcr0 == 7);
	intel.xcr0 = 3; /* CPU may implement AVX; its OS adapter cannot save YMM. */
	vmctx_cpu_model_intersect(&result, &amd, &intel);
	assert(result.xcr0 == 3 && !(result.leaf1_ecx & VMCTX_CPU_AVX_DEP));
	assert(!(result.leaf7_ebx & VMCTX_CPU_BIT(5)));
	intel.leaf1_ecx &= ~VMCTX_CPU_BIT(27);
	vmctx_cpu_model_intersect(&result, &amd, &intel);
	assert(!(result.leaf1_ecx & VMCTX_CPU_XSAVE));
	__u32 a, b, c, d;
	vmctx_cpu_model_cpuid(&result, 0xd, 0, &a, &b, &c, &d);
	assert(!(a | b | c | d));
	result = amd; result.reserved2 = 1;
	assert(!vmctx_cpu_model_valid(&result));
	result = amd; result.xcr0 = 1;
	assert(!vmctx_cpu_model_valid(&result));
	result = amd; result.leaf7_ebx |= VMCTX_CPU_BIT(16);
	assert(!vmctx_cpu_model_valid(&result)); /* never expose AVX-512 */
	vmctx_cpu_model_cpuid(&amd, 0xd, 0, &a, &b, &c, &d);
	assert(a == 7 && b == 832 && c == 832 && d == 0);
	vmctx_cpu_model_cpuid(&amd, 0xd, 2, &a, &b, &c, &d);
	assert(a == 256 && b == 576 && !c && !d);
	const unsigned hidden[][2] = {{4,0},{7,1},{0xd,1},{0xd,9},{0xb,0},{0x15,0},
		{0x16,0},{0x1f,0},{0x40000000,0},{0x80000008,0},{0x8000000a,0},{~0U,~0U}};
	for (unsigned i = 0; i < sizeof(hidden)/sizeof(hidden[0]); i++) {
		vmctx_cpu_model_cpuid(&amd, hidden[i][0], hidden[i][1], &a, &b, &c, &d);
		assert(!(a | b | c | d));
	}
	vmctx_cpu_model_cpuid(&amd, 0, 0, &a, &b, &c, &d);
	char vendor[13];
	memcpy(vendor, &b, 4); memcpy(vendor+4, &d, 4); memcpy(vendor+8, &c, 4); vendor[12] = 0;
	assert(!strcmp(vendor, "GenuineIntel"));
	puts("PASS: CPU intersection, state dependencies, malformed models, and hidden host leaves");
}
