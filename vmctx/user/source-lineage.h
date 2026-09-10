/* SPDX-License-Identifier: GPL-2.0 */
/* Fork lineage belongs to immutable MM lifetimes. It remains traversable
 * after an intermediate task exits or execs; live task representatives are
 * resolved separately immediately before accessing native memory. */
#ifndef VMR_SOURCE_LINEAGE_H
#define VMR_SOURCE_LINEAGE_H
static struct source_lineage { source_mm_id child, parent; } *fork_rel;
static size_t nfork_rel, fork_rel_capacity;
static pthread_mutex_t fork_rel_lock=PTHREAD_MUTEX_INITIALIZER;

static source_mm_id fork_parent_mm(source_mm_id child)
{
	source_mm_id parent=0;
	pthread_mutex_lock(&fork_rel_lock);
	for (size_t i=0;i<nfork_rel;i++)
		if (fork_rel[i].child==child) { parent=fork_rel[i].parent; break; }
	pthread_mutex_unlock(&fork_rel_lock);
	return parent;
}

static void fork_rel_note(source_mm_id cm, source_mm_id pm)
{
	if (!cm || !pm || cm<=pm) { errno=EPROTO; source_adapter_failed("fork MM lineage"); }
	pthread_mutex_lock(&fork_rel_lock);
	for (size_t i=0;i<nfork_rel;i++) {
		if (fork_rel[i].child!=cm) continue;
		if (fork_rel[i].parent!=pm) abort();
		pthread_mutex_unlock(&fork_rel_lock);
		return;
	}
	if (nfork_rel==fork_rel_capacity) {
		size_t capacity=fork_rel_capacity ? fork_rel_capacity*2 : 64;
		if (capacity<fork_rel_capacity || capacity>SIZE_MAX/sizeof(*fork_rel)) abort();
		void *grown=realloc(fork_rel,capacity*sizeof(*fork_rel));
		if (!grown) abort();
		fork_rel=grown; fork_rel_capacity=capacity;
	}
	fork_rel[nfork_rel++]=(struct source_lineage){cm,pm};
	pthread_mutex_unlock(&fork_rel_lock);
}

/* The bounded output is a caller's batch, never permission to omit siblings. */
static int fork_rel_children(source_mm_id pm, source_mm_id *out, int max)
{
	int n=0;
	pthread_mutex_lock(&fork_rel_lock);
	for (size_t i=0;i<nfork_rel;i++) {
		if (fork_rel[i].parent!=pm) continue;
		if (n==max) { pthread_mutex_unlock(&fork_rel_lock); errno=EOVERFLOW;
			source_adapter_failed("enumerating fork descendants"); }
		out[n++]=fork_rel[i].child;
	}
	pthread_mutex_unlock(&fork_rel_lock);
	/* An ended intermediate MM may still connect live descendants to this
	 * snapshot. Native task retirement cannot erase an inheritance edge. */
	return n;
}

struct source_cow_entry { source_mm_id mm; uint64_t page; };
static struct source_cow_entry *cow_broken;
static size_t cow_capacity, cow_count;
static pthread_mutex_t cow_broken_lock=PTHREAD_MUTEX_INITIALIZER;

static size_t source_cow_slot(source_mm_id mm, uint64_t page, size_t capacity)
{
	uint64_t hash=mm*UINT64_C(0x9e3779b97f4a7c15) ^ (page>>12);
	hash^=hash>>30; hash*=UINT64_C(0xbf58476d1ce4e5b9);
	hash^=hash>>27;
	return (size_t)hash & (capacity-1);
}

static int cow_mm_is_broken(source_mm_id mm, uint64_t page)
{
	int found=0;
	page &= ~(uint64_t)(VMR_PG_SIZE-1);
	pthread_mutex_lock(&cow_broken_lock);
	if (cow_capacity) {
		size_t i=source_cow_slot(mm,page,cow_capacity);
		while (cow_broken[i].mm) {
			if (cow_broken[i].mm==mm && cow_broken[i].page==page) { found=1; break; }
			i=(i+1)&(cow_capacity-1);
		}
	}
	pthread_mutex_unlock(&cow_broken_lock);
	return found;
}

static void cow_mm_note_break(source_mm_id mm, uint64_t page)
{
	if (!mm) { errno=EPROTO; source_adapter_failed("recording fork page identity"); }
	page &= ~(uint64_t)(VMR_PG_SIZE-1);
	pthread_mutex_lock(&cow_broken_lock);
	if (!cow_capacity || cow_count>=cow_capacity/2) {
		size_t capacity=cow_capacity ? cow_capacity*2 : 256;
		if (capacity<cow_capacity || capacity>SIZE_MAX/sizeof(*cow_broken)) abort();
		struct source_cow_entry *grown=calloc(capacity,sizeof(*grown));
		if (!grown) abort();
		for (size_t j=0;j<cow_capacity;j++) {
			struct source_cow_entry entry=cow_broken[j];
			if (!entry.mm) continue;
			size_t i=source_cow_slot(entry.mm,entry.page,capacity);
			while (grown[i].mm) i=(i+1)&(capacity-1);
			grown[i]=entry;
		}
		free(cow_broken); cow_broken=grown; cow_capacity=capacity;
	}
	size_t i=source_cow_slot(mm,page,cow_capacity);
	while (cow_broken[i].mm) {
		if (cow_broken[i].mm==mm && cow_broken[i].page==page) goto done;
		i=(i+1)&(cow_capacity-1);
	}
	cow_broken[i]=(struct source_cow_entry){mm,page};
	cow_count++; n_cow_pages_owed++;
done:
	pthread_mutex_unlock(&cow_broken_lock);
}

static void cow_note_break(source_id child, uint64_t page)
{ cow_mm_note_break(as_of(child),page); }
#endif
