# Galois Bipartite Filter (GBF)

[![CI](https://github.com/ElVec1o/GBF-NEON/actions/workflows/ci.yml/badge.svg)](https://github.com/ElVec1o/GBF-NEON/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A C++17 approximate set-membership filter. Variant of the Ribbon Filter
(Dillinger & Walzer 2021); the contribution here is engineering — sharded
parallel build, batch prefetch, mmap zero-copy load, FastFilter-compatible
C API, and an explicit NEON SIMD parity verification path that wins ~25%
query latency on Apple Silicon.

For the full design journey (8 ideas tried, 7 failed, 1 shipped), see
[`ARTICLE.md`](ARTICLE.md).

## Measured numbers — Apple M2, 1M random uint64 keys, 10 threads

| Metric                  | Value           |
|-------------------------|-----------------|
| bits/key                | **8.520**       |
| build (1M keys)         | **~30 ms**      |
| query latency           | **8.4 ns**      |
| batch query (prefetch)  | **9.2 ns**      |
| false-positive rate     | **0.78%**       |
| false negatives         | **0**           |
| mmap zero-copy load     | **0.03 ms**     |

## Head-to-head vs competent Blocked Bloom (`./build/bench_vs`)

Measured on the same M2, same workload:

| Filter                   | bits/key | build (ms) | query (ns) | FP rate |
|--------------------------|---------:|-----------:|-----------:|--------:|
| **GBF v1.0**             |  8.520   |    30      |    8.4     |  0.78%  |
| Blocked Bloom (8.26 bpk) |  8.260   |     5      |   16.4     |  2.90%  |
| Blocked Bloom (12 bpk)   | 12.000   |     4      |   14.8     |  1.97%  |
| Naive Bloom (k=6)        |  8.500   |     7      |   17.6     |  1.69%  |

At near-equal memory, **GBF wins on query latency (~2× faster) AND FP rate
(~3.7× better)** vs a competent cache-line-aligned Blocked Bloom. The
remaining honest weakness: build time is ~6× slower than Bloom.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Requires: C++17, CMake ≥ 3.16, POSIX (`mmap`). On ARM64 the NEON parity path
is enabled automatically; on x86 the build falls back to a scalar parity
loop (AVX-512 GFNI path is not currently shipped — see [`docs/X86_STATUS.md`](docs/X86_STATUS.md)).

## Run

```bash
./build/gbf_bench       # standalone benchmark (numbers above)
./build/gbf_test        # 24 correctness tests
./build/bench_vs        # head-to-head vs Blocked Bloom
./build/test_c_api      # C API round-trip
```

Live benchmark dashboard:

```bash
python3 dashboard/launch.py    # opens http://localhost:8731/dashboard/index.html
```

## C++ API

Single header. Build from any range of `uint64_t` keys, query in O(1).

```cpp
#include "gbf.hpp"
#include <vector>

std::vector<uint64_t> keys = { /* ... */ };

gbf::GaloisBipartiteFilter f(keys);     // build (parallel, ~30 ms for 1M keys)

if (f.contains(some_key)) { /* maybe present, FP rate 0.78% */ }

// Batch with software prefetch (best for burst workloads)
std::vector<uint64_t> queries = { /* ... */ };
std::vector<bool> results(queries.size());
f.contains_batch(queries.data(), results.data(), queries.size());

// Serialize / mmap zero-copy load
f.save("/path/to/filter.bin");
auto loaded = gbf::GaloisBipartiteFilter::load_mmap("/path/to/filter.bin");
```

`GaloisBipartiteFilter` is `gbf::GaloisBipartiteFilterImpl<2>` (2-way cuckoo,
default). `GaloisBipartiteFilterImpl<3>` is a 3-way variant — denser packing
but slower query.

## C API (FastFilter-compatible)

```c
#include "gbf_c.h"

gbf_filter_t f = {0};
gbf_filter_allocate(n_keys, &f);
gbf_filter_populate(keys, n_keys, &f);
bool present = gbf_filter_contain(query, &f);
gbf_filter_free(&f);
```

To drop into existing FastFilter-style call sites, include the adapter:

```c
#include "gbf_xor8_adapter.h"   // re-#defines xor8_* → gbf_filter_*
```

## Fuzzing

```bash
cmake -S . -B build-fuzz -DGBF_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz -j
./build-fuzz/fuzz_gbf -max_total_time=60 fuzz/corpus
```

Latest run: 176,379 executions in 46 s, 0 crashes (Apple M2 with
libFuzzer + ASan + UBSan).

## CI / x86

GitHub Actions matrix (gcc + clang on ubuntu-latest) in
[`.github/workflows/ci.yml`](.github/workflows/ci.yml). Real x86 perf
numbers live in CI artifacts — the headline numbers in this README are
Apple-M2-only. See [`docs/X86_STATUS.md`](docs/X86_STATUS.md) for what
"x86 validated" does and does not mean here.

## Limitations

- **Not a novel algorithm.** GBF is a Ribbon-Filter variant
  (Dillinger & Walzer 2021). The engineering — sharded parallel build,
  2-way cuckoo with power-of-2 partition masking, batch prefetch query,
  NEON parity verification, mmap zero-copy load — is what's shipped here.
- **Static only.** No `insert` / `erase` after build.
- **No AVX-512 query path** is shipped. The scalar fallback exists, and
  CMake detects AVX-512 VPOPCNTDQ but does not enable a SIMD path until
  someone with Sapphire Rapids / Zen 4 hardware can verify a correct
  implementation bit-for-bit.
- **Pow2 partition sizing** can over-allocate by up to ~30% at adversarial
  N (typically near a power-of-2 boundary). At N=1M the overhead is ~3%.

## License

MIT — see [`LICENSE`](LICENSE).

## References

- Dillinger, P.C. & Walzer, S. *Ribbon filter: practically smaller than
  Bloom and Xor.* SEA 2021.
- Graf, T.M. & Lemire, D. *Binary Fuse Filters.* ACM JEA 2022.
- Dietzfelbinger, M. et al. *Fast Succinct Retrieval and Approximate
  Membership Using Ribbon (BuRR).* SEA 2022 (Best Paper).
- Mohamadi, H. & Chikhi, R. *ZOR filters.* arXiv:2602.03525, 2026.
