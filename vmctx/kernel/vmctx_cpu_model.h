/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef VMCTX_CPU_MODEL_H
#define VMCTX_CPU_MODEL_H
#include <linux/types.h>

#define VMCTX_CTL_CPU_CAPS 33
#define VMCTX_CTL_CPU_MODEL 34
#define VMCTX_CPU_MODEL_VERSION 1

/* Architecture data only. The source and execution adapters independently
 * validate this immutable contract before the first guest instruction. */
struct vmctx_cpu_model {
	__u32 version, reserved;
	__u32 leaf1_ecx, leaf1_edx;
	__u32 leaf7_ebx, leaf7_ecx, leaf7_edx;
	__u32 ext1_ecx, ext1_edx, reserved2;
	__u64 xcr0;
};

#define VMCTX_CPU_BIT(n) (1U << (n))
/* Portable instruction features; no host topology, PMU, virtualization,
 * protection keys, CET, TSX, AMX, AVX-512, vendor extensions or frequency data.
 * State-bearing extensions beyond YMM need a separate negotiated model. */
#define VMCTX_CPU_1C (VMCTX_CPU_BIT(0) | VMCTX_CPU_BIT(1) | VMCTX_CPU_BIT(9) | \
	VMCTX_CPU_BIT(12) | VMCTX_CPU_BIT(13) | VMCTX_CPU_BIT(19) | VMCTX_CPU_BIT(20) | \
	VMCTX_CPU_BIT(22) | VMCTX_CPU_BIT(23) | VMCTX_CPU_BIT(25) | VMCTX_CPU_BIT(26) | \
	VMCTX_CPU_BIT(27) | VMCTX_CPU_BIT(28) | VMCTX_CPU_BIT(29) | VMCTX_CPU_BIT(30))
#define VMCTX_CPU_1D (VMCTX_CPU_BIT(0) | VMCTX_CPU_BIT(4) | VMCTX_CPU_BIT(8) | \
	VMCTX_CPU_BIT(15) | VMCTX_CPU_BIT(23) | VMCTX_CPU_BIT(24) | VMCTX_CPU_BIT(25) | VMCTX_CPU_BIT(26))
#define VMCTX_CPU_7B (VMCTX_CPU_BIT(3) | VMCTX_CPU_BIT(5) | VMCTX_CPU_BIT(8) | \
	VMCTX_CPU_BIT(9) | VMCTX_CPU_BIT(18) | VMCTX_CPU_BIT(19) | VMCTX_CPU_BIT(29))
#define VMCTX_CPU_EC (VMCTX_CPU_BIT(0) | VMCTX_CPU_BIT(5))
#define VMCTX_CPU_ED (VMCTX_CPU_BIT(11) | VMCTX_CPU_BIT(20) | VMCTX_CPU_BIT(29))
#define VMCTX_CPU_AVX_DEP (VMCTX_CPU_BIT(12) | VMCTX_CPU_BIT(28) | VMCTX_CPU_BIT(29))
#define VMCTX_CPU_XSAVE (VMCTX_CPU_BIT(26) | VMCTX_CPU_BIT(27))

static inline void vmctx_cpu_model_trim(struct vmctx_cpu_model *m)
{
	m->version = VMCTX_CPU_MODEL_VERSION;
	m->reserved = m->reserved2 = 0;
	m->leaf1_ecx &= VMCTX_CPU_1C;
	m->leaf1_edx &= VMCTX_CPU_1D;
	m->leaf7_ebx &= VMCTX_CPU_7B;
	m->leaf7_ecx = m->leaf7_edx = 0;
	m->ext1_ecx &= VMCTX_CPU_EC;
	m->ext1_edx &= VMCTX_CPU_ED;
	m->xcr0 &= 7;
	if ((m->leaf1_ecx & VMCTX_CPU_XSAVE) != VMCTX_CPU_XSAVE) {
		m->leaf1_ecx &= ~VMCTX_CPU_XSAVE;
		m->xcr0 &= 3;
	}
	if ((m->xcr0 & 7) != 7 || !(m->leaf1_ecx & VMCTX_CPU_BIT(28))) {
		m->xcr0 &= 3;
		m->leaf1_ecx &= ~VMCTX_CPU_AVX_DEP;
		m->leaf7_ebx &= ~VMCTX_CPU_BIT(5);
	}
}

static inline void vmctx_cpu_model_intersect(struct vmctx_cpu_model *out,
	const struct vmctx_cpu_model *a, const struct vmctx_cpu_model *b)
{
	*out = *a;
	out->leaf1_ecx &= b->leaf1_ecx;
	out->leaf1_edx &= b->leaf1_edx;
	out->leaf7_ebx &= b->leaf7_ebx;
	out->leaf7_ecx &= b->leaf7_ecx;
	out->leaf7_edx &= b->leaf7_edx;
	out->ext1_ecx &= b->ext1_ecx;
	out->ext1_edx &= b->ext1_edx;
	out->xcr0 &= b->xcr0;
	vmctx_cpu_model_trim(out);
}

static inline int vmctx_cpu_model_valid(const struct vmctx_cpu_model *m)
{
	struct vmctx_cpu_model c = *m;
	vmctx_cpu_model_trim(&c);
	return m->version == c.version && !m->reserved && !m->reserved2 &&
		m->leaf1_ecx == c.leaf1_ecx && m->leaf1_edx == c.leaf1_edx &&
		m->leaf7_ebx == c.leaf7_ebx && !m->leaf7_ecx && !m->leaf7_edx &&
		m->ext1_ecx == c.ext1_ecx && m->ext1_edx == c.ext1_edx &&
		m->xcr0 == c.xcr0 && (m->xcr0 & 3) == 3 &&
		(m->leaf1_edx & VMCTX_CPU_1D) == VMCTX_CPU_1D &&
		(m->ext1_edx & VMCTX_CPU_ED) == VMCTX_CPU_ED;
}

static inline int vmctx_cpu_model_subset(const struct vmctx_cpu_model *m,
	const struct vmctx_cpu_model *caps)
{
	return vmctx_cpu_model_valid(m) &&
		!(m->leaf1_ecx & ~caps->leaf1_ecx) && !(m->leaf1_edx & ~caps->leaf1_edx) &&
		!(m->leaf7_ebx & ~caps->leaf7_ebx) &&
		!(m->ext1_ecx & ~caps->ext1_ecx) && !(m->ext1_edx & ~caps->ext1_edx) &&
		!(m->xcr0 & ~caps->xcr0);
}

/* All unlisted leaves/subleaves are zero, never native CPUID passthrough. */
static inline void vmctx_cpu_model_cpuid(const struct vmctx_cpu_model *m,
	__u32 leaf, __u32 subleaf, __u32 *a, __u32 *b, __u32 *c, __u32 *d)
{
	*a = *b = *c = *d = 0;
	switch (leaf) {
	case 0:
		*a = 0xd;
		/* Fixed compatibility vendor, unrelated to either host. Some
		 * loaders skip baseline feature discovery for unknown vendors.
		 * Family/model stay generic and every feature remains negotiated. */
		*b = 0x756e6547; *d = 0x49656e69; *c = 0x6c65746e; /* GenuineIntel */
		break;
	case 1:
		*a = 0x600; *b = 1U << 16; /* generic family 6, one logical CPU */
		*c = m->leaf1_ecx; *d = m->leaf1_edx;
		break;
	case 7:
		if (!subleaf) *b = m->leaf7_ebx;
		break;
	case 0xd:
		if (!(m->leaf1_ecx & VMCTX_CPU_BIT(26))) break;
		if (!subleaf) {
			*a = m->xcr0;
			*b = *c = (m->xcr0 & 4) ? 832 : 576;
		} else if (subleaf == 2 && (m->xcr0 & 4)) {
			*a = 256; *b = 576;
		}
		break;
	case 0x80000000:
		*a = 0x80000001;
		break;
	case 0x80000001:
		*c = m->ext1_ecx; *d = m->ext1_edx;
		break;
	}
}
#endif
