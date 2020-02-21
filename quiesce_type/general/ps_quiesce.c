#include <ps_smr.h>

int __ps_smr_reclaim_batch(int batch, ps_tsc_t qsc, coreid_t curr, struct ps_qsc_list *ql, struct ps_mem *m, ps_free_fn_t ffn);

inline void
__ps_invoke_smr(coreid_t curr, struct ps_qsc_list *ql, struct ps_smr_info *si, struct ps_mem *mem, ps_free_fn_t ffn)
{
	if (unlikely(si->account.qmemcnt >= si->account.qmemtarget)) __ps_smr_reclaim(curr, ql, si, mem, ffn);
}

static inline void
__ps_timing_update_remote(struct parsec *parsec, struct ps_smr_percore *curr, int remote_cpu)
{
	struct ps_quiescence_timing *cpu_i;

	cpu_i = &(parsec->timing_info[remote_cpu].timing);

	curr->timing_others[remote_cpu].time_in  = cpu_i->time_in;
	curr->timing_others[remote_cpu].time_out = cpu_i->time_out;

	/*
	 * We are reading remote cachelines possibly, so this time
	 * stamp reading cost is fine.
	 */
	curr->timing_others[remote_cpu].time_updated = ps_tsc();

	/* If remote core has information that can help, use it. */
	if (curr->timing.last_known_quiescence < cpu_i->last_known_quiescence) {
		curr->timing.last_known_quiescence = cpu_i->last_known_quiescence;
	}

	ps_mem_fence();

	return;
}

static int
ps_quiesce(struct parsec *parsec, ps_tsc_t tsc, const int blocking, ps_tsc_t *qsc)
{
	int inlib_curr, qsc_cpu, curr_cpu, first_try, i, done_i;
	ps_tsc_t min_known_qsc;
	ps_tsc_t in, out, update;
	struct ps_smr_percore *cpuinfo;
	struct ps_quiescence_timing *timing_local;
	ps_tsc_t time_check;
	assert(parsec);

	time_check   = tsc;
	curr_cpu     = ps_coreid();
	cpuinfo      = &(parsec->timing_info[curr_cpu]);
	timing_local = &cpuinfo->timing;
	inlib_curr   = __ps_in_lib(timing_local);

	*qsc = timing_local->last_known_quiescence;
	/*
	 * We cannot attempt quiescence for a time after we entered
	 * the library.  By the definition of quiescence, this is not
	 * possible.  Thus, ensure quiescence on the current core:
	 * either time_in > time_check, or we are not in the lib right
	 * now.  Either call ps_quiesce when we aren't in the library,
	 * or for a quiescence period _before_ when we entered.
	 */
	if (unlikely((time_check > timing_local->time_in) && inlib_curr)) return -EQUIESCENCE;

	min_known_qsc = (unsigned long long)(-1); /* start with the largest value */
	for (i = 1 ; i < PS_NUMCORES ; i++) {
		/* Make sure we don't all hammer core 0... */
		qsc_cpu = (curr_cpu + i) % PS_NUMCORES;
		assert(qsc_cpu != curr_cpu);

		first_try = 1;
		done_i    = 0;
re_check:
		/* If we can use the quiescence for another core */
		if (time_check < timing_local->last_known_quiescence) break;

		/* Use our cached values of the other core's values */
		in     = cpuinfo->timing_others[qsc_cpu].time_in;
		out    = cpuinfo->timing_others[qsc_cpu].time_out;
		update = cpuinfo->timing_others[qsc_cpu].time_updated;

		/*
		 * If the time is before the last in-tsc, or the other
		 * cores has entered and exited the parallel section,
		 * and our updated version of its timing happened
		 * before the time in question, this core is done.
		 */
		if ((time_check < in) || ((time_check < update) && (in < out))) done_i = 1;

		if (done_i) {
			/*
			 * We want to update our own version of the
			 * time furthest into the past that quiescence
			 * has been observed.
			 */
			/* assertion: update >= in */
			if (in < out) {
				if (min_known_qsc > update) min_known_qsc = update;
			} else {
				if (min_known_qsc > in)     min_known_qsc = in;
			}
			continue; /* move on to the next core... */
		}

		/*
		 * If no blocking allowed, then read at most one remote
		 * cacheline per core.
		 */
		if      (first_try) first_try = 0;
		else if (!blocking) return -1;

		/*
		 * If we couldn't satisfy the quiescence locally, then
		 * we need to update our cached state for the remote
		 * core.
		 */
		__ps_timing_update_remote(parsec, cpuinfo, qsc_cpu);

		goto re_check;
	}

	/*
	 * Update our cached value of the last known quiescence value.
	 * This is a little complicated as it can be updated to the
	 * min_known_qsc if we had to iterate through all cores (thus
	 * we likely found an improvement to our previous value.
	 */
	if (PS_NUMCORES > 1 && i == PS_NUMCORES) {
		if (inlib_curr && (min_known_qsc > timing_local->time_in)) {
			min_known_qsc = timing_local->time_in;
		}

		assert(min_known_qsc < (unsigned long long)(-1));
		/*
		 * This implies we went through all cores. Thus the
		 * min_known_quie can be used to determine global
		 * quiescence.
		 */
		if (timing_local->last_known_quiescence < min_known_qsc) {
			*qsc = timing_local->last_known_quiescence = min_known_qsc;
		}
		ps_mem_fence();
	}

	return 0;
}

/*
 * Blocking and non-blocking versions of quiescence.  By default, we
 * should only use the non-blocking version (i.e. the system should be
 * wait-free), but we might run out of memory if this is the case.
 */
int
ps_quiesce_wait(struct parsec *p, ps_tsc_t tsc, ps_tsc_t *qsc_tsc)
{ return ps_quiesce(p, tsc, 1, qsc_tsc); }

int
ps_try_quiesce(struct parsec *p, ps_tsc_t tsc, ps_tsc_t *qsc_tsc)
{ return ps_quiesce(p, tsc, 0, qsc_tsc); }

/*
 * We assume that the quiescence queue has at least PS_QLIST_BATCH items
 * in it.
 */
void
__ps_smr_reclaim(coreid_t curr, struct ps_qsc_list *ql, struct ps_smr_info *si,
		 struct ps_mem *m, ps_free_fn_t ffn)
{
	struct parsec    *ps = m->percore[curr].smr_info.ps;
	struct ps_mheader *a = __ps_qsc_peek(ql);
	int increase_backlog = 0, r = 0;
	ps_tsc_t qsc, tsc;
	assert(ps && ql && si);

	if (!a) return ;
	tsc = a->tsc_free;
	if (ps_try_quiesce(ps, tsc, &qsc)) increase_backlog = 1;
	else r = __ps_smr_reclaim_batch(PS_QLIST_BATCH,  qsc, curr, ql, m, ffn);
	si->account.qmemcnt -= r;
	if (r < PS_QLIST_BATCH) increase_backlog = 1;
	if (increase_backlog) si->account.qmemtarget += PS_QLIST_BATCH; /* TODO: shrink target */

	return;

}

void
__ps_timing_info_init(struct parsec *ps, ps_tsc_t now)
{
	int i, j;
	for (i = 0 ; i < PS_NUMCORES ; i++) {
		ps->timing_info[i].timing.last_known_quiescence = now;
		for (j = 0 ; j < PS_NUMCORES ; j++) {
			struct __ps_other_core *o = &ps->timing_info[i].timing_others[j];

			o->time_in = o->time_out = o->time_updated = now;
			o->time_out++;
		}
	}
}

void
__ps_smr_account_init(struct ps_smr_info *si)
{
	si->account.qmemtarget = PS_QLIST_BATCH;
	si->account.qmemcnt    = 0;
}
