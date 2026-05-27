/*
 * GBF Standalone Benchmark (v0.4)
 *
 * Measures for BOTH GBF<2> and GBF<3>:
 *   - Build time
 *   - Zero false-negative verification
 *   - False positive rate
 *   - bits/key, query throughput
 *   - Batch prefetch query throughput
 *
 * For GBF<2> only: serialize/deserialize timings.
 *
 * Output is parseable by python/bench.py — top-level keys (e.g. `bits_per_key:`)
 * correspond to GBF<2> (the default), to preserve historical schema.
 */

#include "gbf.hpp"
#include "hash.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <vector>

using Clock = std::chrono::high_resolution_clock;

template <typename Filter>
struct Results {
    double build_ms{0};
    double bpk{0};
    size_t mem_bytes{0};
    size_t false_negatives{0};
    size_t false_positives{0};
    double fp_rate{0};
    double query_ns{0};
    size_t blocks{0};
    double batch_query_ns{0};
    size_t batch_false_negatives{0};
};

template <typename Filter>
static Results<Filter> run_variant(const std::vector<uint64_t>& members,
                                    const std::vector<uint64_t>& non_members,
                                    size_t threads) {
    Results<Filter> r;
    size_t N = members.size();

    auto t0 = Clock::now();
    Filter filter(members.begin(), members.end(), threads);
    auto t1 = Clock::now();
    r.build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.bpk      = filter.bits_per_key();
    r.mem_bytes= filter.memory_bytes();
    r.blocks   = filter.block_count();

    for (uint64_t k : members)
        if (!filter.contains(k)) ++r.false_negatives;

    auto t2 = Clock::now();
    for (uint64_t k : non_members)
        r.false_positives += filter.contains(k) ? 1 : 0;
    auto t3 = Clock::now();

    r.query_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / static_cast<double>(N);
    r.fp_rate  = static_cast<double>(r.false_positives) / static_cast<double>(N);

    // Batch
    {
        std::vector<uint64_t> batch_keys;
        batch_keys.reserve(2 * N);
        batch_keys.insert(batch_keys.end(), members.begin(), members.end());
        batch_keys.insert(batch_keys.end(), non_members.begin(), non_members.end());

        std::vector<char> batch_buf(batch_keys.size(), 0);
        bool* bresults = reinterpret_cast<bool*>(batch_buf.data());

        auto tb0 = Clock::now();
        filter.contains_batch(batch_keys.data(), bresults, batch_keys.size());
        auto tb1 = Clock::now();

        double batch_total_ns = std::chrono::duration<double, std::nano>(tb1 - tb0).count();
        r.batch_query_ns = batch_total_ns / static_cast<double>(batch_keys.size());

        for (size_t i = 0; i < N; ++i)
            if (!bresults[i]) ++r.batch_false_negatives;
    }

    return r;
}

template <typename R>
static void print_variant(const char* label, const R& r) {
    std::printf("\n=== %s ===\n", label);
    std::printf("bits_per_key: %.3f\n",       r.bpk);
    std::printf("build_ms: %.2f\n",           r.build_ms);
    std::printf("memory_bytes: %zu\n",        r.mem_bytes);
    std::printf("false_negatives: %zu\n",     r.false_negatives);
    std::printf("false_positives: %zu\n",     r.false_positives);
    std::printf("fp_rate: %.6f\n",            r.fp_rate);
    std::printf("query_ns: %.2f\n",           r.query_ns);
    std::printf("blocks: %zu\n",              r.blocks);
    std::printf("batch_query_ns: %.2f\n",     r.batch_query_ns);
    std::printf("batch_false_negatives: %zu\n", r.batch_false_negatives);
}

int main(int argc, char** argv) {
    size_t N       = 1'000'000;
    size_t threads = 0;

    if (argc >= 2) N       = static_cast<size_t>(std::atoll(argv[1]));
    if (argc >= 3) threads = static_cast<size_t>(std::atoll(argv[2]));

    std::mt19937_64 rng(0xDEADBEEF12345678ULL);
    std::vector<uint64_t> members(N);
    for (auto& k : members) k = rng();
    std::mt19937_64 rng2(0xCAFEBABE87654321ULL);
    std::vector<uint64_t> non_members(N);
    for (auto& k : non_members) k = rng2();

    size_t eff_threads = std::max<size_t>(1,
        threads == 0 ? std::thread::hardware_concurrency() : threads);

    // -----------------------------------------------------------------------
    // GBF<2> (default) — emit legacy top-level keys for python parsers
    // -----------------------------------------------------------------------
    auto r2 = run_variant<gbf::GaloisBipartiteFilter>(members, non_members, threads);

    std::printf("keys: %zu\n",          N);
    std::printf("threads: %zu\n",       eff_threads);
    std::printf("build_ms: %.2f\n",     r2.build_ms);
    std::printf("bits_per_key: %.3f\n", r2.bpk);
    std::printf("memory_bytes: %zu\n",  r2.mem_bytes);
    std::printf("false_negatives: %zu\n", r2.false_negatives);
    std::printf("false_positives: %zu\n", r2.false_positives);
    std::printf("fp_rate: %.6f\n",      r2.fp_rate);
    std::printf("query_ns: %.2f\n",     r2.query_ns);
    std::printf("blocks: %zu\n",        r2.blocks);
    std::printf("batch_query_ns: %.2f\n", r2.batch_query_ns);
    std::printf("batch_false_negatives: %zu\n", r2.batch_false_negatives);

    print_variant("GBF<2> (default)", r2);

    // -----------------------------------------------------------------------
    // GBF<3> (denser)
    // -----------------------------------------------------------------------
    auto r3 = run_variant<gbf::GaloisBipartiteFilter3>(members, non_members, threads);
    print_variant("GBF<3> (denser)", r3);

    // -----------------------------------------------------------------------
    // Serialize / Deserialize (GBF<2> only)
    // -----------------------------------------------------------------------
    {
        gbf::GaloisBipartiteFilter filter(members.begin(), members.end(), threads);
        const std::string tmp_path = "/tmp/gbf_bench.bin";

        auto ts0 = Clock::now();
        bool saved = filter.save(tmp_path);
        auto ts1 = Clock::now();
        double serialize_ms = std::chrono::duration<double, std::milli>(ts1 - ts0).count();

        if (!saved) {
            std::fprintf(stderr, "WARN: save to %s failed\n", tmp_path.c_str());
            std::printf("\nserialize_ms: -1\ndeserialize_ms: -1\nloaded_fn: -1\n");
        } else {
            auto tl0 = Clock::now();
            gbf::GaloisBipartiteFilter loaded = gbf::GaloisBipartiteFilter::load(tmp_path);
            auto tl1 = Clock::now();
            double deserialize_ms = std::chrono::duration<double, std::milli>(tl1 - tl0).count();

            size_t loaded_fn = 0;
            for (uint64_t k : members) if (!loaded.contains(k)) ++loaded_fn;

            std::printf("\nserialize_ms: %.2f\n",   serialize_ms);
            std::printf("deserialize_ms: %.2f\n",   deserialize_ms);
            std::printf("loaded_fn: %zu\n",         loaded_fn);
            std::printf("serialized_bytes: %zu\n",  filter.size_bytes());

            auto tm0 = Clock::now();
            gbf::GaloisBipartiteFilter mapped =
                gbf::GaloisBipartiteFilter::load_mmap(tmp_path);
            auto tm1 = Clock::now();
            double mmap_load_ms =
                std::chrono::duration<double, std::milli>(tm1 - tm0).count();

            size_t mapped_fn = 0;
            for (uint64_t k : members) if (!mapped.contains(k)) ++mapped_fn;
            std::printf("mmap_load_ms: %.4f\n", mmap_load_ms);
            std::printf("mmap_fn: %zu\n",       mapped_fn);
            if (mapped_fn > 0) {
                std::fprintf(stderr, "FATAL: mmap'd filter has %zu false negatives!\n",
                             mapped_fn);
            }
            if (loaded_fn > 0) {
                std::fprintf(stderr, "FATAL: loaded filter has %zu false negatives!\n",
                             loaded_fn);
            }
        }
    }

    if (r2.false_negatives > 0 || r3.false_negatives > 0) {
        std::fprintf(stderr, "FATAL: false negatives detected (GBF<2>=%zu, GBF<3>=%zu)\n",
                     r2.false_negatives, r3.false_negatives);
        return 1;
    }
    return 0;
}
