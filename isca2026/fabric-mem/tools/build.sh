#!/usr/bin/env bash
set -euo pipefail
paper_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
"$paper_root/tools/setup.sh"
cd "$paper_root"
mkdir -p build .cache
export XDG_CACHE_HOME="$paper_root/.cache"
.tools/tectonic --bundle https://relay.fullyjustified.net/default_bundle_v33.tar \
  --keep-logs --keep-intermediates --synctex --outdir build main.tex
cp build/main.pdf paper.pdf
printf 'Built %s/paper.pdf\n' "$paper_root"
