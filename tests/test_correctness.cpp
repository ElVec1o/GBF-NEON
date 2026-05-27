/*
 * GBF Correctness Tests (v0.4 — runs against BOTH GBF<2> and GBF<3>)
 *
 * Tests:
 *   1. Zero false negatives for varying key counts
 *   2. False positive rate within expected range
 *   3. Empty filter
 *   4. Single key
 *   5. Exact duplicate keys (should not crash)
 *   6. Multi-threaded build matches single-threaded FP rate
 *   7. 1M keys, multi-thread, zero false negatives
 */

#include "../src/gbf.hpp"
#include "../src/hash.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <cmath>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (cond) { ++g_passed; std::printf("[PASS] %s\n", msg); }              \
        else      { ++g_failed; std::printf("[FAIL] %s\n", msg); }              \
    } while (0)

template <typename Filter>
static std::pair<size_t, size_t> run_test(size_t N, size_t threads,
                                           uint64_t member_seed,
                                           uint64_t nonmember_seed) {
    std::mt19937_64 rng(member_seed);
    std::vector<uint64_t> members(N);
    for (auto& k : members) k = rng();

    std::mt19937_64 rng2(nonmember_seed);
    std::vector<uint64_t> non_members(N);
    for (auto& k : non_members) k = rng2();

    Filter f(members.begin(), members.end(), threads);

    size_t fn = 0, fp = 0;
    for (auto k : members)     fn += f.contains(k) ? 0 : 1;
    for (auto k : non_members) fp += f.contains(k) ? 1 : 0;
    return {fn, fp};
}

template <typename Filter>
static void run_suite(const char* label) {
    std::printf("\n=== Suite: %s ===\n", label);

    // 1. Empty filter
    {
        std::vector<uint64_t> empty;
        Filter f(empty);
        CHECK(!f.contains(42ULL), "Empty filter: no FP on 42");
        CHECK(!f.contains(0ULL),  "Empty filter: no FP on 0");
    }

    // 2. Single key
    {
        std::vector<uint64_t> one = {12345678901234567ULL};
        Filter f(one);
        CHECK(f.contains(12345678901234567ULL), "Single key: member found");
        int fp = 0;
        for (uint64_t k = 1; k <= 1000; ++k) fp += f.contains(k) ? 1 : 0;
        CHECK(fp < 20, "Single key: FP count < 20 out of 1000 non-members");
    }

    // 3. Zero FN — small
    {
        auto [fn, fp] = run_test<Filter>(1000, 1, 0xABCD1111, 0xEFEF2222);
        CHECK(fn == 0, "1k keys, 1 thread: zero FN");
        (void)fp;
    }

    // 4. Zero FN — medium
    {
        auto [fn, fp] = run_test<Filter>(100'000, 1, 0xABCD3333, 0xEFEF4444);
        CHECK(fn == 0, "100k keys, 1 thread: zero FN");
        (void)fp;
    }

    // 5. FP rate within bounds
    {
        auto [fn, fp] = run_test<Filter>(100'000, 1, 0x11112222, 0x33334444);
        double fp_rate = static_cast<double>(fp) / 100'000.0;
        CHECK(fp_rate >= 0.0005 && fp_rate <= 0.030,
              "100k keys: FP rate in [0.05%, 3.0%]");
        std::printf("       observed fp_rate = %.4f%%\n", fp_rate * 100);
    }

    // 6. Multi-thread
    {
        auto [fn, fp] = run_test<Filter>(100'000, 0, 0x55556666, 0x77778888);
        CHECK(fn == 0, "100k keys, multi-thread: zero FN");
        double fp_rate = static_cast<double>(fp) / 100'000.0;
        CHECK(fp_rate >= 0.0005 && fp_rate <= 0.050,
              "100k keys, multi-thread: FP rate reasonable");
        std::printf("       observed fp_rate = %.4f%%\n", fp_rate * 100);
    }

    // 7. Duplicates
    {
        std::vector<uint64_t> dups = {1,2,3,1,2,3,4,4,4,5};
        Filter f(dups, 1);
        bool ok = true;
        for (auto k : dups) ok &= f.contains(k);
        CHECK(ok, "Duplicate keys: all members found");
    }

    // 8. bpk sanity
    {
        std::mt19937_64 rng(0x9999AAAA);
        std::vector<uint64_t> keys(50'000);
        for (auto& k : keys) k = rng();
        Filter f(keys, 1);
        double bpk = f.bits_per_key();
        CHECK(bpk > 5.0 && bpk < 20.0, "50k keys: bits/key in (5, 20)");
        std::printf("       observed bits/key = %.3f\n", bpk);
    }

    // 9. Large
    {
        auto [fn, fp] = run_test<Filter>(1'000'000, 0, 0xDEADC0DE, 0xBEEFF00D);
        CHECK(fn == 0, "1M keys, multi-thread: zero FN");
        double fp_rate = static_cast<double>(fp) / 1'000'000.0;
        std::printf("       observed fp_rate = %.4f%%\n", fp_rate * 100);
    }
}

int main() {
    std::printf("=== GBF Correctness Tests (v0.4) ===\n");

    run_suite<gbf::GaloisBipartiteFilter>("GBF<2> (default)");
    run_suite<gbf::GaloisBipartiteFilter3>("GBF<3>");

    std::printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
