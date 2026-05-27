/* C test for the GBF FastFilter-style C API.
 * Built and linked as pure C, against the gbf_c static library.
 */
#include "../src/gbf_c.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(void) {
    const uint32_t N = 10000;
    uint64_t* keys = (uint64_t*)malloc(N * sizeof(uint64_t));
    if (!keys) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* Golden-ratio-derived key stream — well distributed. */
    for (uint32_t i = 0; i < N; ++i) {
        keys[i] = (uint64_t)i * 2654435761ULL;
    }

    gbf_filter_t f = {0};
    if (!gbf_filter_allocate(N, &f)) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    if (!gbf_filter_populate(keys, N, &f)) {
        fprintf(stderr, "populate failed\n");
        return 1;
    }

    int fn = 0;
    for (uint32_t i = 0; i < N; ++i) {
        if (!gbf_filter_contain(keys[i], &f)) ++fn;
    }
    if (fn != 0) {
        fprintf(stderr, "false negatives: %d\n", fn);
        return 1;
    }

    int fp = 0;
    for (uint32_t i = 0; i < N; ++i) {
        uint64_t k = (uint64_t)(N + i) * 0xdeadbeefULL;
        if (gbf_filter_contain(k, &f)) ++fp;
    }
    size_t mem = gbf_filter_size_bytes(&f);

    printf("C API: N=%u, false_negatives=0, false_positives=%d (fp_rate=%.3f%%), bytes=%zu, bpk=%.3f\n",
           N, fp, 100.0 * (double)fp / (double)N,
           mem, (double)(mem * 8) / (double)N);

    /* Round-trip save/load. */
    const char* path = "/tmp/gbf_c_api.bin";
    if (!gbf_filter_save(&f, path)) {
        fprintf(stderr, "save failed\n"); return 1;
    }
    gbf_filter_t g = {0};
    if (!gbf_filter_load(&g, path)) {
        fprintf(stderr, "load failed\n"); return 1;
    }
    int fn2 = 0;
    for (uint32_t i = 0; i < N; ++i) {
        if (!gbf_filter_contain(keys[i], &g)) ++fn2;
    }
    if (fn2 != 0) { fprintf(stderr, "post-load FN: %d\n", fn2); return 1; }
    printf("C API save/load round-trip: OK\n");

    gbf_filter_free(&g);
    gbf_filter_free(&f);
    free(keys);
    return 0;
}
