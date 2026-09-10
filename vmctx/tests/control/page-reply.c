// SPDX-License-Identifier: GPL-2.0
/* Fault injection against the real source page reader. No vmctx kernel needed.
 * cc -O2 -pthread -I../../kernel page-reply.c -o page-reply
 */
#define main vmhome_program_main
#include "../../user/vmhome.c"
#undef main
#include <assert.h>
static const struct vmr_mm_binding target={.context=123,.mm=UINT64_C(0x100000007),.epoch=7};

static unsigned corrupt_reply;

static void reply_case(uint32_t magic, int32_t status, size_t header,
		       size_t payload, int expected, int error)
{
	int fd[2];
	char sent[VMR_PG_SIZE], received[VMR_PG_SIZE];
	struct vmr_pgrsp reply = {.magic = magic, .status = status, .gen = 71,
        .target=target,.addr=0x4000,.op=VMR_PG_GET};
	switch(corrupt_reply) {
        case 1: reply.target.context++;break;
        case 2: reply.target.mm+=UINT64_C(1)<<32;break;
        case 3: reply.target.parent_mm=1;break;
        case 4: reply.target.epoch++;break;
        case 5: reply.addr+=4096;break;
        case 6: reply.op=VMR_PG_GETS;break;
        case 7: reply.reserved=1;break;
    }
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    memset(received,0xcc,sizeof(received));
	memset(sent, 0xa5, sizeof(sent));
	assert(write_all(fd[1], &reply, header) == 0);
	assert(write_all(fd[1], sent, payload) == 0);
	assert(!shutdown(fd[1], SHUT_WR));
	last_get_gen = 99;
	errno = 0;
	int result = pg_get_op(fd[0], VMR_PG_GET, 0x4000, received, &target, 0);
	assert(result == expected);
	if (expected < 0) {
		assert(errno == error);
		assert(last_get_gen == 99);
        for(unsigned i=0;i<sizeof(received);i++)assert((unsigned char)received[i]==0xcc);
	} else if (!expected) {
		assert(!memcmp(sent, received, sizeof(sent)));
		assert(last_get_gen == 71);
	}
	close(fd[0]); close(fd[1]);
}

static void *inflight_peer(void *arg)
{
	int fd = *(int *)arg;
	struct vmr_pgreq request;
	struct vmr_pgrsp reply = {.magic = VMR_PG_MAGIC,
		.status = VMR_PG_INFLIGHT, .gen = 72, .why = VMR_INFLIGHT_PULL};
	char page[VMR_PG_SIZE] = {0};
	while (recv(fd, &request, sizeof(request), MSG_WAITALL) == sizeof(request)) {
        reply.target=request.target;reply.addr=request.addr;reply.op=request.op;
		if (write_all(fd, &reply, sizeof(reply)) ||
		    write_all(fd, page, sizeof(page))) break;
	}
	return NULL;
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	for (unsigned spin = 0; spin < 2; spin++) {
		sock_spin_us = spin ? 50 : 0;
		reply_case(VMR_PG_MAGIC, 0, sizeof(struct vmr_pgrsp), VMR_PG_SIZE, 0, 0);
		reply_case(VMR_PG_MAGIC, VMR_PG_ABSENT, sizeof(struct vmr_pgrsp),
			VMR_PG_SIZE, VMR_PG_ABSENT, 0);
		reply_case(VMR_PG_MAGIC, -1234, sizeof(struct vmr_pgrsp), VMR_PG_SIZE,
			-1, EREMOTEIO); /* Opaque remote error, not local errno 1234. */
		reply_case(VMR_PG_MAGIC, 17, sizeof(struct vmr_pgrsp), VMR_PG_SIZE, -1, EPROTO);
		reply_case(0xbad, 0, sizeof(struct vmr_pgrsp), VMR_PG_SIZE, -1, EPROTO);
		reply_case(VMR_PG_MAGIC, 0, 0, 0, -1, ECONNRESET);
		reply_case(VMR_PG_MAGIC, 0, 5, 0, -1, ECONNRESET);
		reply_case(VMR_PG_MAGIC, 0, sizeof(struct vmr_pgrsp), 3, -1, ECONNRESET);
	}
    for(corrupt_reply=1;corrupt_reply<=7;corrupt_reply++)
        reply_case(VMR_PG_MAGIC,0,sizeof(struct vmr_pgrsp),VMR_PG_SIZE,-1,EPROTO);
    corrupt_reply=0;
	int fd[2];
	pthread_t peer;
	char page[VMR_PG_SIZE];
	assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
	assert(!pthread_create(&peer, NULL, inflight_peer, &fd[1]));
	last_get_gen=99;memset(page,0xcc,sizeof(page));
	uint64_t began = now_us();
	errno = 0;
	assert(pg_get_op(fd[0], VMR_PG_GET, 0x4000, page, &target, 0) == -1);
	assert(errno == ETIMEDOUT && now_us() - began >= 1500000 && last_get_gen==99);
    for(unsigned i=0;i<sizeof(page);i++)assert((unsigned char)page[i]==0xcc);
	close(fd[0]);
	assert(!pthread_join(peer, NULL));
	close(fd[1]);
	pg_dead_port = 31111;
	errno = 0;
	assert(pg_connect("127.0.0.1", 31111) == -1 && errno == ECONNREFUSED);
	errno = 0;
	assert(pg_connect("invalid-address", 31112) == -1 && errno == EINVAL);
	puts("PASS: page replies reject errors, malformed frames, truncation and unsettled transfers");
	return 0;
}
