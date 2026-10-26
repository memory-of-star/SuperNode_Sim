#!/usr/bin/env bash
# Build helper for the SUE scale-up platform on the ns-3-alibabacloud baseline.
#
# Workarounds for the local toolchain (Ubuntu / GCC 15 / Python 3.14):
#  1. The `./ns3` wrapper script is incompatible with Python 3.14 (argparse
#     rejects store_true on positional args) -> use CMake directly.
#  2. GCC 15 dropped transitive <stdint.h>; the baseline headers use uint8_t
#     without including it -> force-include stdint.h globally (no source edits).
#
# Usage:
#   ./build.sh                 # configure (once) + build everything, -j8
#   ./build.sh --target libsue # build a specific target
#   rm -rf ns-3-alibabacloud/simulation/build && ./build.sh   # full reconfigure
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM="$HERE/ns-3-alibabacloud/simulation"

if [ ! -f "$SIM/build/CMakeCache.txt" ]; then
  echo ">> configuring (no existing cache)"
  cmake -S "$SIM" -B "$SIM/build" \
    -DNS3_EXAMPLES=OFF -DNS3_TESTS=OFF -DNS3_MTP=ON \
    -DCMAKE_CXX_FLAGS="-include stdint.h"
fi

echo ">> building (-j8)"
cmake --build "$SIM/build" -j 8 "$@"
