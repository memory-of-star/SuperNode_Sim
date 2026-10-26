#!/usr/bin/env bash
# S5b endpoint MTP equivalence (DESIGN.md §11 S5b / §12.20): drive the full SUE
# network with SueLiteEndpoint (self-simulated traffic generator on every host,
# ring shape by default), run once on the single-threaded baseline
# (DefaultSimulatorImpl) and once on the single-process multithreaded simulator
# (NS3_MTP, auto-partition, ENDPOINT_THREADS threads), then diff the aggregate result
# hash. Identical hashes => the endpoint's TX (ScheduleWithContext first frame,
# self-chained Schedule thereafter) and RX (channel-seam upcall) are LP-safe and
# deterministic across thread counts.
#
# Both runs must self-report PASS (every-frame-delivered lossless + per-VC /
# per-destination attribution), and their ENDPOINT HASH strings must be byte-identical.
# S5B_DITHER=0 (exact 1000us start multiples) disables the per-host dither and is
# NOT expected to hash-equal across thread counts — use it only for debugging the
# single-threaded run.
#
# Usage:
#   ./run-s5b.sh [N]   # N = MTP thread count (default 8)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM="$HERE/ns-3-alibabacloud/simulation"
BUILD="$SIM/build"
if [ ! -d "$BUILD" ]; then
  echo "run-s5b.sh: build dir $BUILD missing — run ./build.sh first" >&2
  exit 2
fi
if ! BUILD_TYPE=$(grep '^CMAKE_BUILD_TYPE:' "$BUILD/CMakeCache.txt" | cut -d= -f2 | tr -d '[:space:]'); then
  BUILD_TYPE=default
fi
[ -z "$BUILD_TYPE" ] && BUILD_TYPE=default
BIN="$BUILD/scratch/ns3.36.1-sue-endpoint-ring-$BUILD_TYPE"
N="${1:-8}"
case "$N" in
  ''|*[!0-9]*) echo "run-s5b.sh: invalid thread count '$N' (integer expected)" >&2; exit 2 ;;
  *) ;;
esac

cmake --build "$BUILD" --target scratch_sue-endpoint-ring -j 8

out1=$(timeout 300 env ENDPOINT_THREADS=1    "$BIN") || { echo "run-s5b.sh: baseline (threads=1) CRASHED/timeout (exit $?)" >&2; exit 2; }
out2=$(timeout 300 env ENDPOINT_THREADS="$N" "$BIN") || { echo "run-s5b.sh: mtp (threads=$N) CRASHED/timeout (exit $?)" >&2; exit 2; }
h1=$(echo "$out1" | grep '^ENDPOINT HASH') || { echo "run-s5b.sh: baseline output missing ENDPOINT HASH line" >&2; exit 2; }
h2=$(echo "$out2" | grep '^ENDPOINT HASH') || { echo "run-s5b.sh: mtp output missing ENDPOINT HASH line" >&2; exit 2; }
p1=$(echo "$out1" | grep -o 'ENDPOINT \(PASS\|FAIL\)' | head -1)
p2=$(echo "$out2" | grep -o 'ENDPOINT \(PASS\|FAIL\)' | head -1)
echo "baseline (threads=1) : $h1  [$p1]"
echo "mtp     (threads=$N) : $h2  [$p2]"
if [ "$h1" = "$h2" ] && [ "$p1" = "ENDPOINT PASS" ] && [ "$p2" = "ENDPOINT PASS" ]; then
  echo "ENDPOINT MTP EQUIVALENCE PASS"
  exit 0
fi
echo "ENDPOINT MTP EQUIVALENCE FAIL"
exit 1
