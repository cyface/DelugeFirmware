// Blob-only translation unit: the 16-byte header lands at offset 0, the entry function at offset 16.
#include "spike_plugin.h"

#define SPIKE_ENTRY_ATTR __attribute__((section(".text.entry"), used))
#include "spike_plugin.c"

__attribute__((section(".dlp_header"), used)) const DlpHeader dlp_header = {DLP_MAGIC, DLP_ABI, DLP_ENTRY_OFFSET, 0};
