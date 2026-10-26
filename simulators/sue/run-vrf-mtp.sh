#!/usr/bin/env bash
# VRF 多平面拓扑的 MTP 等价性检查(DESIGN.md §10.5/§12.44,§12.54 v2:IO Die
# 编址、8 平面):单线程 baseline(DefaultSimulatorImpl)vs 单进程多线程
# (NS3_MTP 自动分区)各跑一遍 sue-vrf-planes(8 GPU × 4 IO Die × 8 口 =
# 256 口单交换机,XPU-ID 含 Die 位),diff 行为 hash(VRF-PLANES BHASH:
# SueScratchAppendDeviceCounters 掩 ACK/NACK + CF_Update 节奏类计数 + 交付矩阵)。
#
# 判据(§12.28 同设计):
#   - 双跑 BHASH 逐字节一致 => 数据路径/状态机/交付在两种模拟器下等价;
#   - 双跑各自 VRF-PLANES PASS(互通/平面隔离/端口对称/LLR clean/95% 天花板);
#   - 掩掉的 ACK/NACK 计数仍由 CHECK 4(LLR clean,replay==discard==nack==0)
#     与 delivered==sent 交付断言兜底——"ACK 被吞"类 bug 不会被掩蔽。
#
# 场景臂:默认(LLR=1 + CBFC)、LLR=0、PFC drain=0、PFC drain=2000(生效档)、
# SPLIT(VRF_PL_SPLIT=1 拓扑分解变体,§12.45/§12.54 v2:按 stripe 拆 8 台
# 32 口平面交换机 = 16 LP)。
# 前三臂要求双跑 VRF-PLANES PASS;drain=2000 臂按 §12.43 吞吐塌落到 ~45%
# (22.3T vs 95% 标尺),性能断言 FAIL 是预期——该臂判据放宽为"双跑判定
# 一致(同为 FAIL)+ BHASH 一致",验证的是 PFC pause 生效路径的 MTP 等价。
# 全臂跑完约 10 × 90s(2026-08 实测)。
#
# Usage:
#   ./run-vrf-mtp.sh [N]        # N = MTP 线程数(默认 8)
#   ./run-vrf-mtp.sh 2,4,8      # 逗号列表:多线程数扫描
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM="$HERE/ns-3-alibabacloud/simulation"
BUILD="$SIM/build"
if [ ! -d "$BUILD" ]; then
  echo "run-vrf-mtp.sh: build dir $BUILD missing — run ./build.sh first" >&2
  exit 2
fi
if ! BUILD_TYPE=$(grep '^CMAKE_BUILD_TYPE:' "$BUILD/CMakeCache.txt" | cut -d= -f2 | tr -d '[:space:]'); then
  BUILD_TYPE=default
fi
[ -z "$BUILD_TYPE" ] && BUILD_TYPE=default
BIN="$BUILD/scratch/ns3.36.1-sue-vrf-planes-$BUILD_TYPE"
THREADS_SPEC="${1:-8}"

cmake --build "$BUILD" --target scratch_sue-vrf-planes -j 8

run_arm () {  # $1 = env 赋值串(臂配置) $2 = 线程数 -> 0 iff 等价且双 PASS
  local arm="$1" n="$2" out1 out2 h1 h2 p1 p2
  case "$n" in
    ''|*[!0-9]*) echo "run-vrf-mtp.sh: invalid thread count '$n' (integer expected)" >&2; return 2 ;;
    *) ;;
  esac
  # sue-vrf-planes 以 exit code 表达判定(g_ok?0:1)——drain>0 臂性能断言
  # FAIL 是预期(§12.43),退出码 0/1 均为正常完成,仅 >1 视为崩溃。
  out1=$(timeout 600 env $arm VRF_PL_THREADS=1 "$BIN"); rc1=$?
  if [ "$rc1" -gt 1 ]; then echo "  baseline (threads=1) CRASHED/timeout (exit $rc1)" >&2; return 2; fi
  out2=$(timeout 600 env $arm VRF_PL_THREADS="$n" "$BIN"); rc2=$?
  if [ "$rc2" -gt 1 ]; then echo "  mtp (threads=$n) CRASHED/timeout (exit $rc2)" >&2; return 2; fi
  h1=$(echo "$out1" | grep '^VRF-PLANES BHASH') || { echo "  baseline output missing VRF-PLANES BHASH line" >&2; return 2; }
  h2=$(echo "$out2" | grep '^VRF-PLANES BHASH') || { echo "  mtp output missing VRF-PLANES BHASH line" >&2; return 2; }
  p1=$(echo "$out1" | grep -o 'VRF-PLANES \(PASS\|FAIL\)' | head -1)
  p2=$(echo "$out2" | grep -o 'VRF-PLANES \(PASS\|FAIL\)' | head -1)
  echo "== arm [$arm] (threads=1 vs $n) =="
  echo "baseline (threads=1) : ${p1:-<no verdict>}"
  echo "mtp     (threads=$n) : ${p2:-<no verdict>}"
  if [ -z "$p1" ] || [ "$p1" != "$p2" ]; then
    echo "VRF-MTP EQUIVALENCE FAIL (verdicts differ or missing: '$p1' vs '$p2')"
    return 1
  fi
  case "$arm" in
    *DRAIN_NS*) # §12.43 生效档:性能断言 FAIL 是预期(§12.43 ~45%),双跑判定一致即可
      ;;
    *)
      if [ "$p1" != "VRF-PLANES PASS" ]; then
        echo "VRF-MTP EQUIVALENCE FAIL (self-check not PASS on both runs)"
        return 1
      fi
      ;;
  esac
  if [ "$h1" != "$h2" ]; then
    echo "VRF-MTP EQUIVALENCE FAIL (BHASH differs)"
    diff <(echo "$h1" | tr ';' '\n') <(echo "$h2" | tr ';' '\n') | head -20 || true
    return 1
  fi
  echo "VRF-MTP EQUIVALENCE PASS"
}

rc=0
for n in ${THREADS_SPEC//,/ }; do
  run_arm "VRF_PL_LLR=1" "$n" || rc=1
  run_arm "VRF_PL_LLR=0" "$n" || rc=1
  run_arm "VRF_PL_FC=PFC" "$n" || rc=1
  run_arm "VRF_PL_FC=PFC VRF_PL_PFC_DRAIN_NS=2000" "$n" || rc=1
  run_arm "VRF_PL_SPLIT=1" "$n" || rc=1
done
exit $rc
