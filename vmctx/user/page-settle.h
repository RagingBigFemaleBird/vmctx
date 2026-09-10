/* SPDX-License-Identifier: GPL-2.0 */
/* The snapshot lease stops execution, not page-service workers. Reserve each
 * page across pull, storage, acknowledgement and loan retirement. Source CLAIM
 * requires dropping our reservation: its GET must be able to finish before the
 * source can give the page back. No coherence lock spans network or retry waits.
 * An unresolved page prevents source dispatch; a timeout never retires a loan. */
static int page_recall_all(memory_target ctx)
{
	static __thread char page[VMR_PG_SIZE];
	uint64_t as = as_id(ctx);
	unsigned long n = 0, yielded = 0;
	size_t count;
	int result = -1;
	struct loan *loans = loan_snapshot(as, &count);

	for (size_t i = 0; i < count; i++) {
		uint64_t base = loans[i].page, started = now_us();
		for (;;) {
			struct pg_request_claim transfer
				__attribute__((cleanup(pg_request_release))) = {0};
			int prior = pg_try_claim(as, base, PST_REQUESTING, &transfer.claim);
			int wait = prior < 0;
			if (wait && errno != EBUSY) goto done;
			transfer.final = prior < 0 ? PST_INVALID : (enum pg_st)prior;
			if (!wait) {
				/* The copied key is a candidate; ownership is read only
				 * after acquiring this episode's reservation. */
				uint64_t owner = page_holder(ctx, base);
				if (!owner) break;
				int shared = page_shared(ctx, base);
				/* A shared loan's bytes live at an object/offset slot,
				 * even when this context has never mapped that page.
				 * Retain that slot across the pull; private backing at
				 * the same virtual address is a different page. */
				int mapped_shared = shared_range_has(ctx, base);
				off_t shared_offset = mapped_shared ? shared_obj_off(ctx, base) : -1;
				if (mapped_shared && shared_offset < 0) {
					fprintf(stderr, "[vmremote] settle failed: shared page 0x%llx has no backing slot\n",
						(unsigned long long)base);
					goto done;
				}
				struct source_page_receipt receipt
					__attribute__((cleanup(source_receipt_finish)))={0};
				int pull = ctx_page_pull(ctx, base, page,&receipt);
				if (shared_range_has(ctx, base) != mapped_shared ||
				    (mapped_shared && shared_obj_off(ctx, base) != shared_offset)) {
					fprintf(stderr, "[vmremote] settle failed: page 0x%llx changed backing during the pull\n",
						(unsigned long long)base);
					goto done;
				}
				if (pull == SETTLE_CLAIM) {
					pg_request_release(&transfer);
					wait = 1;
				} else if (pull == SETTLE_BYTES) {
					coh_enter();
					long copied = mapped_shared ?
						pwrite(shared_obj, page, VMR_PG_SIZE, shared_offset) :
						poke(ctx, base, page, VMR_PG_SIZE);
					if (copied == (long)VMR_PG_SIZE) {
						mark_installed(ctx, base, VMR_PG_SIZE);
					} else if (!mapped_shared && obj_write(ctx, base, page, VMR_PG_SIZE) ==
						   (long)VMR_PG_SIZE) {
						n_settle_to_object++;
						retain_put(ctx, base, page);
					} else {
						coh_leave();
						fprintf(stderr, "[vmremote] settle failed: page 0x%llx has no complete landing\n",
							(unsigned long long)base);
						goto done;
					}
					coh_leave();
					ack_installed(&receipt,base);
					ack_flush();
				} else if (pull == SETTLE_ABSENT || pull == SETTLE_NOTHOLDER) {
					int present = mapped_shared ?
						shared_obj_has(shared_offset) &&
						pread(shared_obj, page, VMR_PG_SIZE, shared_offset) == (long)VMR_PG_SIZE :
						own_object_page(ctx, base, page);
					if (!present) {
						fprintf(stderr, "[vmremote] settle failed: page 0x%llx has no surviving object copy\n",
							(unsigned long long)base);
						goto done;
					}
				} else {
					fprintf(stderr, "[vmremote] settle failed: page 0x%llx owner %llu pull status %d\n",
						(unsigned long long)base, (unsigned long long)owner, pull);
					goto done;
				}
				if (!wait) {
					coh_enter();
					if (shared) page_make_writable(ctx, base);
					page_lend(ctx, base, 0);
					coh_leave();
					transfer.final = shared ? PST_SHARED : PST_OWNED;
					n++;
					break;
				}
			}
			/* Keep source callbacks able to complete the crossing. This
			 * four-second diagnosis bound is below the native fault bound;
			 * exceeding it fails preparation without authorizing dispatch. */
			if (now_us() - started >= UINT64_C(4000000)) {
				fprintf(stderr, "[vmremote] settle failed: page 0x%llx transfer did not settle in four seconds\n",
					(unsigned long long)base);
				goto done;
			}
			yielded++;
			usleep(1000);
		}
	}
	result = 0;
done:
	free(loans);
	ack_flush();
	if (n || yielded)
		fprintf(stderr, "[vmremote] settled %lu page(s) into context %d before snapshot (%lu transfer yields)\n",
			n, ctx_id(ctx), yielded);
	return result;
}
