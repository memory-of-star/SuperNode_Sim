#!/usr/bin/env bash
# S5a MTP equivalence check (DESIGN.md §10.5): run the SUE network once on the
# single-threaded baseline (DefaultSimulatorImpl) and once on the single-process
# multithreaded simulator (NS3_MTP, auto-partition, MTP_THREADS threads), then
# diff the aggregate result hash. Identical hashes => the model's node-owned
# timers are context-routed correctly and node state is single-owner per LP.
#
# Two equivalence modes (§10.5 收尾清单 ②):
#   - NACK : one corrupted frame -> NACK -> replay -> clean lossless convergence.
#   - FLUSH: first 3 frames corrupted + LLR_REPLAY_CT_MAX=2 -> replay_ct
#     exhaustion FLUSH -> cross-LP INIT/INIT_ECHO re-sync -> recovery (discard>0).
# Each mode must hash identically across the two thread counts, and both runs of
# a mode must self-report PASS (the mode-specific CHECKs).
#
# Equivalence criterion (§12.20/§12.27/§12.28): the diff compares the BEHAVIORAL
# hash (MTP BHASH), which masks the ACK/NACK CtlOS tallies. Those counts are
# order-sensitive (periodic cumulative-ACK cadence + coalescing + dup
# classification; a cross-LP frame arrival sharing a timestamp with the local
# ACK timer tie-breaks differently under Default vs MTP, shifting the
# cumulative ACK seq stream by one level, n8d4 rx_ack ±1). Hardening (§12.28
# review fix): the masked tallies are still asserted per-field as |delta| <= 1
# via the MTP ACKSUM line — a real "ACKs swallowed / duplicated" MTP bug would
# show a large delta while the behavioral hash stays identical. The full S8
# HASH stays printed for diagnostics.
#
# Usage:
#   ./run-s8-mtp.sh [N]            # N = MTP thread count (default 8)
#   ./run-s8-mtp.sh 2,4,8          # comma list: scan multiple thread counts
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM="$HERE/ns-3-alibabacloud/simulation"
BUILD="$SIM/build"
if [ ! -d "$BUILD" ]; then
  echo "run-s8-mtp.sh: build dir $BUILD missing — run ./build.sh first" >&2
  exit 2
fi
if ! BUILD_TYPE=$(grep '^CMAKE_BUILD_TYPE:' "$BUILD/CMakeCache.txt" | cut -d= -f2 | tr -d '[:space:]'); then
  BUILD_TYPE=default
fi
[ -z "$BUILD_TYPE" ] && BUILD_TYPE=default
BIN="$BUILD/scratch/ns3.36.1-sue-mtp-equivalence-$BUILD_TYPE"
THREADS_SPEC="${1:-8}"

cmake --build "$BUILD" --target scratch_sue-mtp-equivalence -j 8

run_mode () {  # $1 = MTP_FLUSH (0 NACK / 1 FLUSH) $2 = MTP thread count -> 0 iff PASS
  local mode="$1" n="$2" mlabel out1 out2 h1 h2 p1 p2
  if [ "$mode" = 1 ]; then mlabel="FLUSH"; else mlabel="NACK"; fi
  case "$n" in
    ''|*[!0-9]*) echo "run-s8-mtp.sh: invalid thread count '$n' (integer expected)" >&2; return 2 ;;
    *) ;;
  esac
  out1=$(timeout 300 env MTP_THREADS=1    MTP_FLUSH="$mode" "$BIN") || { echo "  baseline (threads=1) CRASHED/timeout (exit $?)" >&2; return 2; }
  out2=$(timeout 300 env MTP_THREADS="$n" MTP_FLUSH="$mode" "$BIN") || { echo "  mtp (threads=$n) CRASHED/timeout (exit $?)" >&2; return 2; }
  h1=$(echo "$out1" | grep '^MTP BHASH') || { echo "  baseline output missing MTP BHASH line" >&2; return 2; }
  h2=$(echo "$out2" | grep '^MTP BHASH') || { echo "  mtp output missing MTP BHASH line" >&2; return 2; }
  p1=$(echo "$out1" | grep -o 'MTP \(PASS\|FAIL\)' | head -1)
  p2=$(echo "$out2" | grep -o 'MTP \(PASS\|FAIL\)' | head -1)
  echo "== $mlabel mode (threads=1 vs $n) =="
  echo "baseline (threads=1) : $h1  [$p1]"
  echo "mtp     (threads=$n) : $h2  [$p2]"
  echo "  (MTP BHASH = behavioral hash; ACK/NACK CtlOS tallies masked)"
  local ok=1
  if [ "$h1" != "$h2" ] || [ "$p1" != "MTP PASS" ] || [ "$p2" != "MTP PASS" ]; then
    ok=0
  fi
  # Masked-tally hardening: |delta| <= 1 per field across the two simulators.
  local a1 a2 v1 v2 d
  a1=$(echo "$out1" | grep '^MTP ACKSUM') || { echo "  baseline output missing MTP ACKSUM" >&2; ok=0; }
  a2=$(echo "$out2" | grep '^MTP ACKSUM') || { echo "  mtp output missing MTP ACKSUM" >&2; ok=0; }
  for f in tx_ack tx_nack rx_ack rx_nack seq_err; do
    v1=$(echo "$a1" | grep -oE "$f=[0-9]+" | cut -d= -f2)
    v2=$(echo "$a2" | grep -oE "$f=[0-9]+" | cut -d= -f2)
    if [ -z "$v1" ] || [ -z "$v2" ]; then ok=0; continue; fi
    d=$((v1 > v2 ? v1 - v2 : v2 - v1))
    if [ "$d" -gt 1 ]; then
      echo "  MTP ACKSUM DELTA FAIL: $f $v1 vs $v2 (delta $d > 1 — possible ACK swallow/dup bug)"
      ok=0
    fi
  done
  if [ "$ok" = 1 ]; then
    echo "$mlabel MTP EQUIVALENCE PASS (threads=$n)"
    return 0
  fi
  echo "$mlabel MTP EQUIVALENCE FAIL (threads=$n)"
  return 1
}

fail=0
for n in ${THREADS_SPEC//,/ }; do
  run_mode 0 "$n" || fail=$((fail + 1))
  run_mode 1 "$n" || fail=$((fail + 1))
done
exit $fail
