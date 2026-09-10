/* SPDX-License-Identifier: GPL-2.0 */
/* Included by the execution adapter after its context/ctl helpers. A source
 * preparation request selects this operation. The identity is the published
 * address-space lineage; no source syscall or native clone flag is decoded.
 *
 * Lock order: snapshot serial -> short context/lineage locks -> native leases.
 * Page-service threads never acquire the serial lock: they must remain able
 * to answer source faults while a source snapshot is in progress. */
struct snapshot_serial {
	struct snapshot_serial *next;
	uint64_t as;
	pthread_mutex_t mutex;
};
static pthread_mutex_t snapshot_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct snapshot_serial *snapshot_registry;
struct snapshot_lease {
	struct snapshot_serial *serial;
	unsigned n;
	struct vmctx_quiesce gates[MAX_CTX];
};

static void snapshot_end(struct snapshot_lease *s)
{
	while (s->n) {
		struct vmctx_quiesce *q = &s->gates[--s->n];
		q->op = VMCTX_QUIESCE_END;
		if (ctl(0, VMCTX_CTL_QUIESCE, q)) {
			perror("vmremote: releasing execution snapshot lease");
			abort();
		}
	}
	if (s->serial) {
		pthread_mutex_unlock(&s->serial->mutex);
		s->serial = NULL;
	}
}

static int snapshot_begin(memory_target pid, struct snapshot_lease *s)
{
	uint64_t as = as_id(pid);
	memory_target members[MAX_CTX];
	struct snapshot_serial *serial;
	int n;
	pthread_mutex_lock(&snapshot_registry_lock);
	for (serial = snapshot_registry; serial && serial->as != as; serial = serial->next) {}
	if (!serial) {
		serial = calloc(1, sizeof(*serial));
		if (serial) {
			serial->as = as;
			pthread_mutex_init(&serial->mutex, NULL);
			serial->next = snapshot_registry;
			snapshot_registry = serial;
		}
	}
	pthread_mutex_unlock(&snapshot_registry_lock);
	if (!serial) return -1;
	pthread_mutex_lock(&serial->mutex);
	s->serial = serial;
	/* Every source child creation takes this lock, so membership cannot
	 * grow between enumeration and publication of this operation's child. */
	n = as_members(pid, members, MAX_CTX);
	for (int i = 0; i < n; i++) {
		if (members[i] != pid && ctx_execution_ended(ctx_id(members[i]), 0)) continue;
		struct vmctx_quiesce q = {.version = VMCTX_QUIESCE_ABI,
			.size = sizeof(q), .op = VMCTX_QUIESCE_BEGIN};
		if (ctl(members[i], VMCTX_CTL_QUIESCE, &q)) {
			int saved = errno;
			if (members[i] != pid && ctx_wait_execution_ended(ctx_id(members[i]))) continue;
			fprintf(stderr, "vmremote: snapshot cannot pause member %d of MM %llu: %s\n",
				ctx_id(members[i]), (unsigned long long)as, strerror(saved));
			snapshot_end(s); errno = saved; return -1;
		}
		s->gates[s->n++] = q;
	}
	return 0;
}
