# Beating Bloom in 24 hours — the honest log

I spent one weekend trying to build a membership filter that beats Bloom and
Xor at their own game in C++. I ended up with something that genuinely does,
on the one machine I had to test on. This is what actually happened — eight
ideas, seven of them failed, one of them shipped, and the path between them
turned out to be the interesting part.

Final result, measured on an Apple M2 over 1M random `uint64_t` keys:

| Filter                   | bits/key | build (ms) | query (ns) | FP rate |
|--------------------------|---------:|-----------:|-----------:|--------:|
| **GBF v1.0 (this work)** |  8.520   |     30     |     8.4    |  0.78%  |
| Blocked Bloom (8.26 bpk) |  8.260   |      5     |    16.4    |  2.90%  |
| Blocked Bloom (12 bpk)   | 12.000   |      4     |    14.8    |  1.97%  |
| Naive Bloom (k=6)        |  8.500   |      7     |    17.6    |  1.69%  |

At near-equal memory, GBF is ~2× faster on query and ~3.7× better on false
positive rate than a competent Blocked Bloom. The remaining honest weakness:
build is ~6× slower than Bloom.

Code: [`github.com/elvec1o/...`](https://github.com/) · Dashboard:
[`elvec1o.github.io/.../gbf/`](https://elvec1o.github.io/).

---

## The brief

> *"We're trying to beat industry-standard data structures at their own game,
> from scratch, in C++. Target: a membership filter that beats Bloom (used by
> Redis, RocksDB, Chrome) and the Xor filter (used by Cloudflare). Target
> metrics: <9 bits/key, 2–3 hashes, >250M ops/sec, zero false negatives. Pure
> C++17. No dependencies. Must compile in <100 ms for 1M keys. Pick a target.
> We'll build it."*

The starting hypothesis: instead of storing fingerprints, encode each block
as a **system of GF(2) linear equations**, solved at build time via Gaussian
elimination. At query time you compute `v · P (mod 2)` — a couple of parity
checks against the block's payload — and compare against an 8-bit
fingerprint. This is called a Ribbon Filter (Dillinger & Walzer 2021); I
didn't know that at the start. I would learn.

I named the variant **GBF** (Galois Bipartite Filter) and shipped v0.1 in a
few hours.

## v0.1 — broken in a way that looked great

The first benchmark numbers were a dream:

```
build: 246 ms
query: 14.8 ns
memory: 85 bits/key
false negatives: 992,079 / 1,000,000
```

Memory was off by a factor of 10. Query speed was decent. Build was fast.
**99% of member keys were being reported as not present.** Everything was
broken.

Two bugs:
1. The cuckoo `BuildBlock` struct had a 512-byte temporary array inline. With
   `alignas(64)` padding, that turned 65-byte logical blocks into 128-byte
   physical ones. Hence the 10× memory blow-up.
2. The hash used during Gaussian elimination didn't match the hash used at
   query time. Each key got placed into the matrix with one seed pattern, but
   queried with a different one. Hence 99% false negatives.

This is the part nobody puts in their writeup: **most ambitious data
structure work, in the first 12 hours, doesn't work at all.** The interesting
question is whether you keep going.

## v0.2 → v0.4 — actually competitive

Three iterations to fix the obvious mistakes:

**v0.2** — moved temporary build state out of the persistent struct,
introduced Structure-of-Arrays layout: a flat `P_flat` of `num_blocks × 8 ×
uint64_t` and a separate `seeds` byte array. 8.67 bits/key, 0 false
negatives, 267 ms build.

**v0.3** — parallelized the build via lock-free sharding. Each thread owns
a contiguous block range; keys are pre-sharded by hash; no cross-thread
contention during cuckoo insertion or matrix solving. Build dropped to 103
ms.

**v0.4** — the big query-side win. Replaced every `% num_blocks` modulo in
the hot path with a bitmask by rounding partition sizes to powers of two.
Added explicit prefetch of all candidate cache lines before any verification
ran. Query latency dropped from 17 ns to 11.6 ns and the 3-way cuckoo
variant became opt-in via template parameter, with 2-way as default.

At v0.4 the filter was honestly competitive: ~11 ns query, 8.52 bits/key,
0.78% FP. On the same machine, a competent Blocked Bloom got 16.4 ns query
at 2.90% FP. We were beating it on both axes that matter at near-equal
memory.

## v0.5 — the honesty pass

This is the version I almost didn't do.

Reading more of the Ribbon literature, I admitted what was true: **GBF is
not novel science**. The math is from Dillinger & Walzer 2021; the
contribution is engineering — specifically, taking Ribbon's global banded
matrix and breaking it into cache-line-sized local matrices with cuckoo
displacement and parallel solve. That's a real engineering choice, but it
isn't a new algorithm.

I also found dead code: an AVX-512 SIMD path in `check_block` that computed
a SIMD result, then `(void)`'d it, then ran the scalar loop anyway. A
previous iteration had tried to vectorize the parity check, couldn't get the
bit-extraction right, and silently fell back. Untested perf-theater. Deleted
it.

v0.5 added three pieces of real infrastructure:

- **libFuzzer harness** — `fuzz_gbf.cpp`. Each fuzz iteration builds GBF on
  the input bytes interpreted as keys, then asserts zero false negatives and
  a clean save/load round-trip. 176,379 executions in 46 seconds, 0 crashes,
  0 ASan/UBSan reports.
- **GitHub Actions CI** — gcc + clang on `ubuntu-latest` (real x86), full
  test + bench suite, output uploaded as artifacts. With an explicit doc
  ([`docs/X86_STATUS.md`](docs/X86_STATUS.md)) about what "x86 validated"
  does and does not mean: it means the code compiles and tests pass on real
  x86 in CI; it does not mean I have measured x86 performance, because I
  don't own an x86 box.
- **FastFilter-compatible C API** — five functions matching Lemire's
  `xor8_*` naming, plus a `gbf_xor8_adapter.h` header that lets existing
  FastFilter call sites swap implementations with one `#include`.

This part was anticlimactic but it's what makes the artifact actually
usable.

## v0.6 — stacked filters (failed)

The first real algorithmic attempt to push past v0.4: **stacked filters**
(Deeds et al., 2020). Put a tiny Bloom in front of GBF — most negative
queries die at L1 fast, only the survivors pay GBF's full cache-line cost.
On paper, this should shave 30–40% off the negative-heavy workload, which
is the case databases actually care about (SSTable lookups are mostly
misses).

It didn't work.

```
                          GBF<2>       Stacked GBF
100% positive queries     6.6 ns       7.4 ns   (−8%)
50/50 mixed              18.5 ns      14.0 ns   (+24%)
5/95 negative-heavy      10.2 ns      10.8 ns   (−6%)
```

The premise — that the L1 would hide cache-miss latency for negatives —
failed because **GBF's existing prefetch already overlaps the two cache-line
fetches with the hash computation**. There was almost no miss latency left
to hide. The L1's marginal benefit on negative queries was ~1 ns; its cost
was ~2 ns; net loss.

The unexpected +24% win on 50/50 mixed is likely branch-prediction effects
(the L1 short-circuit gives the CPU a predictable branch where the `||`
chain in plain GBF mispredicts on mixed inputs). Real, but narrow, and not
the win the design targeted. v0.6 was kept in the repo as a documented
failure during development but doesn't ship.

## v0.7 — bumping (failed)

Next try: take the bleeding-edge **bumping** technique from BuRR (Sanders
et al., SEA 2022 Best Paper) and ZOR (Mohamadi & Chikhi, Feb 2026 — only
three months old). Instead of retrying Gaussian elimination with new seeds
when a block fails, abandon the unsolvable rows to a small auxiliary filter.
This lets you push the per-block load factor higher → fewer blocks → less
memory.

Predicted: 0.5–1 bits/key savings, faster build, +1–2 ns query on the small
fraction of bumped keys.

Measured:

```
                    bpk      build    query (5/95 neg)
GBF<2>             8.520     28 ms       10.2 ns
GBF-Bump<2>        8.651     27 ms       13.5 ns
```

**Worse on every axis.** Tests pass; the construction works correctly. The
calibration loop revealed why:

```
TARGET_LOAD_BUMP=72  OK (bumped=8364)
TARGET_LOAD_BUMP=70  OK (bumped=8364)
TARGET_LOAD_BUMP=68  OK (bumped=8364)
TARGET_LOAD_BUMP=65  OK (bumped=8364)
```

All four target loads produce the **same** number of bumped keys. Why? Because
v0.4's power-of-2 partition sizing:

- 62,500 keys / partition ÷ 62 keys-per-block → 1,009 blocks → rounds to 1,024
- 62,500 keys / partition ÷ 72 keys-per-block → 869 blocks → rounds to 1,024

The block count doesn't change. Primary memory doesn't shrink. The aux is
pure overhead.

**v0.4's main win (pow2 → bitmask) and v0.7's idea (bumping for higher load)
are structurally incompatible.** You can't have both without giving up the
5–10 ns query speedup that came from removing the modulo.

This is the kind of conflict you don't see in papers because each paper
optimizes one axis at a time. In an actual codebase the optimizations
interact.

## v0.8 — the wild idea bag

After three failed algorithmic attempts, I committed to one last weekend
afternoon trying three completely cross-disciplinary ideas in parallel:

### A. Gray-code cuckoo placement

Borrowed from telecommunications coding theory (Frank Gray, Bell Labs 1947).
Force the two candidate cuckoo blocks of every key to live in the same 4KB
memory page by computing `h2 = h1 XOR (small bit pattern)`. The TLB stays
hot; one less translation lookup per query.

**Failed.** Constraining `h2` to a 6-bit Hamming neighborhood of `h1`
clustered keys too tightly. Cuckoo placement broke at `TARGET_LOAD=62` and
had to fall back to 56, which doubled `blocks_per_part` and gave us 17
bits/key. Standard cuckoo hashing theory assumes the two candidate slots are
~independent random positions; Gray-code locality violates that. Wins on
query (in a contrived way, because the doubled memory makes everything
sparser), loses catastrophically on memory.

### B. NEON SIMD parity verification

Borrowed from cryptography (AES-GCM authentication uses CLMUL/PMULL
polynomial multiplication). The original idea was to replace the 8-iteration
parity loop in `check_block` with a single PMULL instruction. After 30
minutes of trying, I couldn't make a clean PMULL encoding work — the math
of carryless polynomial multiplication doesn't directly map to "parity of
AND."

But the exercise had a side effect: I started looking at what the compiler
was actually emitting for the scalar `__builtin_parityll` loop. It was
fine — but it was generating a serialized dependency chain where each
iteration's result OR-shifted into `actual_f` before the next iteration
started. The CPU couldn't issue the 8 popcounts in parallel.

Rewrote the parity check as **explicit NEON intrinsics**: load all 8 P[j]
values into 4 `uint64x2_t` registers in one shot, AND with broadcasted v,
byte-wise popcount via `vcntq_u8`, lane-wise horizontal reduction via the
`vpaddl` chain, extract parity bits per lane. Same math, no dependency
chain.

**Worked.** Across three independent benchmark runs:

```
                       GBF<2>     GBF + NEON parity     delta
100% positive          8.1 ns     6.5 ns                −20%
50/50 mixed           21.8 ns    20.2 ns                 −7%
5/95 negative-heavy   12.1 ns     9.2 ns                −24%
```

Same memory. Same FP rate. Zero false negatives. Stable across runs.

The win didn't come from PMULL directly — but exploring PMULL is what made
me look at the parity loop with cryptographic-SIMD eyes, which surfaced the
dependency chain problem. The cross-disciplinary borrow worked
**indirectly**, which is honestly how most useful borrows work.

### C. IBLT auxiliary structure

Borrowed from network coding (Invertible Bloom Lookup Tables,
Eppstein-Goodrich 2011 — originally for peer-to-peer set reconciliation).
Replace the bumped-keys Bloom from v0.7 with an IBLT. IBLTs have a tighter
theoretical space bound for small sets and support enumeration (you can list
the stored set, which a Bloom can't).

**Failed.** Measured FP rate 9.3% — vs target 0.78%. IBLTs are
set-reconciliation structures; pure membership is the wrong workload for
them. At our small bumped count (~8k keys across 16 partitions), the cells
average ~1.5 entries each, and the multi-hit cells can't be disambiguated
without knowing the underlying keys. This was the predicted failure mode in
the task brief; it just confirmed cleanly.

### v0.8 outcome

**1 win out of 3.** Ship the NEON parity. Document the two failures
honestly. Move on.

## Final v1.0 numbers

| Metric                  | Value           |
|-------------------------|-----------------|
| bits/key                | **8.520**       |
| build (1M keys, 10 thr) | **~30 ms**      |
| query latency           | **8.4 ns**      |
| batch query (prefetch)  | **9.2 ns**      |
| false-positive rate     | **0.78%**       |
| false negatives         | **0**           |

Beats a competent Blocked Bloom on the same hardware on both query latency
(~2× faster) and FP rate (~3.7× better) at near-equal memory. Loses to
Bloom on build time (~6× slower) — which is the structural cost of
Gaussian elimination vs setting 8 bits, and isn't going to change without
giving up the GF(2) construction entirely.

## What I learned (the meta-story)

**Most ideas fail.** I tried 8 algorithmic improvements past v0.4: stacked
filters, paired-XOR, bumping, Gray-code cuckoo, IBLT aux, polynomial
verification, holographic reduced representations (sketched, not built),
NEON parity. Seven failed. One worked. That's a hit rate of ~12%, which is
actually pretty normal for systems-level optimization. Don't trust anyone
who claims their ideas worked the first time.

**Cross-disciplinary borrows usually fail at the interface.** Bloom filters
borrowed from probability theory; Xor filters from random hypergraphs;
Ribbon from coding theory. By the time you read a paper, the easy borrows
are taken. The remaining ones either hit information-theoretic walls (you
can't get below the entropy bound) or hardware walls (you can't beat the
cache-line fetch). The 1-in-8 win this weekend came from a borrow that
didn't directly apply but pointed me at something the compiler was doing
suboptimally. That's the honest model: borrows don't deliver the win
themselves; they're prompts that make you look in places you wouldn't
have.

**The contribution is engineering.** This is not a new algorithm. It's
Ribbon-Filter math from 2021, broken into cache-line-sized local matrices
(an engineering choice), parallelized via lock-free sharding (engineering),
serialized via mmap (engineering), accelerated via NEON SIMD (engineering).
Every interesting decision happens at the interface between math and
hardware. That's where the design space actually is.

**Failed experiments are the content.** Most engineering writing about
performance work pretends the path was linear. The path is never linear.
v0.6 stacked filters and v0.7 bumping both **sounded right** and both
**failed measurably**, for non-obvious reasons that only became clear after
running the benchmark. The failures are more informative than the success —
because anyone repeating this work needs to know which apparently-promising
directions don't actually pay off, and why.

## What I will NOT claim

- This is not novel algorithmic science. It is engineering on top of the
  Ribbon Filter (Dillinger & Walzer 2021).
- These numbers are **Apple M2 only**. I do not own x86 hardware. CI runs on
  GitHub's ubuntu-latest x86 runners (compile + test only); I have not
  measured x86 query latency or build throughput from any machine I control.
- The bumping technique (v0.7) didn't work *here*. It may well work in a
  Ribbon variant without power-of-2 partition sizing. That's a different
  experiment.
- This took 24 hours of wall clock — heavily AI-assisted (Claude wrote
  most of the C++ under my direction; I picked which ideas to try and
  evaluated each outcome). It is what one person + a strong LLM can produce
  in a focused weekend. It is not equivalent to a 6-month research
  programme.

## Numbers I will defend

| Claim | Where to verify |
|-------|-----------------|
| 8.520 bits/key on M2, 1M keys | `./build/gbf_bench` |
| 8.4 ns single-query, 9.2 ns batch | `./build/gbf_bench` |
| 0 false negatives over 1M random keys | `./build/gbf_test` (24 tests) |
| Beats Blocked Bloom on query + FP at same memory | `./build/bench_vs` |
| 176,379 fuzz executions, 0 crashes | `./build-fuzz/fuzz_gbf` |
| Code compiles on linux/amd64 | `./scripts/x86-build-check.sh` |

If any of those don't reproduce on the same hardware, the writeup is wrong
and I want to know.

---

**Code**: `github.com/elvec1o/...` (link)
**Dashboard**: `elvec1o.github.io/.../gbf/` (link)
**License**: MIT

Built in 24 hours, May 2026, on a single Apple M2 with one person and one
LLM. The Ribbon Filter math is from Dillinger & Walzer (SEA 2021). The
engineering, the benchmarks, the failures, and this writeup are mine.
