// SPDX-License-Identifier: GPL-2.0
/* Framing must preserve every active byte and reject incomplete state before
 * the native adapter sees it. This control needs no vmctx kernel. */
#include "../../user/cpu-wire.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static void reject(struct vmr_cpu_state *cpu, size_t len)
{
	struct vmr_cpu_state before = *cpu;
	errno = 0;
	assert(vmr_cpu_wire_decode(cpu, len) == -1 && errno == EPROTO);
	assert(!memcmp(cpu, &before, sizeof(before)));
}

int main(void)
{
	const unsigned sizes[] = {576, 840, 4096, VMR_XSTATE_MAX};
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		struct vmr_cpu_state source, received;
		unsigned char *bytes = (unsigned char *)&source;
		for (size_t j = 0; j < sizeof(source); j++)
			bytes[j] = (j * 71 + j / 251) & 255;
		source.xstate_size = sizes[i]; source.reserved = 0;
		size_t len = vmr_cpu_wire_size(&source);
		assert(len == 176 + sizes[i]);
		memset(&received, 0xe5, sizeof(received));
		memcpy(&received, &source, len);
		reject(&received, len - 1);
		reject(&received, len + 1);
		reject(&received, SIZE_MAX);
		assert(!vmr_cpu_wire_decode(&received, len));
		assert(!memcmp(&received, &source, len));
		for (size_t j = len; j < sizeof(received); j++)
			assert(!((unsigned char *)&received)[j]);
		received.reserved = 1; reject(&received, len);
		received.reserved = 0;
		received.xstate_size = 0; reject(&received, 176);
		received.xstate_size = 575; reject(&received, 751);
		received.xstate_size = VMR_XSTATE_MAX + 1;
		reject(&received, sizeof(received));
	}
	/* A short header may end at the last readable byte. Reject by framing
	 * without reading xstate_size or any of the absent registers. */
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	unsigned char *p = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(p != MAP_FAILED && !mprotect(p + page, page, PROT_NONE));
	for (size_t len = 0; len < 176; len += 8)
		assert(vmr_cpu_wire_decode((void *)(p + page - len), len) == -1);
	assert(!munmap(p, page * 2));
	puts("PASS: CPU framing preserves all active bytes and rejects missing, extra or invalid state");
	return 0;
}
