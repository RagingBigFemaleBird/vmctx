/* SPDX-License-Identifier: GPL-2.0 */
/* Each MM serializes its READ/apply/ACK cursor through a private mutex.
 * Network calls hold no layout or native binding guard: a source callback can
 * complete local mapping work before answering. Only local application holds
 * layout_order_lock. All batch validation precedes the first local mutation. */
struct access_cursor {
	uint64_t as;
	uint64_t identity, acknowledged;
	pthread_mutex_t serial;
	int failed;
	struct access_cursor *next;
};
static struct access_cursor *access_cursors;
static pthread_mutex_t access_cursors_lock = PTHREAD_MUTEX_INITIALIZER;

static struct access_cursor *access_cursor_for(uint64_t as)
{
    if (!as) {errno=EINVAL;return NULL;}
    pthread_mutex_lock(&access_cursors_lock);
    struct access_cursor *c;
    for (c=access_cursors;c;c=c->next) if(c->as==as) break;
    if (!c) {
        c=calloc(1,sizeof(*c));
        if (c) {
            int error=pthread_mutex_init(&c->serial,NULL);
            if(error) {free(c);c=NULL;errno=error;}
            else {
                c->as=c->identity=as;
                c->next=access_cursors;access_cursors=c;
            }
        }
    }
    pthread_mutex_unlock(&access_cursors_lock);
    return c;
}

/* Split at newer published mapping incarnations. A journal can arrive after
 * a later mapping reply on another context's connection. Protecting or
 * discarding the replaced incarnation must not modify the new one. */
static uint64_t access_construction_boundary(uint64_t as, uint64_t start,
		uint64_t end, uint64_t sequence, unsigned int kind, int *skip)
{
	uint64_t next = end;
	*skip = 0;
	pthread_mutex_lock(&cons_lock);
	for (size_t i = 0; i < cons_n; i++) {
		struct construction *c = &cons[i];
		if (c->as != as || c->op == VMCTX_MAP_PROT || c->seq < sequence ||
		    (c->seq == sequence && kind != VMR_ACCESS_UNMAP) ||
		    c->end <= start || c->start >= end) continue;
		if (c->start <= start) {
			*skip = 1;
			if (c->end < next) next = c->end;
		} else if (c->start < next) next = c->start;
	}
	pthread_mutex_unlock(&cons_lock);
	return next;
}

static int access_apply_span(memory_target pid, const struct vmr_access_event *e,
			     uint64_t start, uint64_t end)
{
	uint64_t as = as_id(pid);
	if (e->kind == VMR_ACCESS_DISCARD || e->kind == VMR_ACCESS_UNMAP) {
		range_vacated_seq(pid, start, end, e->kind == VMR_ACCESS_DISCARD, e->sequence);
		if (e->kind == VMR_ACCESS_DISCARD) return 0;
	}
	while (start < end) {
		struct access_range r;
		int have = access_lookup(as, start, end, &r);
		uint64_t next = r.end < end ? r.end : end;
		if (next <= start) return -1;
		if (have && r.sequence > e->sequence) { start = next; continue; }
		r.start = start; r.end = next; r.sequence = e->sequence;
		r.protection = e->protection;
		if (e->kind != VMR_ACCESS_PROTECT) r.denied = e->kind == VMR_ACCESS_DENY;
		if (e->kind == VMR_ACCESS_UNMAP) { r.denied = 1; r.protection = 0; }
		access_note(r);
		if (e->kind == VMR_ACCESS_PROTECT || e->kind == VMR_ACCESS_DENY ||
		    e->kind == VMR_ACCESS_ALLOW) {
			unsigned int prot = r.denied ? 0 :
				((r.protection & VMR_PROT_READ) ? PROT_READ : 0) |
				((r.protection & VMR_PROT_WRITE) ? PROT_WRITE : 0) |
				((r.protection & VMR_PROT_EXEC) ? PROT_EXEC : 0);
			struct vmctx_reply change = {
				.map_op = VMCTX_MAP_PROT, .map_addr = start,
				.map_len = next - start, .map_prot = prot,
			};
			/* Every native MM must complete its PTE/TLB update before ACK. */
			if (as_apply_map_siblings(pid, &change)) return -1;
			shared_layout_protect(as, start, next, prot);
			cons_note_locked(pid, start, next, e->sequence, VMCTX_MAP_PROT, prot, 0);
			if (e->kind == VMR_ACCESS_DENY)
				range_vacated_1(pid, start, next, 1);
		}
		start = next;
	}
	return 0;
}

static int access_batch_valid(const struct vmr_access_batch *b,
        uint64_t identity,uint64_t acknowledged)
{
    if(b->identity!=identity || b->reserved || b->count>VMR_ACCESS_MAX ||
       b->acknowledged!=acknowledged || b->cursor>b->head) return 0;
    uint64_t previous=acknowledged;
    for(unsigned i=0;i<b->count;i++) {
        const struct vmr_access_event *e=&b->event[i];
        if(e->sequence<=previous || e->sequence>b->head || e->start>=e->end ||
           ((e->start|e->end)&(VMR_PG_SIZE-1)) ||
           e->kind<VMR_ACCESS_UNMAP || e->kind>VMR_ACCESS_CONSTRUCT ||
           (e->protection & ~(VMR_PROT_READ|VMR_PROT_WRITE|VMR_PROT_EXEC))) return 0;
        previous=e->sequence;
    }
    return b->cursor==previous;
}

static int access_apply_batch(memory_target pid,const struct vmr_access_batch *batch)
{
    int result=0;
    pthread_mutex_lock(&layout_order_lock);
    for(unsigned i=0;i<batch->count && !result;i++) {
        const struct vmr_access_event *e=&batch->event[i];
        map_trace("executor","access as=%llu identity=%llu seq=%llu kind=%u range=%llx-%llx prot=%u",
            (unsigned long long)as_id(pid),(unsigned long long)batch->identity,
            (unsigned long long)e->sequence,e->kind,(unsigned long long)e->start,
            (unsigned long long)e->end,e->protection);
        for(uint64_t start=e->start;start<e->end;) {
            int skip;
            uint64_t end=access_construction_boundary(as_id(pid),start,e->end,
                e->sequence,e->kind,&skip);
            if(end<=start || (!skip && access_apply_span(pid,e,start,end))) {result=-1;break;}
            start=end;
        }
    }
    pthread_mutex_unlock(&layout_order_lock);
    return result;
}

static int access_sync(memory_target pid)
{
    struct access_cursor *c=access_cursor_for(as_id(pid));
    if(!c)return -1;
    pthread_mutex_lock(&c->serial);
    int result=-1,error=c->failed ? c->failed : EPROTO;
    if(c->failed)goto done;
    for(;;) {
        struct vmr_access_batch batch={0},ack={0};
        struct vmr_rsp receipt={0};
        uint64_t args[6]={c->identity,c->acknowledged};
        long n=home_memory_call(pid,VMR_OP_ACCESS_READ,args,NULL,0,&batch,sizeof(batch),&receipt);
        if(n!=sizeof(batch) || receipt.datalen!=sizeof(batch) ||
           !access_batch_valid(&batch,c->identity,c->acknowledged)) goto done;
        if(!batch.count) {result=0;break;}
        if(access_apply_batch(pid,&batch))goto done;
        args[1]=batch.cursor;
        receipt=(struct vmr_rsp){0};
        n=home_memory_call(pid,VMR_OP_ACCESS_ACK,args,NULL,0,&ack,sizeof(ack),&receipt);
        if(n!=sizeof(ack) || receipt.datalen!=sizeof(ack) || ack.reserved ||
           ack.identity!=c->identity || ack.acknowledged!=batch.cursor ||
           ack.cursor!=batch.cursor || ack.head<batch.cursor || ack.count)goto done;
        c->acknowledged=batch.cursor;
        if(batch.count<VMR_ACCESS_MAX) {result=0;break;}
    }
done:
    if(result)c->failed=error;
    pthread_mutex_unlock(&c->serial);
    if(result)errno=error;
    return result;
}
