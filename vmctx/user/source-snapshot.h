/* SPDX-License-Identifier: GPL-2.0 */
/* Source Linux adapter. A newborn service task has not executed instructions
 * or serviced a call. Its resident private pages belong to its native fork
 * snapshot. Publish that ownership before the executor makes the child visible
 * to ancestor copy-on-write workers. No native VMA or pagemap flags cross the
 * wire. This scan requires the parent snapshot lease and an unreleased child. */
static int source_child_pages(source_target child, uint64_t cursor,
			      struct vmr_child_pages *batch)
{
	char *line = NULL;
	size_t cap = 0;
	FILE *maps;
	int fd, result = -EIO;
	uint64_t entries[256];

	memset(batch, 0, sizeof(*batch));
	if (cursor & (VMR_PG_SIZE - 1)) return -EINVAL;
	uint64_t maps_mm, pages_mm;
	int maps_fd=source_record_proc_open(source_target_id(child),"maps",&maps_mm);
	if (maps_fd<0) return -errno;
	maps=fdopen(maps_fd,"r");
	if (!maps) { result=-errno; close(maps_fd); return result; }
	fd = source_record_proc_open(source_target_id(child),"pagemap",&pages_mm);
	if (fd < 0) { result = -errno; goto out_maps; }
	if (maps_mm!=child->mm || pages_mm!=child->mm) { result=-ESTALE; goto out; }
	while (getline(&line, &cap, maps) >= 0) {
		unsigned long long lo, hi;
		char perm[5];
		if (sscanf(line, "%llx-%llx %4s", &lo, &hi, perm) != 3) goto out;
		if (perm[3] != 'p' || hi <= cursor || hi >= (UINT64_C(1) << 63)) continue;
		uint64_t address = lo < cursor ? cursor : lo;
		for (; address < hi;) {
			size_t n = (hi - address) / VMR_PG_SIZE;
			if (n > 256) n = 256;
			ssize_t got;
			do { got = pread(fd, entries, n * sizeof(*entries),
				(off_t)(address / VMR_PG_SIZE * sizeof(*entries))); }
			while (got < 0 && errno == EINTR);
			if (got != (ssize_t)(n * sizeof(*entries))) goto out;
			for (size_t i = 0; i < n; i++, address += VMR_PG_SIZE) {
				if (!(entries[i] & (UINT64_C(1) << 63))) continue;
				/* Its own native snapshot supersedes ancestor guesses.
				 * The task stays parked throughout this publication. */
				if (pg_set(child, address, PG_OURS)) goto out;
				cow_mm_note_break(child->mm, address);
				batch->pages[batch->count++] = address;
				if (batch->count == VMR_CHILD_PAGES_MAX) {
					batch->next = address + VMR_PG_SIZE;
					result = 0; goto out;
				}
			}
		}
	}
	if (!ferror(maps)) result = 0;
out:
	close(fd);
out_maps:
	free(line);
	fclose(maps);
	return result;
}
