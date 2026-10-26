#!/usr/bin/env bash
# S5e 2:1 CBFC incast (DESIGN §12.29): three XPU hosts, all links 200Gbps
# LLR+CBFC, host0+host1 both line-rate senders into host2 (2 × ~190G offered
# into one 200G egress). Prints the delivery curve, credit probes, per-source
# split and the lossless/rate CHECKs.
#
# Usage:
#   ./run-s5e-incast.sh [RATE] [GBITS] [CREDITS] [DSCP0] [DSCP1] [DSCP_TO_VC_MAP]
#     RATE     link rate (default 200Gbps)
#     GBITS    per-sender payload, Gbit (default 1 -> 61k frames @ 2048B each)
#     CREDITS  CBFC credit limit per VC (default 1024; 512 caps ~91.75%)
#     DSCP0    host0 shim-DSCP flow class (default 0; >0 maps to a VC via
#              DSCP_TO_VC_MAP, §7) -> multi-VC incast CHECKs enabled
#     DSCP1    host1 shim-DSCP flow class (default 0)
#     DSCP_TO_VC_MAP  optional "k:v,..." override injected into the inline conf
#              (end-to-end non-identity mapping proof; default = direct map)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM="$HERE/ns-3-alibabacloud/simulation"
BUILD="$SIM/build"
if [ ! -d "$BUILD" ]; then
  echo "run-s5e-incast.sh: build dir $BUILD missing — run ./build.sh first" >&2
  exit 2
fi
if ! BUILD_TYPE=$(grep '^CMAKE_BUILD_TYPE:' "$BUILD/CMakeCache.txt" | cut -d= -f2 | tr -d '[:space:]'); then
  BUILD_TYPE=default
fi
[ -z "$BUILD_TYPE" ] && BUILD_TYPE=default
BIN="$BUILD/scratch/ns3.36.1-sue-incast-2to1-$BUILD_TYPE"
RATE="${1:-200Gbps}"
GBITS="${2:-1}"
CREDITS="${3:-1024}"
DSCP0="${4:-0}"
DSCP1="${5:-0}"
DSCP_TO_VC_MAP="${6:-}"

cmake --build "$BUILD" --target scratch_sue-incast-2to1 -j 8

if ! out=$(timeout 300 env INCAST_RATE="$RATE" INCAST_GBITS="$GBITS" INCAST_CREDITS="$CREDITS" \
      INCAST_DSCP0="$DSCP0" INCAST_DSCP1="$DSCP1" \
      INCAST_DSCP_TO_VC_MAP="$DSCP_TO_VC_MAP" "$BIN"); then
  echo "run-s5e-incast.sh: binary crashed/timeout (exit $?)" >&2
  exit 2
fi
echo "$out" | grep -E '^INCAST (RESULT|SPLIT|COUNTERS)|^INCAST (PASS|FAIL)' || true
# keep the CHECK + delivery-curve + per-source/egress rate-curve lines (the
# evidence, not just the verdict)
echo "$out" | grep -E '^\s+\[(ok|FAIL)\]|^\s+t=.*rx=|^\s+RATES ' || true
if ! echo "$out" | grep -q 'INCAST PASS'; then
  echo "run-s5e-incast.sh: no INCAST PASS in output — verdict or binary failure" >&2
  exit 1
fi
