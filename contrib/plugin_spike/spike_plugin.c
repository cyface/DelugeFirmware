// The spike kernel. Compiled twice: as a position-independent blob (build.sh) and as the
// in-firmware reference (included by io/debug/plugin_spike.cpp), so the two can be compared
// bit-for-bit and cycle-for-cycle. No globals, no statics, no libc: everything it needs comes
// through the argument list, which is what keeps the blob free of relocations.
#include "spike_plugin.h"

#ifndef SPIKE_ENTRY_ATTR
#define SPIKE_ENTRY_ATTR
#endif

SPIKE_ENTRY_ATTR uint32_t spike_render(const SpikeHostApi* api, SpikeState* s, int32_t* out, uint32_t n,
                                       const SpikeParams* p) {
	uint32_t ph0 = s->phase0;
	uint32_t ph1 = s->phase1;
	int32_t lp = s->lp;
	uint32_t check = 0;
	for (uint32_t i = 0; i < n; i++) {
		ph0 += p->inc0;
		ph1 += p->inc1;
		int32_t saw0 = (int32_t)ph0 >> 1;
		int32_t saw1 = (int32_t)ph1 >> 1;
		int32_t mix = saw0 + saw1;
		/* one-pole lowpass, q31 coefficient, 64-bit intermediate (inlined smull, no libgcc) */
		lp += (int32_t)(((int64_t)(mix - lp) * p->cutoff) >> 31);
		int32_t v = api->mulQ31(lp, p->gain); /* host call per sample */
		out[i] = v;
		check ^= (uint32_t)v + i;
	}
	s->phase0 = ph0;
	s->phase1 = ph1;
	s->lp = lp;
	return check;
}
