/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../user/vmrproto.h"
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL memory identity line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void)
{
	struct vmr_rsp reply={0};
	CHECK(vmr_rsp_bindings_valid(&reply));
	struct vmr_mm_binding parent={.context=UINT64_C(0xf000000100000002),
		.mm=UINT64_C(0x100000007),.epoch=31};
	struct vmr_mm_binding born={.context=UINT64_C(0xe000000100000002),
		.mm=UINT64_C(0x8000000100000007),.parent_mm=parent.mm,.epoch=1};
	CHECK(vmr_binding_valid(&parent) && vmr_binding_valid(&born));
	CHECK(!vmr_binding_equal(&parent,&born));
	/* Source context tokens currently fit signed 64 bits in child_source;
	 * MM identities retain all 64 bits and may share every low 32-bit bit. */
	born.context=52;
	reply.binding=parent;reply.child_binding=born;reply.child_source=52;
	reply.effects=VMR_EFFECT_CHILD;CHECK(vmr_rsp_bindings_valid(&reply));
	struct vmr_rsp copy;
	unsigned char bytes[sizeof(reply)];memcpy(bytes,&reply,sizeof(reply));memcpy(&copy,bytes,sizeof(copy));
	CHECK(vmr_binding_equal(&copy.binding,&parent) && vmr_binding_equal(&copy.child_binding,&born));
	CHECK(vmr_rsp_bindings_valid(&copy));
	copy.child_binding.context++;CHECK(!vmr_rsp_bindings_valid(&copy));copy=reply;
	copy.child_binding.mm=parent.mm;CHECK(!vmr_rsp_bindings_valid(&copy));copy=reply;
	copy.child_binding.parent_mm++;CHECK(!vmr_rsp_bindings_valid(&copy));copy=reply;
	copy.child_binding.epoch=0;CHECK(!vmr_rsp_bindings_valid(&copy));copy=reply;
	copy.binding.mm=0;CHECK(!vmr_rsp_bindings_valid(&copy));copy=reply;
	copy.binding.context=0;CHECK(!vmr_rsp_bindings_valid(&copy));copy=reply;
	copy.binding.parent_mm=parent.mm;CHECK(!vmr_rsp_bindings_valid(&copy));copy=reply;
	copy.effects=0;CHECK(!vmr_rsp_bindings_valid(&copy));
	copy.child_binding=(struct vmr_mm_binding){0};CHECK(vmr_rsp_bindings_valid(&copy));
	copy.binding.epoch=0;CHECK(!vmr_rsp_bindings_valid(&copy));
	reply.child_shared_mm=1;reply.child_binding.mm=parent.mm;
	reply.child_binding.parent_mm=parent.parent_mm;CHECK(vmr_rsp_bindings_valid(&reply));
	copy=reply;copy.child_binding.parent_mm=parent.mm;CHECK(!vmr_rsp_bindings_valid(&copy));
	copy=reply;copy.child_shared_mm=2;CHECK(!vmr_rsp_bindings_valid(&copy));
	copy=reply;copy.child_binding.context=parent.context;CHECK(!vmr_rsp_bindings_valid(&copy));
	puts("PASS: source MM publications retain full identities and reject missing generations, contradictory ancestry and malformed births");
	return 0;
}
