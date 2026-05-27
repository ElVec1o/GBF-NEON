/*
 * GBF C API — FastFilter-style interface.
 *
 * Mirrors the shape (allocate / populate / contain / free + serialize) of
 * Daniel Lemire's fastfilter_cpp xor8_* API so existing users can swap GBF
 * in via the adapter header `gbf_xor8_adapter.h`.
 *
 * Symbol naming intentionally uses the `gbf_filter_*` prefix (NOT `xor8_*`)
 * so we are not pretending to BE xor8 — we are SOURCE-compatible with it via
 * the adapter macros. Names diverge on purpose; the adapter bridges them.
 *
 * Underlying implementation: gbf::GaloisBipartiteFilterImpl<2> (the 2-way
 * default — faster query, ~8.5 bpk).
 */
#ifndef GBF_C_H
#define GBF_C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gbf_filter_s {
    void* opaque;  /* points to gbf::GaloisBipartiteFilterImpl<2> on the heap */
} gbf_filter_t;

/* Mark the filter as empty / not-yet-built. Does NOT allocate the internal
 * arrays — those are sized during gbf_filter_populate() from the actual key
 * count (the filter is statically sized at build time, not incrementally).
 * Returns true on success.
 */
bool gbf_filter_allocate(uint32_t n_keys, gbf_filter_t* f);

/* Build the filter from `keys` (size = n_keys). Replaces any prior contents
 * of `f`. Returns false on build failure (e.g. cuckoo placement exhaustion
 * on pathological inputs).
 */
bool gbf_filter_populate(const uint64_t* keys, uint32_t n_keys, gbf_filter_t* f);

/* Membership query. Returns true if `key` was in the populated set, or a
 * probabilistic false positive.
 */
bool gbf_filter_contain(uint64_t key, const gbf_filter_t* f);

/* Free internal storage. Leaves `f->opaque = NULL`. Safe to call on a
 * default-zeroed gbf_filter_t.
 */
void gbf_filter_free(gbf_filter_t* f);

/* Bytes of in-memory storage owned by the filter (not including the
 * gbf_filter_t struct itself).
 */
size_t gbf_filter_size_bytes(const gbf_filter_t* f);

/* Serialize to `path`. Returns true on success. */
bool gbf_filter_save(const gbf_filter_t* f, const char* path);

/* Load from `path` into `f`, replacing any prior contents. Returns true
 * on success.
 */
bool gbf_filter_load(gbf_filter_t* f, const char* path);

#ifdef __cplusplus
}
#endif
#endif /* GBF_C_H */
