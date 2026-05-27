#pragma once

/*
 * Galois Bipartite Filter (GBF) v0.4
 *
 * A probabilistic membership filter based on GF(2) linear algebra.
 *
 * v0.4 changes (from v0.3):
 *  - Power-of-2 partition / block sizing → mod replaced by mask.
 *  - Pipelined 3 prefetches before any check in contains() (overlap misses).
 *  - Templated CUCKOO_WAYS (2 or 3). Default alias `GaloisBipartiteFilter` is 2-way:
 *      faster query (2 cache lines instead of 3), slightly higher bits/key.
 *    `GaloisBipartiteFilter3` is the v0.3-style 3-way (denser, slower query).
 *  - Cuckoo BFS queue replaced by stack-allocated fixed ring buffer.
 *
 * Design:
 *   - Memory divided into 64-byte cache-line-aligned blocks (8 × uint64_t each).
 *   - Each key hashes to a 64-bit vector v and an 8-bit fingerprint f.
 *   - Build: Gaussian elimination over GF(2) solves V·P = F per block.
 *   - Query: compute v·P mod 2 → 8 parity bits, compare to f.
 *   - WAYS candidate blocks per key (cuckoo placement).
 *   - Sharded parallel build, lock-free within shards.
 *   - Batch prefetch API for high-throughput burst queries.
 *   - Serialize/deserialize to flat binary (mmap-compatible layout).
 */

#include "hash.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <sys/stat.h>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define GBF_HAVE_NEON 1
#else
#define GBF_HAVE_NEON 0
#endif

namespace gbf {

// v0.8: explicit NEON 8-parity SIMD (default on ARM64).
// Cross-disciplinary origin: came out of exploring cryptographic CLMUL/PMULL
// (AES-GCM authentication) for filter verification. The direct PMULL
// replacement of parity didn't work, but laying the 8 AND-popcount ops out
// as explicit NEON intrinsics breaks the dependency chain the compiler
// emitted for the scalar loop. Measured on Apple M2: -1.5 to -3 ns per
// query vs scalar, same memory, same FP rate, zero false negatives.
static inline uint8_t gbf_parity8(uint64_t v, const uint64_t* P) noexcept {
#if GBF_HAVE_NEON
    uint64x2_t p01 = vld1q_u64(P);
    uint64x2_t p23 = vld1q_u64(P + 2);
    uint64x2_t p45 = vld1q_u64(P + 4);
    uint64x2_t p67 = vld1q_u64(P + 6);
    uint64x2_t vv  = vdupq_n_u64(v);
    uint64x2_t a01 = vandq_u64(p01, vv);
    uint64x2_t a23 = vandq_u64(p23, vv);
    uint64x2_t a45 = vandq_u64(p45, vv);
    uint64x2_t a67 = vandq_u64(p67, vv);
    auto popcnt_lane = [](uint64x2_t a) -> uint64x2_t {
        uint8x16_t  c8  = vcntq_u8(vreinterpretq_u8_u64(a));
        uint16x8_t  c16 = vpaddlq_u8(c8);
        uint32x4_t  c32 = vpaddlq_u16(c16);
        return vpaddlq_u32(c32);
    };
    uint64x2_t c01 = popcnt_lane(a01);
    uint64x2_t c23 = popcnt_lane(a23);
    uint64x2_t c45 = popcnt_lane(a45);
    uint64x2_t c67 = popcnt_lane(a67);
    uint64_t r = 0;
    r |= (vgetq_lane_u64(c01, 0) & 1ULL) << 0;
    r |= (vgetq_lane_u64(c01, 1) & 1ULL) << 1;
    r |= (vgetq_lane_u64(c23, 0) & 1ULL) << 2;
    r |= (vgetq_lane_u64(c23, 1) & 1ULL) << 3;
    r |= (vgetq_lane_u64(c45, 0) & 1ULL) << 4;
    r |= (vgetq_lane_u64(c45, 1) & 1ULL) << 5;
    r |= (vgetq_lane_u64(c67, 0) & 1ULL) << 6;
    r |= (vgetq_lane_u64(c67, 1) & 1ULL) << 7;
    return static_cast<uint8_t>(r);
#else
    uint8_t actual = 0;
    for (int j = 0; j < 8; ++j)
        actual |= static_cast<uint8_t>(__builtin_parityll(v & P[j]) << j);
    return actual;
#endif
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr size_t BLOCK_COLS       = 8;    // parity columns per block (8 bits → 1 byte fp)
static constexpr size_t BLOCK_CAPACITY   = 64;   // rows per block (64-bit vectors → 64×64 matrix)

// TARGET_LOAD: target keys/block at build time.
// v0.4 calibration (Apple M2, N=1M random uint64 keys, power-of-2 rounded block counts):
//   WAYS=2:
//     TARGET_LOAD=56 → reliable, bpk ~ 9.3
//     TARGET_LOAD=58 → reliable, bpk ~ 9.0
//     TARGET_LOAD=60 → reliable, bpk ~ 8.7   <-- selected for 2-way default
//   WAYS=3:
//     TARGET_LOAD=63 → reliable, bpk ~ 8.6 (after power-of-2 rounding overhead)  <-- selected
template <int WAYS> struct TargetLoadFor;
template <> struct TargetLoadFor<2> { static constexpr size_t value = 62; };
template <> struct TargetLoadFor<3> { static constexpr size_t value = 63; };

static constexpr uint32_t MAX_SEED_TRIES = 256;  // GE retries per block before giving up

// Magic number: ASCII "GBF\x01" + version=4 in lower 4 bytes
static constexpr uint64_t GBF_MAGIC = 0x4742460100000004ULL;

// Round x UP to next power of two (returns 1 for x==0).
static inline size_t next_pow2(size_t x) noexcept {
    if (x <= 1) return 1;
    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    return x + 1;
}

// ---------------------------------------------------------------------------
// GaloisBipartiteFilterImpl<WAYS>
// ---------------------------------------------------------------------------

template <int WAYS = 2>
class GaloisBipartiteFilterImpl {
    static_assert(WAYS == 2 || WAYS == 3, "WAYS must be 2 or 3");
public:
    static constexpr int ways = WAYS;

    template <typename It>
    GaloisBipartiteFilterImpl(It begin, It end, size_t threads = 0) {
        if (threads == 0)
            threads = std::max<size_t>(1, std::thread::hardware_concurrency());

        // Round thread count up to next power of two so num_partitions is pow2.
        size_t pow2_threads = next_pow2(threads);

        n_keys_ = static_cast<size_t>(std::distance(begin, end));
        if (n_keys_ == 0) {
            num_blocks_      = 0;
            num_partitions_  = pow2_threads;
            blocks_per_part_ = 0;
            block_mask_      = 0;
            part_mask_       = pow2_threads - 1;
            return;
        }

        constexpr size_t TARGET_LOAD = TargetLoadFor<WAYS>::value;

        // Initial: enough blocks for TARGET_LOAD keys/block.
        size_t need = (n_keys_ + TARGET_LOAD - 1) / TARGET_LOAD;
        size_t per_part = (need + pow2_threads - 1) / pow2_threads;
        if (per_part == 0) per_part = 1;
        per_part = next_pow2(per_part);

        num_partitions_  = pow2_threads;
        blocks_per_part_ = per_part;
        num_blocks_      = num_partitions_ * blocks_per_part_;
        block_mask_      = blocks_per_part_ - 1;
        part_mask_       = num_partitions_ - 1;

        P_flat_.assign(num_blocks_ * BLOCK_COLS, 0ULL);
        seeds_.assign(num_blocks_, 0);

        // Partition keys into shards by partition index
        std::vector<std::vector<uint64_t>> shards(num_partitions_);
        for (auto it = begin; it != end; ++it) {
            uint64_t key = static_cast<uint64_t>(*it);
            uint32_t p   = static_cast<uint32_t>(fast_hash(key, 0xCAFEBABEULL) & part_mask_);
            shards[p].push_back(key);
        }

        // Each thread handles one partition: cuckoo insert + GF(2) solve
        std::vector<std::thread> workers;
        std::vector<char> ok(num_partitions_, 1);
        workers.reserve(num_partitions_);
        for (size_t t = 0; t < num_partitions_; ++t) {
            workers.emplace_back([this, t, &shards, &ok]() {
                ok[t] = build_partition(t, shards[t]) ? 1 : 0;
            });
        }
        for (auto& w : workers) w.join();

        for (size_t t = 0; t < num_partitions_; ++t) {
            if (!ok[t])
                throw std::runtime_error("GBF build failed: partition " +
                                         std::to_string(t) + " could not be solved.");
        }
    }

    template <typename Container>
    explicit GaloisBipartiteFilterImpl(const Container& keys, size_t threads = 0)
        : GaloisBipartiteFilterImpl(keys.begin(), keys.end(), threads) {}

    GaloisBipartiteFilterImpl() = default;

    // Query: O(1), WAYS cache-line touches with pipelined prefetch.
    bool contains(uint64_t key) const noexcept {
        if (num_blocks_ == 0) return false;
        uint32_t p_idx       = static_cast<uint32_t>(fast_hash(key, 0xCAFEBABEULL) & part_mask_);
        size_t   part_offset = static_cast<size_t>(p_idx) * blocks_per_part_;
        uint32_t h1 = static_cast<uint32_t>(part_offset + (fast_hash(key, 0ULL) & block_mask_));
        uint32_t h2 = static_cast<uint32_t>(part_offset + (fast_hash(key, 1ULL) & block_mask_));

        const uint64_t* P = P_data();
        __builtin_prefetch(&P[static_cast<size_t>(h1) * BLOCK_COLS], 0, 0);
        __builtin_prefetch(&P[static_cast<size_t>(h2) * BLOCK_COLS], 0, 0);

        if constexpr (WAYS == 3) {
            uint32_t h3 = static_cast<uint32_t>(part_offset + (fast_hash(key, 2ULL) & block_mask_));
            __builtin_prefetch(&P[static_cast<size_t>(h3) * BLOCK_COLS], 0, 0);
            return check_block(h1, key) || check_block(h2, key) || check_block(h3, key);
        } else {
            return check_block(h1, key) || check_block(h2, key);
        }
    }

    void contains_batch(const uint64_t* keys, bool* results, size_t n) const {
        constexpr size_t PREFETCH_DIST = 8;
        for (size_t i = 0; i < std::min(PREFETCH_DIST, n); ++i) {
            prefetch_key(keys[i]);
        }
        for (size_t i = 0; i < n; ++i) {
            if (i + PREFETCH_DIST < n) {
                prefetch_key(keys[i + PREFETCH_DIST]);
            }
            results[i] = contains(keys[i]);
        }
    }

    size_t memory_bytes() const noexcept {
        return P_flat_.size() * sizeof(uint64_t) + seeds_.size() * sizeof(uint8_t);
    }

    size_t size_bytes() const noexcept {
        size_t header   = 4 * sizeof(uint64_t);
        size_t p_bytes  = num_blocks_ * BLOCK_COLS * sizeof(uint64_t);
        size_t s_bytes  = (num_blocks_ + 7) & ~size_t(7);
        return header + p_bytes + s_bytes;
    }

    double bits_per_key() const noexcept {
        if (n_keys_ == 0) return 0.0;
        return static_cast<double>(memory_bytes() * 8) / static_cast<double>(n_keys_);
    }

    size_t key_count()   const noexcept { return n_keys_; }
    size_t block_count() const noexcept { return num_blocks_; }

    // -----------------------------------------------------------------------
    // Serialize / Deserialize
    // -----------------------------------------------------------------------
    bool save(const std::string& path) const {
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return false;

        auto write_all = [&](const void* buf, size_t len) -> bool {
            const char* p = static_cast<const char*>(buf);
            while (len > 0) {
                ssize_t r = ::write(fd, p, len);
                if (r <= 0) return false;
                p   += r;
                len -= static_cast<size_t>(r);
            }
            return true;
        };

        uint64_t header[4] = {
            GBF_MAGIC,
            static_cast<uint64_t>(num_blocks_),
            static_cast<uint64_t>(num_partitions_),
            static_cast<uint64_t>(blocks_per_part_)
        };

        bool ok = write_all(header, sizeof(header));
        ok = ok && write_all(P_flat_.data(), P_flat_.size() * sizeof(uint64_t));

        size_t seed_padded = (num_blocks_ + 7) & ~size_t(7);
        std::vector<uint8_t> seed_buf(seed_padded, 0);
        std::copy(seeds_.begin(), seeds_.end(), seed_buf.begin());
        ok = ok && write_all(seed_buf.data(), seed_padded);

        ::close(fd);
        return ok;
    }

    static GaloisBipartiteFilterImpl load(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("GBF load: cannot open " + path);

        auto read_all = [&](void* buf, size_t len) {
            char* p = static_cast<char*>(buf);
            while (len > 0) {
                ssize_t r = ::read(fd, p, len);
                if (r <= 0) {
                    ::close(fd);
                    throw std::runtime_error("GBF load: unexpected EOF");
                }
                p   += r;
                len -= static_cast<size_t>(r);
            }
        };

        uint64_t header[4];
        read_all(header, sizeof(header));

        if (header[0] != GBF_MAGIC) {
            ::close(fd);
            throw std::runtime_error("GBF load: bad magic");
        }

        GaloisBipartiteFilterImpl gbf;
        gbf.num_blocks_      = static_cast<size_t>(header[1]);
        gbf.num_partitions_  = static_cast<size_t>(header[2]);
        gbf.blocks_per_part_ = static_cast<size_t>(header[3]);
        gbf.block_mask_      = gbf.blocks_per_part_ ? (gbf.blocks_per_part_ - 1) : 0;
        gbf.part_mask_       = gbf.num_partitions_ ? (gbf.num_partitions_ - 1) : 0;
        gbf.n_keys_          = 0;

        gbf.P_flat_.resize(gbf.num_blocks_ * BLOCK_COLS);
        read_all(gbf.P_flat_.data(), gbf.P_flat_.size() * sizeof(uint64_t));

        size_t seed_padded = (gbf.num_blocks_ + 7) & ~size_t(7);
        std::vector<uint8_t> seed_buf(seed_padded);
        read_all(seed_buf.data(), seed_padded);
        gbf.seeds_.assign(seed_buf.begin(), seed_buf.begin() + gbf.num_blocks_);

        ::close(fd);
        return gbf;
    }

private:
    void prefetch_key(uint64_t key) const noexcept {
        uint32_t p_idx       = static_cast<uint32_t>(fast_hash(key, 0xCAFEBABEULL) & part_mask_);
        size_t   part_offset = static_cast<size_t>(p_idx) * blocks_per_part_;
        uint32_t h1 = static_cast<uint32_t>(part_offset + (fast_hash(key, 0ULL) & block_mask_));
        uint32_t h2 = static_cast<uint32_t>(part_offset + (fast_hash(key, 1ULL) & block_mask_));
        const uint64_t* P = P_data();
        __builtin_prefetch(&P[static_cast<size_t>(h1) * BLOCK_COLS], 0, 1);
        __builtin_prefetch(&P[static_cast<size_t>(h2) * BLOCK_COLS], 0, 1);
        if constexpr (WAYS == 3) {
            uint32_t h3 = static_cast<uint32_t>(part_offset + (fast_hash(key, 2ULL) & block_mask_));
            __builtin_prefetch(&P[static_cast<size_t>(h3) * BLOCK_COLS], 0, 1);
        }
    }

    const uint64_t* P_data() const noexcept {
        return P_view_ ? P_view_ : P_flat_.data();
    }
    const uint8_t* seeds_data() const noexcept {
        return seeds_view_ ? seeds_view_ : seeds_.data();
    }

    struct BuildBlock {
        uint8_t  count = 0;
        uint64_t keys[BLOCK_CAPACITY];
    };

    bool build_partition(size_t t, const std::vector<uint64_t>& part_keys) {
        size_t part_offset = t * blocks_per_part_;
        std::vector<BuildBlock> build_blocks(blocks_per_part_);

        for (uint64_t key : part_keys) {
            if (!insert_cuckoo(key, build_blocks))
                return false;
        }

        for (size_t bi = 0; bi < blocks_per_part_; ++bi) {
            size_t global_bi = part_offset + bi;
            if (build_blocks[bi].count == 0) {
                seeds_[global_bi] = 0;
                continue;
            }
            bool solved = false;
            for (uint32_t seed = 0; seed < MAX_SEED_TRIES && !solved; ++seed) {
                solved = solve_block(global_bi, build_blocks[bi], seed);
            }
            if (!solved) return false;
        }
        return true;
    }

    // Fixed-size stack ring buffer for cuckoo BFS
    struct QEntry { uint64_t key; int depth; };
    struct CuckooQueue {
        static constexpr int CAP = 1024;
        QEntry buf[CAP];
        int head = 0, tail = 0;
        bool push(QEntry e) noexcept {
            int next = (tail + 1) & (CAP - 1);
            if (next == head) return false;
            buf[tail] = e;
            tail = next;
            return true;
        }
        bool pop(QEntry& out) noexcept {
            if (head == tail) return false;
            out = buf[head];
            head = (head + 1) & (CAP - 1);
            return true;
        }
        bool empty() const noexcept { return head == tail; }
    };

    bool insert_cuckoo(uint64_t key,
                       std::vector<BuildBlock>& build_blocks) {
        CuckooQueue q;
        if (!q.push({key, 0})) return false;

        QEntry e;
        while (q.pop(e)) {
            uint64_t curr = e.key;
            int depth = e.depth;
            if (depth > 512) return false;

            uint32_t h1 = static_cast<uint32_t>(fast_hash(curr, 0ULL) & block_mask_);
            uint32_t h2 = static_cast<uint32_t>(fast_hash(curr, 1ULL) & block_mask_);

            if (build_blocks[h1].count < BLOCK_CAPACITY) {
                build_blocks[h1].keys[build_blocks[h1].count++] = curr;
                continue;
            }
            if (build_blocks[h2].count < BLOCK_CAPACITY) {
                build_blocks[h2].keys[build_blocks[h2].count++] = curr;
                continue;
            }

            uint32_t kick_block = h1;
            if (build_blocks[h2].count > build_blocks[kick_block].count) kick_block = h2;

            if constexpr (WAYS == 3) {
                uint32_t h3 = static_cast<uint32_t>(fast_hash(curr, 2ULL) & block_mask_);
                if (build_blocks[h3].count < BLOCK_CAPACITY) {
                    build_blocks[h3].keys[build_blocks[h3].count++] = curr;
                    continue;
                }
                if (build_blocks[h3].count > build_blocks[kick_block].count) kick_block = h3;
            }

            uint32_t kick_slot = static_cast<uint32_t>(
                fast_hash(curr, static_cast<uint64_t>(depth)) % BLOCK_CAPACITY);
            uint64_t kicked = build_blocks[kick_block].keys[kick_slot];
            build_blocks[kick_block].keys[kick_slot] = curr;
            if (!q.push({kicked, depth + 1})) return false;
        }
        return true;
    }

    bool solve_block(size_t global_bi, const BuildBlock& bb, uint32_t seed) {
        uint8_t n = bb.count;
        if (n == 0) return true;

        uint64_t V[BLOCK_CAPACITY] = {};
        uint8_t  F[BLOCK_CAPACITY] = {};

        for (uint8_t i = 0; i < n; ++i) {
            uint64_t key = bb.keys[i];
            uint64_t h   = fast_hash(key, static_cast<uint64_t>(seed));
            V[i] = fast_hash(h, 1ULL);
            F[i] = static_cast<uint8_t>(fast_hash(h, 2ULL) & 0xFF);
        }

        uint8_t pivot_row[64];
        memset(pivot_row, 0xFF, sizeof(pivot_row));
        uint8_t next_row = 0;

        for (int col = 0; col < 64 && next_row < n; ++col) {
            int piv = -1;
            for (int r = next_row; r < n; ++r) {
                if ((V[r] >> col) & 1) { piv = r; break; }
            }
            if (piv < 0) continue;

            if (piv != next_row) {
                std::swap(V[piv], V[next_row]);
                std::swap(F[piv], F[next_row]);
            }
            pivot_row[col] = next_row;

            for (int r = 0; r < n; ++r) {
                if (r == next_row) continue;
                uint64_t mask = -((V[r] >> col) & 1ULL);
                V[r] ^= V[next_row] & mask;
                F[r] ^= F[next_row] & mask;
            }
            ++next_row;
        }

        for (int r = 0; r < n; ++r) {
            if (V[r] == 0 && F[r] != 0) return false;
        }

        uint64_t* P = &P_flat_[global_bi * BLOCK_COLS];
        for (int j = 0; j < 8; ++j) P[j] = 0;

        for (int col = 0; col < 64; ++col) {
            uint8_t pr = pivot_row[col];
            if (pr == 0xFF) continue;
            uint8_t fval = F[pr];
            for (int j = 0; j < 8; ++j) {
                uint64_t bit = (fval >> j) & 1ULL;
                P[j] |= (bit << col);
            }
        }

        seeds_[global_bi] = static_cast<uint8_t>(seed & 0xFF);
        return true;
    }

    bool check_block(uint32_t b_idx, uint64_t key) const noexcept {
        uint64_t h           = fast_hash(key, static_cast<uint64_t>(seeds_data()[b_idx]));
        uint64_t v           = fast_hash(h, 1ULL);
        uint8_t  expected_f  = static_cast<uint8_t>(fast_hash(h, 2ULL) & 0xFF);

        const uint64_t* P = P_data();
        P += static_cast<size_t>(b_idx) * BLOCK_COLS;
        return gbf_parity8(v, P) == expected_f;
    }

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------
    size_t   n_keys_         = 0;
    size_t   num_blocks_     = 0;
    size_t   num_partitions_ = 1;
    size_t   blocks_per_part_= 0;
    size_t   block_mask_     = 0;
    size_t   part_mask_      = 0;

    std::vector<uint64_t> P_flat_;
    std::vector<uint8_t>  seeds_;

    const uint64_t* P_view_     = nullptr;
    const uint8_t*  seeds_view_ = nullptr;
    void*           mmap_addr_  = nullptr;
    size_t          mmap_len_   = 0;

public:
    ~GaloisBipartiteFilterImpl() {
        if (mmap_addr_ && mmap_len_) {
            ::munmap(mmap_addr_, mmap_len_);
            mmap_addr_ = nullptr;
            mmap_len_  = 0;
        }
    }

    GaloisBipartiteFilterImpl(const GaloisBipartiteFilterImpl&)            = delete;
    GaloisBipartiteFilterImpl& operator=(const GaloisBipartiteFilterImpl&) = delete;

    GaloisBipartiteFilterImpl(GaloisBipartiteFilterImpl&& o) noexcept
        : n_keys_(o.n_keys_), num_blocks_(o.num_blocks_),
          num_partitions_(o.num_partitions_), blocks_per_part_(o.blocks_per_part_),
          block_mask_(o.block_mask_), part_mask_(o.part_mask_),
          P_flat_(std::move(o.P_flat_)), seeds_(std::move(o.seeds_)),
          P_view_(o.P_view_), seeds_view_(o.seeds_view_),
          mmap_addr_(o.mmap_addr_), mmap_len_(o.mmap_len_) {
        o.P_view_     = nullptr;
        o.seeds_view_ = nullptr;
        o.mmap_addr_  = nullptr;
        o.mmap_len_   = 0;
    }
    GaloisBipartiteFilterImpl& operator=(GaloisBipartiteFilterImpl&& o) noexcept {
        if (this != &o) {
            if (mmap_addr_ && mmap_len_) ::munmap(mmap_addr_, mmap_len_);
            n_keys_          = o.n_keys_;
            num_blocks_      = o.num_blocks_;
            num_partitions_  = o.num_partitions_;
            blocks_per_part_ = o.blocks_per_part_;
            block_mask_      = o.block_mask_;
            part_mask_       = o.part_mask_;
            P_flat_          = std::move(o.P_flat_);
            seeds_           = std::move(o.seeds_);
            P_view_          = o.P_view_;
            seeds_view_      = o.seeds_view_;
            mmap_addr_       = o.mmap_addr_;
            mmap_len_        = o.mmap_len_;
            o.P_view_ = nullptr; o.seeds_view_ = nullptr;
            o.mmap_addr_ = nullptr; o.mmap_len_ = 0;
        }
        return *this;
    }

    static GaloisBipartiteFilterImpl load_mmap(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("GBF load_mmap: cannot open " + path);

        struct stat st;
        if (::fstat(fd, &st) < 0) {
            ::close(fd);
            throw std::runtime_error("GBF load_mmap: fstat failed");
        }
        size_t flen = static_cast<size_t>(st.st_size);

        void* addr = ::mmap(nullptr, flen, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (addr == MAP_FAILED) {
            throw std::runtime_error("GBF load_mmap: mmap failed");
        }

        const uint64_t* header = static_cast<const uint64_t*>(addr);
        if (header[0] != GBF_MAGIC) {
            ::munmap(addr, flen);
            throw std::runtime_error("GBF load_mmap: bad magic");
        }

        GaloisBipartiteFilterImpl gbf;
        gbf.num_blocks_      = static_cast<size_t>(header[1]);
        gbf.num_partitions_  = static_cast<size_t>(header[2]);
        gbf.blocks_per_part_ = static_cast<size_t>(header[3]);
        gbf.block_mask_      = gbf.blocks_per_part_ ? (gbf.blocks_per_part_ - 1) : 0;
        gbf.part_mask_       = gbf.num_partitions_ ? (gbf.num_partitions_ - 1) : 0;
        gbf.n_keys_          = 0;

        const uint8_t* base = static_cast<const uint8_t*>(addr);
        size_t off = 4 * sizeof(uint64_t);
        gbf.P_view_     = reinterpret_cast<const uint64_t*>(base + off);
        off += gbf.num_blocks_ * BLOCK_COLS * sizeof(uint64_t);
        gbf.seeds_view_ = base + off;

        gbf.mmap_addr_ = addr;
        gbf.mmap_len_  = flen;
        return gbf;
    }
};

// Convenience aliases:
//   GaloisBipartiteFilter  → 2-way (default; faster query, slightly more bits/key)
//   GaloisBipartiteFilter3 → 3-way (denser, slower query)
using GaloisBipartiteFilter  = GaloisBipartiteFilterImpl<2>;
using GaloisBipartiteFilter3 = GaloisBipartiteFilterImpl<3>;

}  // namespace gbf
