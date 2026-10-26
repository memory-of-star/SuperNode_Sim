#!/usr/bin/env bash
# S5c GPU-to-GPU throughput measurement: run the clean-link and the 1/10000-loss
# scenarios back to back and print both THPT RESULT lines side by side.
#
# Usage:
#   ./run-s5c-thpt.sh [RATE] [GBITS] [LOSS_EVERY] [CREDITS]
#     RATE       link rate (default 400Gbps)   GBITS  payload (default 10)
#     LOSS_EVERY 0 = clean only, else run clean + that 1/N loss (default 10000)
#     CREDITS    CBFC credit limit per VC (default 1024 = canonical Task B matrix;
#                <1024 is the binding constraint — 512 credits gives 91.75% of line
#                rate; pass e.g. 2048 to scan the credit window)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM="$HERE/ns-3-alibabacloud/simulation"
BUILD="$SIM/build"
if [ ! -d "$BUILD" ]; then
  echo "run-s5c-thpt.sh: build dir $BUILD missing — run ./build.sh first" >&2
  exit 2
fi
if ! BUILD_TYPE=$(grep '^CMAKE_BUILD_TYPE:' "$BUILD/CMakeCache.txt" | cut -d= -f2 | tr -d '[:space:]'); then
  BUILD_TYPE=default
fi
[ -z "$BUILD_TYPE" ] && BUILD_TYPE=default
BIN="$BUILD/scratch/ns3.36.1-sue-thpt-line-rate-$BUILD_TYPE"
RATE="${1:-400Gbps}"
GBITS="${2:-10}"
LOSS="${3:-10000}"
CREDITS="${4:-1024}"

cmake --build "$BUILD" --target scratch_sue-thpt-line-rate -j 8

run_one () {  # $1 = lossEvery (0 or N); prints THPT lines; returns 0 iff PASS
  local out
  if ! out=$(timeout 300 env THPT_RATE="$RATE" THPT_GBITS="$GBITS" THPT_LOSS_EVERY="$1" THPT_CREDITS="$CREDITS" "$BIN"); then
    echo "run-s5c-thpt.sh: binary crashed/timeout (loss=$1, exit $?)" >&2
    return 2
  fi
  echo "$out" | grep -E '^THPT (RESULT|COUNTERS)|^THPT (PASS|FAIL)' || true
  if ! echo "$out" | grep -q 'THPT PASS'; then
    echo "run-s5c-thpt.sh: no THPT PASS in output (loss=$1) — verdict or binary failure" >&2
    return 1
  fi
  return 0
}

fail=0
echo "== clean link =="
run_one 0 || fail=1
if [ "$LOSS" -gt 0 ]; then
  echo "== ${LOSS}-loss (1/${LOSS} frames bad) =="
  run_one "$LOSS" || fail=1
fi
exit $fail
