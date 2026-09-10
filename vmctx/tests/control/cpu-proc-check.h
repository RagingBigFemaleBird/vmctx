/* SPDX-License-Identifier: GPL-2.0 */
/* Compare the entire advertised flag set with an independently supplied CPU
 * contract. This also rejects native topology/cache/frequency identification. */
#ifndef CPU_PROC_CHECK_H
#define CPU_PROC_CHECK_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cpu_proc_check(FILE *f, const uint32_t words[5])
{
	static const struct { const char *name; unsigned word, bit; } flags[] = {
		{"fpu",0,0}, {"tsc",0,4}, {"cx8",0,8}, {"cmov",0,15},
		{"mmx",0,23}, {"fxsr",0,24}, {"sse",0,25}, {"sse2",0,26},
		{"pni",1,0}, {"pclmulqdq",1,1}, {"ssse3",1,9}, {"fma",1,12},
		{"cx16",1,13}, {"sse4_1",1,19}, {"sse4_2",1,20}, {"movbe",1,22},
		{"popcnt",1,23}, {"aes",1,25}, {"xsave",1,26}, {"avx",1,28},
		{"f16c",1,29}, {"rdrand",1,30},
		{"bmi1",2,3}, {"avx2",2,5}, {"bmi2",2,8}, {"erms",2,9},
		{"rdseed",2,18}, {"adx",2,19}, {"sha_ni",2,29},
		{"lahf_lm",3,0}, {"abm",3,5},
		{"syscall",4,11}, {"nx",4,20}, {"lm",4,29},
	};
	char *line = NULL;
	size_t cap = 0;
	unsigned entries = 0, vendors = 0, names = 0, sets = 0;
	int ok = 1;
	while (getline(&line, &cap, f) >= 0 && ok) {
		if (!strcmp(line, "\n")) continue;
		char *value = strchr(line, ':');
		if (!value) { ok = 0; break; }
		*value++ = 0;
		line[strcspn(line, "\t")] = 0;
		value += strspn(value, " \t");
		value[strcspn(value, "\n")] = 0;
		if (!strcmp(line, "processor")) entries++;
		else if (!strcmp(line, "vendor_id")) { vendors++; ok = !strcmp(value, "GenuineIntel"); }
		else if (!strcmp(line, "model name")) { names++; ok = !strcmp(value, "Generic x86-64"); }
		else if (!strcmp(line, "cpu family")) ok = !strcmp(value, "6");
		else if (!strcmp(line, "model") || !strcmp(line, "stepping")) ok = !strcmp(value, "0");
		else if (!strcmp(line, "cpuid level")) ok = !strcmp(value, "13");
		else if (!strcmp(line, "fpu") || !strcmp(line, "fpu_exception") || !strcmp(line, "wp")) ok = !strcmp(value, "yes");
		else if (!strcmp(line, "flags")) {
			unsigned seen[sizeof(flags)/sizeof(flags[0])] = {0};
			char *save, *s = strtok_r(value, " \t", &save);
			sets++;
			for (; s && ok; s = strtok_r(NULL, " \t", &save)) {
				unsigned i;
				for (i = 0; i < sizeof(flags)/sizeof(flags[0]); i++)
					if (!strcmp(s, flags[i].name)) break;
				if (i == sizeof(flags)/sizeof(flags[0]) || seen[i]++) {
					fprintf(stderr, "unexpected or repeated CPU flag: %s\n", s);
					ok = 0;
				}
			}
			for (unsigned i = 0; i < sizeof(flags)/sizeof(flags[0]); i++)
				if (seen[i] != !!(words[flags[i].word] & (1U << flags[i].bit))) {
					fprintf(stderr, "CPU flag differs from contract: %s\n", flags[i].name);
					ok = 0;
				}
		} else {
			fprintf(stderr, "native CPU field exposed: %s\n", line);
			ok = 0;
		}
	}
	ok &= !ferror(f) && entries && vendors == entries && names == entries && sets == entries;
	free(line);
	return ok;
}
#endif
