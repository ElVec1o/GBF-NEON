/*
 * gbf_xor8_adapter.h — drop-in source-level adapter that lets code written
 * against Daniel Lemire's fastfilter_cpp xor8_* API call GBF instead.
 *
 * Usage:
 *   // replace #include "xorfilter.h" with:
 *   #include "gbf_xor8_adapter.h"
 *   // everything else (xor8_allocate / xor8_populate / xor8_contain /
 *   // xor8_free) keeps compiling and now uses GBF under the hood.
 *
 * Caveats:
 *   - This adapter remaps SYMBOL NAMES only. The GBF struct layout is NOT
 *     binary-compatible with xor8_s; do not mix object files where one TU
 *     saw the real xor8 and another saw GBF.
 *   - Serialized files are NOT cross-compatible with xor8 files.
 *   - GBF currently supports the core 5-call surface
 *     (allocate / populate / contain / free + size_bytes/save/load).
 *     Less common xor8 helpers (e.g. serialization-header inspection) are
 *     not mapped.
 */
#ifndef GBF_XOR8_ADAPTER_H
#define GBF_XOR8_ADAPTER_H

#include "gbf_c.h"

#define xor8_t                gbf_filter_t
#define xor8_allocate         gbf_filter_allocate
#define xor8_populate         gbf_filter_populate
#define xor8_contain          gbf_filter_contain
#define xor8_free             gbf_filter_free
#define xor8_size_in_bytes    gbf_filter_size_bytes
#define xor8_serialize        gbf_filter_save
#define xor8_deserialize      gbf_filter_load

#endif /* GBF_XOR8_ADAPTER_H */
