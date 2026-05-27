#!/usr/bin/env bash
# Build GBF for linux/amd64 and run its correctness tests under qemu.
# NOTE: perf numbers from qemu emulation are meaningless; use GitHub Actions
# (real x86 ubuntu-latest) for that.
set -euo pipefail

docker buildx build --platform linux/amd64 -f Dockerfile.x86 -t gbf-x86 .

echo
echo "=== gbf_test under linux/amd64 (qemu) ==="
docker run --rm --platform linux/amd64 gbf-x86

echo
echo "=== test_c_api under linux/amd64 (qemu) ==="
docker run --rm --platform linux/amd64 gbf-x86 /src/build/test_c_api

echo
echo "x86_64 build + test: PASSED (qemu emulation; perf numbers below are MEANINGLESS)"
echo "=== gbf_bench under qemu (DO NOT QUOTE THESE NUMBERS) ==="
docker run --rm --platform linux/amd64 gbf-x86 /src/build/gbf_bench || true
