// Plugin ABI for the #37 SDRAM plugin spike. Shared verbatim by the standalone blob build
// (contrib/plugin_spike/build.sh) and the in-firmware reference (io/debug/plugin_spike.cpp).
#pragma once
#include <stdint.h>

#define DLP_MAGIC 0x31504C44u /* "DLP1" little-endian */
#define DLP_ABI 1u
#define DLP_ENTRY_OFFSET 16u /* header is 16 bytes; entry code follows immediately */

typedef struct {
	uint32_t magic;
	uint32_t abi;
	uint32_t entryOffset; /* byte offset of the entry function from the start of the blob (Thumb: caller sets bit 0) */
	uint32_t reserved;
} DlpHeader;

/* Services the host hands to the plugin. The plugin has no imports: every call into the firmware
 * goes through this table, so the blob needs no relocations and no symbol resolution. */
typedef struct {
	int32_t (*mulQ31)(int32_t a, int32_t b); /* (a*b)>>31, implemented by the host; proves SDRAM->internal calls */
} SpikeHostApi;

typedef struct {
	uint32_t phase0;
	uint32_t phase1;
	int32_t lp;
} SpikeState;

typedef struct {
	uint32_t inc0;
	uint32_t inc1;
	int32_t cutoff; /* q31 one-pole coefficient */
	int32_t gain;   /* q31 */
} SpikeParams;

/* Renders n samples of a two-saw + one-pole-lowpass voice into out, returns an xor checksum of the output. */
typedef uint32_t (*SpikeRenderFn)(const SpikeHostApi* api, SpikeState* s, int32_t* out, uint32_t n,
                                  const SpikeParams* p);
