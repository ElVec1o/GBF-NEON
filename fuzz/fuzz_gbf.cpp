// libFuzzer harness for the Galois Bipartite Filter.
//
// Invariants checked:
//   1. Zero false negatives: every key inserted must be found by contains().
//   2. Build either succeeds or throws (no UB, no ASan/UBSan errors).
//   3. save() / load() round-trip preserves all members.
//
// Build (requires libFuzzer-capable clang, e.g. brew llvm or clang on Linux):
//   cmake -S . -B build-fuzz -DGBF_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j
// Run:
//   ./build-fuzz/fuzz_gbf -max_total_time=60 fuzz/corpus

#include "../src/gbf.hpp"

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 8) return 0;
    // Cap so fuzzing iterations stay quick. 50k keys is plenty to exercise
    // cuckoo path + GE solve while keeping exec/s reasonable.
    size_t n = std::min(size / 8, size_t(50000));
    if (n == 0) return 0;

    std::vector<uint64_t> keys(n);
    std::memcpy(keys.data(), data, n * sizeof(uint64_t));

    // Deduplicate — otherwise duplicates inflate the cuckoo path artificially
    // and trip TARGET_LOAD on pathological streams.
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    if (keys.empty()) return 0;

    try {
        gbf::GaloisBipartiteFilterImpl<2> f2(keys, /*threads=*/1);
        gbf::GaloisBipartiteFilterImpl<3> f3(keys, /*threads=*/1);

        // Zero-false-negative invariant.
        for (uint64_t k : keys) {
            if (!f2.contains(k)) __builtin_trap();
            if (!f3.contains(k)) __builtin_trap();
        }

        // Serialize/deserialize round-trip on f2.
        const std::string path = "/tmp/gbf_fuzz.bin";
        if (!f2.save(path)) return 0;
        auto loaded = gbf::GaloisBipartiteFilterImpl<2>::load(path);
        for (uint64_t k : keys) {
            if (!loaded.contains(k)) __builtin_trap();
        }
    } catch (const std::exception&) {
        // Build failure on pathological inputs (e.g. cuckoo eviction limit) is
        // a legitimate outcome — we only fail on UB, ASan/UBSan errors, or FN.
    }
    return 0;
}
