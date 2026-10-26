#!/usr/bin/env bash
# Run the SUE TestSuite (src/sue/test/sue-test.cc).
#
# The fork's build does not produce a test-runner binary and test.py is broken
# on Python 3.14, so this compiles a minimal TestRunner (links libsue-test,
# which registers the suite at static-init) and runs it. Mirrors the build.sh
# workaround convention; does not touch any baseline module.
#
# Usage:
#   ./run-sue-tests.sh              # run the full SUE suite, verbose
#   ./run-sue-tests.sh --suite=sue --xml=result.xml
#   ./run-sue-tests.sh --suite=sue --verbose
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM="$HERE/ns-3-alibabacloud/simulation"
BUILD="$SIM/build"
LIB="$BUILD/lib"
BIN="$BUILD/sue-test-runner"
SRC="$BUILD/sue-test-runner.cc"

# 1. Ensure the module + test library are built. build.sh configures with
#    NS3_TESTS=OFF (fresh deploys / any reconfigure lose the target; AGENTS.md
#    "已知基线问题": NS3_TESTS=ON 会让无参 all 踩 mtp-test-suite), so probe
#    first and, when absent, flip the cache ON just for this target — then
#    restore OFF so the default `./build.sh`(=all) stays safe.
if ! make -C "$BUILD" -n libsue-test >/dev/null 2>&1; then
  echo ">> libsue-test absent (NS3_TESTS=OFF cache) -> reconfigure with tests ON"
  cmake -S "$SIM" -B "$BUILD" -DNS3_TESTS=ON >/dev/null
fi
cmake --build "$BUILD" --target libsue-test -j 8

# 1b. Resolve the library profile suffix from CMakeCache.txt (avoids assuming
#     "-default"; falls back to a glob if the cache is stale).
if ! BUILD_TYPE=$(grep '^CMAKE_BUILD_TYPE:' "$BUILD/CMakeCache.txt" | cut -d= -f2 | tr -d '[:space:]'); then
  BUILD_TYPE=default
fi
[ -z "$BUILD_TYPE" ] && BUILD_TYPE=default
SUF="-${BUILD_TYPE}"
resolve_lib () {   # $1 = lib base name (e.g. "sue-test")
  local glob
  glob=$(ls "$LIB"/libns3.36.1-"$1""$SUF".so 2>/dev/null || ls "$LIB"/libns3.36.1-"$1"-*.so 2>/dev/null | head -n1)
  if [ -z "$glob" ]; then
    echo "run-sue-tests.sh: cannot find libns3.36.1-$1*.so under $LIB (build type '$BUILD_TYPE')" >&2
    exit 1
  fi
  echo "$glob"
}
SUE_TEST_LIB=$(resolve_lib sue-test)
SUE_LIB=$(resolve_lib sue)
P2P_LIB=$(resolve_lib point-to-point)
NETWORK_LIB=$(resolve_lib network)
CORE_LIB=$(resolve_lib core)
# With NS3_MTP=ON the point-to-point lib references MtpInterface symbols
# (rdma-hw.cc explicitCriticalSection), so the runner must also link libmtp.
MTP_LIB=$(ls "$LIB"/libns3.36.1-mtp*.so 2>/dev/null | head -n1 || true)

# 2. Compile the minimal runner (idempotent; g++ rebuilds if source is newer).
#    --no-as-needed is required: the suite registers via a static initializer,
#    so nothing in main references libsue-test and a default as-needed link
#    would drop it (and with it the whole suite).
if [ ! -x "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
  cat > "$SRC" <<'EOF'
#include "ns3/test.h"
int main (int argc, char *argv[]) { return ns3::TestRunner::Run (argc, argv); }
EOF
  g++ -std=gnu++17 -O2 -g -include stdint.h -I"$BUILD/include" -I/usr/include/libxml2 \
    -o "$BIN" "$SRC" \
    -Wl,-rpath,"$LIB" \
    -Wl,--no-as-needed "$SUE_TEST_LIB" -Wl,--as-needed \
    "$SUE_LIB" \
    "$P2P_LIB" \
    "$NETWORK_LIB" \
    "$CORE_LIB" \
    "$MTP_LIB"
fi

# 3. Default to the full suite, verbose (per-case results).
if [ $# -eq 0 ]; then
  set -- --suite=sue --verbose
fi
"$BIN" "$@"
rc=$?

# Restore NS3_TESTS=OFF (keeps the default `./build.sh` = all target out of the
# mtp-test-suite breakage). The already-built libsue-test .so stays on disk, so
# the runner binary above keeps working across invocations.
cmake -S "$SIM" -B "$BUILD" -DNS3_TESTS=OFF >/dev/null
exit $rc
