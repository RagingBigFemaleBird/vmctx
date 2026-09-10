/* SPDX-License-Identifier: GPL-2.0 */
/* One connection drives one native call. Admission, dispatch and completion
 * remain separate so ptrace/seccomp can change the call before memory work is
 * selected, and vfork can publish a child before its parent returns. */
struct source_admission {
	struct vmctx_syscall_gate gate;
	struct vmr_req admitted;
	struct vmr_mm_binding admitted_target;
	uint64_t next;
	int active, begun, prepared, committed, birth_seen;
};

static int source_admission_native(source_id id, struct source_admission *call,
		unsigned op)
{
	call->gate.op=op;
	if (!ctl(id,VMCTX_CTL_SYSCALL_GATE,&call->gate)) return 0;
	if (errno==EAGAIN || errno==EBUSY) return 1;
	return -1;
}

static int source_admission_birth(source_id id, struct source_admission *call,
		struct vmr_rsp *reply)
{
	if (call->birth_seen || !call->gate.child_host_pid) return 0;
	struct source_record *parent=source_record_get(id);
	if (!parent) return -1;
	struct source_context child={.fd=-1};
	int error=source_context_child(&parent->native,call->gate.ticket,&child);
	int saved=errno;
	source_record_put(parent);
	if (error) { errno=saved; return (saved==EBUSY || saved==EAGAIN) ? 1 : -1; }
	struct vmctx_context info;
	if (source_context_info(&child,&info)) {
		saved=errno; source_context_close(&child); errno=saved;
		return saved==EAGAIN ? 1 : -1;
	}
	/* A birth already ended before publication needs no executor task.
	 * Native wait/ptrace retain the source's actual child and exit status. */
	if (info.flags & VMCTX_CONTEXT_ENDED) {
		source_context_close(&child); call->birth_seen=1; return 0;
	}
	source_id born=source_record_add(&child);
	if (!born) return -1;
	/* Reserve the program lifetime before exposing the token. CHILD later
	 * transfers this same reference/count to its connection exactly once. */
	pthread_mutex_lock(&ctx_count_lock);
	n_contexts++;
	pthread_mutex_unlock(&ctx_count_lock);
	if(source_binding_snapshot(born,&reply->child_binding))return -1;
	if(!vmr_binding_valid(&call->admitted_target)) {errno=EPROTO;return -1;}
	if (!call->gate.child_shared_mm) {
		fork_rel_note(reply->child_binding.mm,call->admitted_target.mm);
		relro_inherit(reply->child_binding.mm,call->admitted_target.mm);
		reply->child_binding.parent_mm=call->admitted_target.mm;
	} else if(reply->child_binding.mm!=call->admitted_target.mm) {
		errno=ESTALE;return -1;
	}
	reply->effects|=VMR_EFFECT_CHILD;
	reply->child_source=born;
	reply->child_shared_mm=call->gate.child_shared_mm;
	call->birth_seen=1;
	return 0;
}

/* Returns 1 when final completion metadata may be built, 0 for an admission
 * or running reply, and -1 for a protocol/native failure. No polling call
 * repeats runtime credit, SETCPU, memory preparation or dispatch. */
static int source_admission_step(source_id id, struct source_admission *call,
		const struct vmr_req *request, struct vmr_cpu_state *cpu,
		struct vmr_rsp *reply)
{
	memset(reply,0,sizeof(*reply)); reply->magic=VMR_MAGIC;
	if (request->nr==VMR_OP_PREPARE) {
		if (call->active || request->ticket || vmr_cpu_wire_decode(cpu,request->datalen)) {
			errno=EPROTO; return -1;
		}
		if (call->next==UINT64_MAX) { errno=EOVERFLOW; return -1; }
		/* The Linux source adapter constructs the native entry frame once,
		 * before entry work. After BEGIN no incoming frame can overwrite it. */
		cpu->regs.orig_rax=request->call_nr;
		cpu->regs.rdi=request->args[0]; cpu->regs.rsi=request->args[1];
		cpu->regs.rdx=request->args[2]; cpu->regs.r10=request->args[3];
		cpu->regs.r8=request->args[4]; cpu->regs.r9=request->args[5];
		if (source_runtime_credit(id,request) || cpu_state_ctl(id,VMCTX_CTL_SETCPU,cpu))
			return -1;
		uint64_t next=call->next+1;
		*call=(struct source_admission){.next=next,.active=1,
			.gate={.version=VMCTX_SYSCALL_GATE_ABI,.size=sizeof(call->gate),
				.ticket=next,.state=VMCTX_GATE_ENTERING}};
		incall_set(id,1);
	} else if (!call->active || request->ticket!=call->gate.ticket ||
		   request->datalen || request->execution_epoch || request->execution_ns ||
		   request->call_nr || request->args[0] || request->args[1] ||
		   request->args[2] || request->args[3] || request->args[4] || request->args[5]) {
		errno=EPROTO; return -1;
	}
	reply->ticket=call->gate.ticket;
	int result;
	if (!call->begun) {
		result=source_admission_native(id,call,VMCTX_GATE_BEGIN);
		if (result<0) return -1;
		if (!result) call->begun=1;
	} else {
		result=source_admission_native(id,call,VMCTX_GATE_QUERY);
		if (result<0) return -1;
	}
	if (result>0) {
		reply->call_state=call->committed ? VMR_CALL_RUNNING :
			call->prepared ? VMR_CALL_ADMITTED : VMR_CALL_ENTERING;
		return 0;
	}
	if (call->gate.state==VMCTX_GATE_ADMITTED) {
		struct vmr_mm_binding observed;
		if(source_binding_snapshot(id,&observed))return -1;
		if(!vmr_binding_valid(&call->admitted_target)) {
			call->admitted_target=observed;
			call->admitted=(struct vmr_req){.nr=call->gate.call.nr};
			memcpy(call->admitted.args,call->gate.call.args,sizeof(call->admitted.args));
		} else if(!vmr_binding_equal(&call->admitted_target,&observed) ||
			call->admitted.nr!=call->gate.call.nr ||
			memcmp(call->admitted.args,call->gate.call.args,sizeof(call->admitted.args))) {
			errno=ESTALE;return -1;
		}
		if (is_clone_nr(call->admitted.nr))
			reply->prepare_flags=VMR_PREPARE_SETTLE|VMR_PREPARE_SNAPSHOT;
		if (request->nr==VMR_OP_EXECUTE && !call->committed) {
			if (!call->prepared) {
				ctx_forward_prepare(&call->admitted_target,&call->admitted);
				call->prepared=1;
			}
			result=source_admission_native(id,call,VMCTX_GATE_COMMIT);
			if (result<0) return -1;
			if (!result) call->committed=1;
		}
	} else if (request->nr==VMR_OP_EXECUTE && !call->committed) {
		errno=EPROTO; return -1;
	}
	result=source_admission_birth(id,call,reply);
	if (result<0) return -1;
	/* Export can briefly contend with another control. Do not finish and
	 * retire this ticket until its retained birth has been accounted for. */
	if (result>0) { reply->call_state=VMR_CALL_RUNNING; return 0; }
	unsigned status;
	if (task_dead_status(id,&status)) {
		reply->ended=1; reply->ended_status=status;
		reply->call_state=VMR_CALL_COMPLETE;
		incall_set(id,0); call->active=0;
		return 1;
	}
	switch (call->gate.state) {
	case VMCTX_GATE_ENTERING: reply->call_state=VMR_CALL_ENTERING; return 0;
	case VMCTX_GATE_ADMITTED: reply->call_state=VMR_CALL_ADMITTED; return 0;
	case VMCTX_GATE_RUNNING: reply->call_state=VMR_CALL_RUNNING; return 0;
	case VMCTX_GATE_COMPLETE:
		if (!call->committed) { errno=EPROTO; return -1; }
		reply->call_state=VMR_CALL_COMPLETE;
		incall_set(id,0); call->active=0;
		return 1;
	default: errno=EPROTO; return -1;
	}
}

static int source_admission_complete(source_id id, struct source_admission *call,
		struct vmr_rsp *reply, struct vmr_cpu_state *cpu, int verbose)
{
	const struct vmr_req *request=&call->admitted;
	struct vmr_uregs regs;
	int valid;
	if(source_binding_snapshot(id,&reply->binding))return -1;
	int image=(request->nr==SYS_execve || request->nr==SYS_execveat) &&
		!call->gate.dispatch_ret;
	if(!vmr_binding_valid(&call->admitted_target) ||
	   (image ? (reply->binding.context!=call->admitted_target.context ||
		 reply->binding.mm==call->admitted_target.mm ||
		 reply->binding.epoch<=call->admitted_target.epoch) :
		!vmr_binding_equal(&reply->binding,&call->admitted_target))) {
		errno=ESTALE;return -1;
	}
	reply->retval=ctx_forward(&reply->binding,request,&call->gate,reply,&regs,&valid,verbose);
	reply->regs=regs; reply->regs_valid=valid;
	if (reply->mapping.kind==VMR_MAP_SET) {
		struct vmctx_access_log committed={.version=VMCTX_ACCESS_ABI,.size=sizeof(committed),
			.mm_id=reply->binding.mm};
		struct source_memory_target target={id,reply->binding.mm,reply->binding.epoch};
		if(source_memory_target_origin_call(&target,VMCTX_CTL_ACCESS_LOG,&committed))return -1;
		if(committed.mm_id!=target.mm || !committed.construction || committed.error) {
			errno=EPROTO;return -1;
		}
		reply->mmseq=committed.construction;
	}
	if (image)
		reply->effects|=VMR_EFFECT_IMAGE;
	if (cpu_state_ctl(id,VMCTX_CTL_GETCPU,cpu)) return -1;
	/* COMPLETE publishes the canonical frame after native syscall-exit
	 * work. No pre-entry copy or raw dispatch value overwrites that frame. */
	cpu->regs=regs;
	reply->datalen=vmr_cpu_wire_size(cpu);
	if (!reply->datalen) { errno=EPROTO; return -1; }
	if (verbose)
		fprintf(stderr,"[vmhome] admitted source=%d ticket=%llu nr=%llu dispatch=%lld guest=%lld\n",
			id,(unsigned long long)call->gate.ticket,(unsigned long long)request->nr,
			(long long)call->gate.dispatch_ret,(long long)reply->retval);
	return 0;
}
