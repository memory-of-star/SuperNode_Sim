#!/usr/bin/env bash
# Run every SUE scratch verification program and require its PASS verdict.
# Mirrors the TestSuite coverage through the direct-run binaries (the fork's
# test-runner / test.py are broken on Python 3.14; see AGENTS.md). Registers
# the full regression baseline in one place — CI can call this script instead
# of invoking each program by hand.
#
# Usage:
#   ./run-scratch-checks.sh          # all checks
#   ./run-scratch-checks.sh s5c s8   # subset (program-name substrings)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM="$HERE/ns-3-alibabacloud/simulation"
BUILD="$SIM/build"
if [ ! -d "$BUILD" ]; then
  echo "run-scratch-checks.sh: build dir $BUILD missing — run ./build.sh first" >&2
  exit 2
fi
if ! BUILD_TYPE=$(grep '^CMAKE_BUILD_TYPE:' "$BUILD/CMakeCache.txt" | cut -d= -f2 | tr -d '[:space:]'); then
  BUILD_TYPE=default
fi
[ -z "$BUILD_TYPE" ] && BUILD_TYPE=default
SCRATCH="$BUILD/scratch"

# (program, expected verdict regex)
CHECKS=(
  "sue-afh-device-dispatch:AFH DEVICE-DISPATCH PASS"
  "sue-afh-header-parse:AFH HEADER-PARSE PASS"
  "sue-afh-forward:AFH FORWARD PASS"
  "sue-afh-wire-time:AFH WIRE-TIME PASS"
  "sue-cbfc-engine-credit:CBFC ENGINE-CREDIT PASS"
  "sue-cbfc-e2e-gating:CBFC E2E-GATING PASS"
  "sue-cbfc-cc-update-onwire:CBFC CC-UPDATE-ONWIRE PASS"
  "sue-cfg-parse-apply:CFG PARSE-APPLY PASS"
  "sue-cfg-preset-smoke:CFG PRESET-SMOKE PASS"
  "sue-endpoint-ring:ENDPOINT PASS"
  "sue-entry-dispatch:ENTRY DISPATCH PASS"
  "sue-fc-counters:FC COUNTERS PASS"
  "sue-fc-facade:FC FACADE PASS"
  "sue-fc-frame-headers:FC FRAME-HEADERS PASS"
  "sue-fc-llr-cbfc-coexist:FC LLR-CBFC-COEXIST PASS"
  "sue-incast-2to1:INCAST PASS"
  "sue-fc-ctl-onwire:FC CTL-ONWIRE PASS"
  "sue-llr-engine:LLR ENGINE PASS"
  "sue-llr-flush-init:LLR FLUSH-INIT PASS"
  "sue-llr-pcs-lost:LLR PCS-LOST PASS"
  "sue-llr-poisoned:LLR POISONED PASS"
  "sue-llr-preamble-fcs:LLR PREAMBLE-FCS PASS"
  "sue-llr-replay-buffer:LLR REPLAY-BUFFER PASS"
  "sue-llr-ctlos-loss:LLR CTLOS-LOSS PASS"
  "sue-llr-random-errors:LLR RANDOM-ERRORS PASS"
  "sue-lldp-negotiate:LLDP NEGOTIATE PASS"
  "sue-llr-timer-wakeup:LLR TIMER-WAKEUP PASS"
  "sue-mtp-equivalence:MTP PASS"
  "sue-pfc-gating:PFC GATING PASS"
  "sue-thpt-line-rate:THPT PASS"
  "sue-scale-topo-gen:SCALE TOPO-GEN PASS"
  "sue-scale-smoke:SCALE SMOKE PASS"
  "sue-scale-stress:SCALE STRESS PASS"
  # VRF 32 平面全互联(§12.41):8 GPU × 32×200G、单台 256 口交换机、1792 流
  # all-to-all;默认 LLR=1(§12.42 退款尺寸不变性的事故现场),LLR=0 钉住
  # 同一退款路径的无 preamble 模式。FC=PFC 档(§12.43)走 802.1Qbb 装配
  # 全链:drain 取配置默认 0 → pause 不触发(§6.4 建模约束),六组断言全绿,
  # 钉住 PFC 拓扑装配/注入门控/采样诊断路径;drain>0 的吞吐塌落实验数字
  # 见 DESIGN §12.43 与 SC-34(PFC 更差,不满足 95% 标尺,故不注册)。各 ~93s。
  "sue-vrf-planes:VRF-PLANES PASS"
  "sue-vrf-planes:VRF-PLANES PASS:VRF_PL_LLR=0"
  "sue-vrf-planes:VRF-PLANES PASS:VRF_PL_FC=PFC"
  "sue-axi-workload:AXI-WORKLOAD PASS:AXI_LLR=1 AXI_LOSS_EVERY=100"
  "sue-axi-workload:AXI-WORKLOAD PASS:AXI_LLR=0 AXI_LOSS_EVERY=100 AXI_TXNS=500"

  # CBFC CreditSize 异粒度四臂对照(§12.56, UEC §5.2.5.8 method-2):min() 基线
  # 32/32、方案 32/56+CC 对账、反例 32/56 无 CC(泄漏耗尽窗口硬停)、理想
  # 56/56。判语: a/d 线速且互差<5%, b 恢复 ≥90% a, c ≤75% a, SW 原生
  # cell 计数 d < a(min() 口径放大约 71%)。
  "sue-cbfc-cellsize:CBFC CELLSIZE PASS"
  # CF_Update 速率扫描(§thu-cf-update-verify 3.12/3.13):裸跑=芯片默认档
  # (threshold 2048B 线速打流 → 6.10M/s@100G),env 段为分号分隔的键值对。
  # THU-SPEC 三速率表对照:default 列 = threshold 2075 + CF_MAX 1.25MB(大帧);
  # max 列 = CF_MIN 960 地板(须 100B 小帧,帧节奏密于地板);min 列 =
  # CF_MAX 12582 无打流(触发口径 993,479/s = THU min,消息 ×4 批量)。
  "sue-cf-rate-scan:CF RATE-SCAN PASS"
  "sue-cf-rate-scan:CF RATE-SCAN PASS:CF_THRESHOLD_BYTES=1;CF_FRAME_BYTES=100;CF_GBITS=1"
  "sue-cf-rate-scan:CF RATE-SCAN PASS:CF_THRESHOLD_BYTES=1;CF_MIN_BYTES=1024;CF_FRAME_BYTES=100;CF_GBITS=1"
  "sue-cf-rate-scan:CF RATE-SCAN PASS:CF_MAX_BYTES=1048576;CF_GBITS=0"
  "sue-cf-rate-scan:CF RATE-SCAN PASS:CF_THRESHOLD_BYTES=2075;CF_MAX_BYTES=1250000;CF_GBITS=1"
  "sue-cf-rate-scan:CF RATE-SCAN PASS:CF_RATE=200Gbps;CF_THRESHOLD_BYTES=2075;CF_MAX_BYTES=1250000;CF_GBITS=1"
  "sue-cf-rate-scan:CF RATE-SCAN PASS:CF_RATE=400Gbps;CF_THRESHOLD_BYTES=2075;CF_MAX_BYTES=1250000;CF_GBITS=1"
  "sue-cf-rate-scan:CF RATE-SCAN PASS:CF_THRESHOLD_BYTES=1;CF_MIN_BYTES=960;CF_FRAME_BYTES=100;CF_GBITS=1"
  "sue-cf-rate-scan:CF RATE-SCAN PASS:CF_RATE=200Gbps;CF_THRESHOLD_BYTES=1;CF_MIN_BYTES=960;CF_FRAME_BYTES=100;CF_GBITS=1"
  "sue-cf-rate-scan:CF RATE-SCAN PASS:CF_RATE=400Gbps;CF_THRESHOLD_BYTES=1;CF_MIN_BYTES=960;CF_FRAME_BYTES=100;CF_GBITS=1"
  "sue-cf-rate-scan:CF RATE-SCAN PASS:CF_MAX_BYTES=12582;CF_GBITS=0"
)

cmake --build "$BUILD" -j 8 --target $(for c in "${CHECKS[@]}"; do echo "scratch_${c%%:*}"; done | tr '\n' ' ') >/dev/null

fail=0; ran=0
for c in "${CHECKS[@]}"; do
  prog="${c%%:*}"; rest="${c#*:}"
  verdict="${rest%%:*}"; envs=()
  if [ "$rest" != "$verdict" ]; then
    # third field: semicolon-separated KEY=VAL env pairs for this run
    IFS=';' read -ra kv <<< "${rest#*:}"
    for e in "${kv[@]}"; do [ -n "$e" ] && envs+=("$e"); done
  fi
  # no args = run everything; otherwise run programs matching any arg substring
  if [ $# -gt 0 ]; then
    case " $* " in *" $prog "*) : ;; *) continue ;; esac
  fi
  ran=1
  bin="$SCRATCH/ns3.36.1-$prog-$BUILD_TYPE"
  out=$(timeout 600 env "${envs[@]}" "$bin"); st=$?
  if [ $st -ne 0 ]; then
    echo "FAIL $prog: binary crashed/timeout (exit $st)" >&2
    fail=1
    continue
  fi
  if echo "$out" | grep -q "$verdict"; then
    echo "PASS $prog"
  else
    echo "FAIL $prog: verdict '$verdict' not found in output" >&2
    fail=1
  fi
done
[ "$ran" = 1 ] || { echo "run-scratch-checks.sh: no matching programs ($*)" >&2; exit 2; }
echo "scratch checks: $(( ${#CHECKS[@]} )) programs, fail=$fail"
exit $fail
