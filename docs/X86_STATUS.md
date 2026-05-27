# x86_64 status — what is and is not validated

## Hard truths

- **x86 performance has NOT been validated on real hardware from this repo.**
  All measured numbers in `README.md` (e.g. 11.6 ns query, 8.52 bpk) were
  taken on an Apple M2 (aarch64).
- The author does not currently have a Sapphire Rapids / Zen 4 box to run on.

## What we do have

### 1. GitHub Actions CI on real x86

`.github/workflows/ci.yml` runs the full test + bench suite on
`ubuntu-latest` (which is real x86_64, not emulated) with both gcc and clang,
plus `macos-latest` (Apple Silicon) for parity. The matrix uploads
`gbf_test.out`, `test_c_api.out`, `gbf_bench.out`, `bench_vs.out` as
workflow artifacts on every push / PR. These are the numbers to cite for
"GBF on Linux x86" — not anything from a developer laptop, and not anything
from qemu.

### 2. Local linux/amd64 compile + correctness check via Docker

```
./scripts/x86-build-check.sh
```

Builds `Dockerfile.x86` for `linux/amd64` and runs `gbf_test` + `test_c_api`
under qemu emulation on the developer Mac. This confirms the code *compiles*
and is *correct* on linux/amd64 — nothing more. Any perf numbers reported by
`gbf_bench` inside this container are meaningless because qemu is
instruction-by-instruction emulating x86 on aarch64 silicon.

## AVX-512 status

There is **no** AVX-512 VPOPCNTDQ query path in this repository today. The
earlier v0.3 attempt was removed as part of the honesty pass — the scalar
path on Apple Silicon turned out to be faster than the half-baked AVX-512
code, and we declined to ship code we could not benchmark on real hardware.

A correct AVX-512 VPOPCNTDQ implementation of `contains()` belongs to
whoever has Sapphire Rapids / Zen 4 silicon to verify it against the scalar
path. The CMake check (`HAVE_AVX512VPOPCNTDQ`) is still wired in
`CMakeLists.txt` so a future implementation can opt in without re-plumbing.

## What "x86 validated" does NOT mean here

It does NOT mean:
- We measured 11.6 ns query on Xeon. (We did not.)
- AVX-512 is enabled. (It is not.)
- qemu benchmarks are representative. (They are not — ignore them.)

It DOES mean:
- The code compiles cleanly with both gcc and clang on linux/amd64.
- `gbf_test`'s 24 correctness tests pass on linux/amd64 (per CI).
- The C API test passes on linux/amd64 (per CI).
