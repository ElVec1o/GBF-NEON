# GBF libFuzzer harness

Coverage-guided fuzzer that checks the GBF invariants:

1. **Zero false negatives** — every inserted key must be found by `contains()`.
2. **No UB / memory errors** — built with `-fsanitize=fuzzer,address,undefined`.
3. **Serialize round-trip** — `save()` then `load()` preserves all members.

It fuzzes both `WAYS=2` and `WAYS=3` for every input.

## Build

Requires a `clang++` with libFuzzer support (Apple clang does NOT ship the
runtime — use `brew install llvm` and point at `/opt/homebrew/opt/llvm/bin/clang++`,
or use system clang on Linux):

```
cmake -S . -B build-fuzz -DGBF_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz -j
```

## Run

```
./build-fuzz/fuzz_gbf -max_total_time=60 fuzz/corpus
```

The `fuzz/corpus/` directory contains a few hand-crafted seeds: sequential keys,
all-zero keys (worst case for duplicate dedup), golden-ratio multiplied keys,
and a longer sequential stream.

## Triage

If the fuzzer finds a crash, libFuzzer drops a `crash-*` file in the current
directory. Reproduce with `./build-fuzz/fuzz_gbf crash-<hash>`.
