/* SPDX-License-Identifier: GPL-2.0 */
/* Page service may discover that the source must interpret an exception.
 * That dispatch can write the faulting page itself (for example an NX stack
 * fault whose signal frame uses that stack). Finish and release both local
 * reservations before dispatch. A legal-access reply restarts page service
 * under a new reservation, so no pre-dispatch ownership or storage observation
 * survives the callback. The native fault event remains held throughout. */
enum { FAULT_NEEDS_OWNER = 2 };

static int fault_from_home_mode(memory_target pid, uint64_t vec, uint64_t addr,
				uint64_t err, int supply_only)
{
	uint64_t ak = as_id(pid), pk = addr & ~UINT64_C(4095);
	int source_allows_access = 0;

	if (vec == 14 && vac_recent_n) vac_recent_check(pid, addr, err);
	if (vec != 14)
		return supply_only ? 0 : owner_takes_fault(pid, vec, pk, addr, err) == 1;
	/* Prefill runs inside a page server's existing reservation. It never
	 * owns a native fault event and cannot dispatch an exception. */
	if (supply_only) {
		int result=fault_from_home_inner(pid, vec, addr, err, 1, 0);
		ack_flush(); /* before the page server releases its reservation */
		return result;
	}

	for (;;) {
		uint32_t generation = 0;
		int owf = ownf_claim(ak, pk, &generation);
		struct pg_claim claim = {0};
		int result, present;

		if (owf == OWNF_BUSY) {
			if (ownf_wait(ak, pk, generation)) {
				if (!as_any_present(pid, pk)) n_ownf_wake_absent++;
				return 1;
			}
			/* The authoritative local claim below still excludes the
			 * incumbent even if the advisory ownership wait times out. */
			owf = OWNF_OFF;
		}
		if (pg_try_claim(ak, pk, PST_REQUESTING, &claim) < 0) {
			n_handoff_yield++;
			if (owf == OWNF_CLAIMED) ownf_release(ak, pk, 0);
			return 1;
		}
		result = fault_from_home_inner(pid, vec, addr, err, 0, source_allows_access);
		/* A pending receipt describes bytes already landed by this
		 * reservation. Confirm it before another transfer can begin. */
		ack_flush();
		present = as_any_present(pid, pk);
		pg_finish_required(&claim, present ?
			((err & 2) ? PST_OWNED : PST_SHARED) : PST_INVALID);
		if (owf == OWNF_CLAIMED) ownf_release(ak, pk, present);
		if (result != FAULT_NEEDS_OWNER) return result;
		if (source_allows_access) abort(); /* no duplicate dispatch */
		if (coh_depth) abort(); /* native source work can require any page */
		fault_map.op = 0;
		result = owner_takes_fault(pid, vec, pk, addr, err);
		if (result != 2) return result == 1;
		source_allows_access = 1;
	}
}
