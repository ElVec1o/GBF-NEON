/*
 * Head-to-head benchmark on the SAME machine, SAME workload:
 *   - GBF v0.3
 *   - Blocked Bloom (competent: 64-byte block + 8 bits per block, one cache line/query)
 *   - Naive Bloom  (k scattered murmur-style hashes; strawman, here for context only)
 *
 * Workload: 1M random uint64 keys; query 1M positives + 1M negatives.
 * Reports a single comparison table.
 */

#include "gbf.hpp"
#include "hash.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Blocked Bloom Filter — cache-line aligned, 8 bits per block, derived hashes.
// Layout: array of 64-byte blocks; each block has 8 * uint64_t = 512 bits.
// One primary hash picks the block; a secondary hash, combined via Kirsch-
// Mitzenmacher double-hashing, produces 8 in-block bit positions (0..511).
// Each query/insert touches exactly ONE cache line.
// ---------------------------------------------------------------------------
class BlockedBloom {
public:
    BlockedBloom(size_t n, double bpk) {
        size_t total_bits = static_cast<size_t>(static_cast<double>(n) * bpk);
        size_t total_64   = (total_bits + 63) / 64;
        // Round up to multiple of 8 (one block = 8 × uint64)
        total_64 = ((total_64 + 7) / 8) * 8;
        num_blocks_ = total_64 / 8;
        if (num_blocks_ == 0) num_blocks_ = 1;
        bits_.assign(num_blocks_ * 8, 0ULL);
    }

    inline void insert(uint64_t key) noexcept {
        uint64_t h1 = fast_hash(key, 0xB10C00DEULL);
        uint64_t h2 = fast_hash(key, 0xB10C00DFULL);
        uint64_t* blk = &bits_[(h1 % num_blocks_) * 8];
        for (int i = 0; i < 8; ++i) {
            uint64_t hi = h1 + static_cast<uint64_t>(i) * h2;
            uint32_t pos = static_cast<uint32_t>(hi & 511);  // 0..511 within block
            blk[pos >> 6] |= (1ULL << (pos & 63));
        }
    }

    inline bool contains(uint64_t key) const noexcept {
        uint64_t h1 = fast_hash(key, 0xB10C00DEULL);
        uint64_t h2 = fast_hash(key, 0xB10C00DFULL);
        const uint64_t* blk = &bits_[(h1 % num_blocks_) * 8];
        for (int i = 0; i < 8; ++i) {
            uint64_t hi = h1 + static_cast<uint64_t>(i) * h2;
            uint32_t pos = static_cast<uint32_t>(hi & 511);
            if ((blk[pos >> 6] & (1ULL << (pos & 63))) == 0) return false;
        }
        return true;
    }

    double bits_per_key(size_t n) const noexcept {
        return static_cast<double>(bits_.size() * 64) / static_cast<double>(n);
    }
    size_t bytes() const noexcept { return bits_.size() * 8; }

private:
    size_t num_blocks_;
    std::vector<uint64_t> bits_;
};

// ---------------------------------------------------------------------------
// Naive Bloom (for context — strawman): k independent hashes scattered across
// the whole bit array. Same as the existing test_vs_bloom.cpp.
// ---------------------------------------------------------------------------
class NaiveBloom {
public:
    NaiveBloom(size_t n, double bpk, int k) : k_(k) {
        size_t m = static_cast<size_t>(static_cast<double>(n) * bpk);
        m = (m + 63) & ~size_t(63);
        bits_.assign(m / 64, 0ULL);
        m_bits_ = m;
    }
    inline void insert(uint64_t key) noexcept {
        for (int i = 0; i < k_; ++i) {
            uint64_t h = fast_hash(key, static_cast<uint64_t>(i));
            uint64_t pos = h % m_bits_;
            bits_[pos >> 6] |= (1ULL << (pos & 63));
        }
    }
    inline bool contains(uint64_t key) const noexcept {
        for (int i = 0; i < k_; ++i) {
            uint64_t h = fast_hash(key, static_cast<uint64_t>(i));
            uint64_t pos = h % m_bits_;
            if ((bits_[pos >> 6] & (1ULL << (pos & 63))) == 0) return false;
        }
        return true;
    }
    double bits_per_key(size_t n) const noexcept {
        return static_cast<double>(m_bits_) / static_cast<double>(n);
    }
private:
    int k_;
    size_t m_bits_;
    std::vector<uint64_t> bits_;
};

// ---------------------------------------------------------------------------

struct Row {
    const char* name;
    double bpk;
    double build_ms;
    double query_ns;
    double fp_rate;
    size_t fn;
};

static void print_header() {
    std::printf("\n%-22s %9s %10s %10s %8s\n",
                "Filter", "bits/key", "build_ms", "query_ns", "fp_rate");
    std::printf("%-22s %9s %10s %10s %8s\n",
                "----------------------", "--------", "--------", "--------", "-------");
}
static void print_row(const Row& r) {
    std::printf("%-22s %9.3f %10.1f %10.2f %7.3f%%\n",
                r.name, r.bpk, r.build_ms, r.query_ns, r.fp_rate * 100.0);
}

int main(int argc, char** argv) {
    size_t N = 1'000'000;
    if (argc >= 2) N = static_cast<size_t>(std::atoll(argv[1]));

    std::mt19937_64 rng(0xC0FFEE12345ULL);
    std::vector<uint64_t> members(N), non_members(N);
    for (auto& k : members)     k = rng();
    for (auto& k : non_members) k = rng();

    // -------- GBF<2> v0.4 (default) --------
    Row gbf_r{"GBF<2> v0.4", 0, 0, 0, 0, 0};
    {
        auto t0 = Clock::now();
        gbf::GaloisBipartiteFilter f(members);
        auto t1 = Clock::now();
        gbf_r.build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        gbf_r.bpk      = f.bits_per_key();

        for (auto k : members) gbf_r.fn += f.contains(k) ? 0 : 1;

        size_t fp = 0;
        auto t2 = Clock::now();
        for (auto k : non_members) fp += f.contains(k) ? 1 : 0;
        auto t3 = Clock::now();
        gbf_r.fp_rate  = static_cast<double>(fp) / static_cast<double>(N);
        gbf_r.query_ns = std::chrono::duration<double, std::nano>(t3 - t2).count()
                         / static_cast<double>(N);
    }

    // -------- GBF<3> v0.4 (denser) --------
    Row gbf3_r{"GBF<3> v0.4", 0, 0, 0, 0, 0};
    {
        auto t0 = Clock::now();
        gbf::GaloisBipartiteFilter3 f(members);
        auto t1 = Clock::now();
        gbf3_r.build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        gbf3_r.bpk      = f.bits_per_key();

        for (auto k : members) gbf3_r.fn += f.contains(k) ? 0 : 1;

        size_t fp = 0;
        auto t2 = Clock::now();
        for (auto k : non_members) fp += f.contains(k) ? 1 : 0;
        auto t3 = Clock::now();
        gbf3_r.fp_rate  = static_cast<double>(fp) / static_cast<double>(N);
        gbf3_r.query_ns = std::chrono::duration<double, std::nano>(t3 - t2).count()
                          / static_cast<double>(N);
    }

    // -------- Blocked Bloom at matched memory (~8.26 bits/key) --------
    Row bb_match{"Blocked Bloom (8.26)", 0, 0, 0, 0, 0};
    {
        BlockedBloom bf(N, 8.26);
        auto t0 = Clock::now();
        for (auto k : members) bf.insert(k);
        auto t1 = Clock::now();
        bb_match.build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        bb_match.bpk      = bf.bits_per_key(N);

        for (auto k : members) bb_match.fn += bf.contains(k) ? 0 : 1;

        size_t fp = 0;
        auto t2 = Clock::now();
        for (auto k : non_members) fp += bf.contains(k) ? 1 : 0;
        auto t3 = Clock::now();
        bb_match.fp_rate  = static_cast<double>(fp) / static_cast<double>(N);
        bb_match.query_ns = std::chrono::duration<double, std::nano>(t3 - t2).count()
                            / static_cast<double>(N);
    }

    // -------- Blocked Bloom at "standard" 12 bits/key --------
    Row bb_12{"Blocked Bloom (12.0)", 0, 0, 0, 0, 0};
    {
        BlockedBloom bf(N, 12.0);
        auto t0 = Clock::now();
        for (auto k : members) bf.insert(k);
        auto t1 = Clock::now();
        bb_12.build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        bb_12.bpk      = bf.bits_per_key(N);

        for (auto k : members) bb_12.fn += bf.contains(k) ? 0 : 1;

        size_t fp = 0;
        auto t2 = Clock::now();
        for (auto k : non_members) fp += bf.contains(k) ? 1 : 0;
        auto t3 = Clock::now();
        bb_12.fp_rate  = static_cast<double>(fp) / static_cast<double>(N);
        bb_12.query_ns = std::chrono::duration<double, std::nano>(t3 - t2).count()
                         / static_cast<double>(N);
    }

    // -------- Naive Bloom (strawman, k=6 at 8.5 bpk) --------
    Row nb_r{"Naive Bloom (8.5,k=6)", 0, 0, 0, 0, 0};
    {
        NaiveBloom bf(N, 8.5, 6);
        auto t0 = Clock::now();
        for (auto k : members) bf.insert(k);
        auto t1 = Clock::now();
        nb_r.build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        nb_r.bpk      = bf.bits_per_key(N);

        for (auto k : members) nb_r.fn += bf.contains(k) ? 0 : 1;

        size_t fp = 0;
        auto t2 = Clock::now();
        for (auto k : non_members) fp += bf.contains(k) ? 1 : 0;
        auto t3 = Clock::now();
        nb_r.fp_rate  = static_cast<double>(fp) / static_cast<double>(N);
        nb_r.query_ns = std::chrono::duration<double, std::nano>(t3 - t2).count()
                        / static_cast<double>(N);
    }

    std::printf("=== Head-to-head (N = %zu, negatives query path) ===\n", N);
    print_header();
    print_row(gbf_r);
    print_row(gbf3_r);
    print_row(bb_match);
    print_row(bb_12);
    print_row(nb_r);
    std::printf("\nFalse-negative counts (must all be 0): GBF2=%zu GBF3=%zu BB8.26=%zu BB12=%zu NB=%zu\n",
                gbf_r.fn, gbf3_r.fn, bb_match.fn, bb_12.fn, nb_r.fn);

    if (gbf_r.fn || gbf3_r.fn || bb_match.fn || bb_12.fn || nb_r.fn) return 1;
    return 0;
}
