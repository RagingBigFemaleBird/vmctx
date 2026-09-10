// SPDX-License-Identifier: GPL-2.0
/* Validate what guest instructions actually observe, including a real XSAVE,
 * prefixed CPUID, and the contract's inheritance through fork and exec. */
#define _GNU_SOURCE
#include <cpuid.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <unistd.h>
#include "cpu-proc-check.h"

static uint64_t digest(void)
{
	uint64_t h = 1469598103934665603ULL;
	const unsigned leaves[] = {0,1,4,7,0xb,0xd,0x15,0x16,0x1f,0x40000000,
		0x80000000,0x80000001,0x80000008,0x8000000a,~0U};
	for (unsigned i = 0; i < sizeof(leaves)/sizeof(leaves[0]); i++)
		for (unsigned sub = 0; sub < 12; sub++) {
			unsigned a,b,c,d;
			__cpuid_count(leaves[i], sub, a,b,c,d);
			unsigned v[] = {a,b,c,d};
			for (unsigned j = 0; j < 4; j++) h = (h ^ v[j]) * 1099511628211ULL;
		}
	return h;
}

int main(int argc, char **argv)
{
	unsigned a,b,c,d;
	const char *stage = "vendor/auxv";
	char vendor[13];
	__cpuid_count(0,0,a,b,c,d);
	memcpy(vendor,&b,4); memcpy(vendor+4,&d,4); memcpy(vendor+8,&c,4); vendor[12]=0;
	if (strcmp(vendor,"GenuineIntel") || a != 0xd || getauxval(AT_HWCAP2)) goto fail;
	stage = "prefixed CPUID";
	__cpuid_count(1,0,a,b,c,d);
	if (a != 0x600 || b != (1U << 16)) goto fail;
	unsigned features = c;
	unsigned pa=1,pb=0,pc=0,pd=0;
	asm volatile(".byte 0x66; cpuid" : "+a"(pa),"=b"(pb),"+c"(pc),"=d"(pd));
	if (pa!=a || pb!=b || pc!=c || pd!=d) goto fail;
	stage = "hidden CPUID leaves";
	const unsigned hidden[][2] = {{4,0},{7,1},{0xd,1},{0xd,9},{0xb,0},{0x15,0},
		{0x16,0},{0x1f,0},{0x40000000,0},{0x80000008,0},{0x8000000a,0},{~0U,0}};
	for (unsigned i=0; i<sizeof(hidden)/sizeof(hidden[0]); i++) {
		__cpuid_count(hidden[i][0],hidden[i][1],a,b,c,d);
		if (a|b|c|d) goto fail;
	}
	if (features & (1U<<26)) {
		stage = "XCR0/XSAVE geometry";
		unsigned lo, hi;
		asm volatile("xgetbv" : "=a"(lo),"=d"(hi) : "c"(0));
		__cpuid_count(0xd,0,a,b,c,d);
		if (hi || (lo!=3 && lo!=7) || a!=lo || d || b!=c || b!=(lo==7 ? 832U : 576U)) goto fail;
		_Alignas(64) unsigned char area[1024];
		memset(area,0xcc,sizeof(area));
		/* XSAVE preserves XSTATE_BV bits outside the requested mask;
		 * initialize the header while keeping the size canary intact. */
		memset(area+512,0,64);
		if (lo==7) asm volatile("vpxor %%ymm0,%%ymm0,%%ymm0" ::: "xmm0");
		asm volatile("xsave (%0)" :: "r"(area),"a"(lo),"d"(0) : "memory");
		stage = "XSAVE write bounds";
		for (unsigned i=b; i<sizeof(area); i++) if (area[i]!=0xcc) goto fail;
		uint64_t active;
		memcpy(&active,area+512,8);
		stage = "XSAVE active components";
		if (active & ~(uint64_t)lo) goto fail;
	}
	stage = "source /proc/cpuinfo contract";
	uint32_t words[5];
	__cpuid_count(1,0,a,b,c,d); words[0] = d; words[1] = c;
	__cpuid_count(7,0,a,b,c,d); words[2] = b;
	__cpuid_count(0x80000001,0,a,b,c,d); words[3] = c; words[4] = d;
	FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
	if (!cpuinfo) goto fail;
	int proc_ok = cpu_proc_check(cpuinfo, words);
	fclose(cpuinfo);
	if (!proc_ok) goto fail;
	uint64_t value = digest();
	stage = "fork/exec identity";
	if (argc == 3 && !strcmp(argv[1],"--child")) {
		if (value != strtoull(argv[2],NULL,16)) goto fail;
		return 0;
	}
	pid_t child=fork();
	if (child<0) return 2;
	if (!child) {
		char expected[32];
		snprintf(expected,sizeof(expected),"%llx",(unsigned long long)value);
		if (digest()!=value) _exit(1);
		execl("/proc/self/exe","cpu-model","--child",expected,(char*)NULL);
		_exit(2);
	}
	int status;
	if (waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)) goto fail;
	stage = "dynamic source loader";
	child = fork();
	if (child < 0) return 2;
	if (!child) {
		execl("/usr/bin/true", "true", (char *)NULL);
		_exit(2);
	}
	if (waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)) goto fail;
	printf("PASS: generic CPUID, bounded XSAVE, XCR0, prefixed CPUID, fork/exec identity %llx, dynamic source loader\n",
		(unsigned long long)value);
	return 0;
fail:
	fprintf(stderr,"FAIL: negotiated CPU model differs at %s (vendor=%s hwcap2=%lx eax=%x ebx=%x ecx=%x edx=%x)\n",
		stage,vendor,getauxval(AT_HWCAP2),a,b,c,d);
	return 1;
}
