#pragma once
#include <cstdint>

// Fast non-cryptographic hash — wyhash-inspired mixing
// Used for ALL hashing in GBF. Never deviate from this in build or query.
inline uint64_t fast_hash(uint64_t x, uint64_t seed) {
    x ^= seed * 0x9e3779b97f4a7c15ULL;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
