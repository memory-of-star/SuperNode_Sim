#!/usr/bin/env bash
set -euo pipefail
paper_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$paper_root/.tools"
if [[ ! -x "$paper_root/.tools/tectonic" ]]; then
  url='https://github.com/tectonic-typesetting/tectonic/releases/download/tectonic%400.17.0/tectonic-0.17.0-x86_64-unknown-linux-musl.tar.gz'
  curl -fL --retry 3 "$url" -o "$paper_root/.tools/tectonic.tar.gz"
  echo "8533d07f9ccbd7a65824b9e0459041bca34af1eb33daba48f59215593753a3b7  $paper_root/.tools/tectonic.tar.gz" | sha256sum -c -
  tar -xzf "$paper_root/.tools/tectonic.tar.gz" -C "$paper_root/.tools"
fi
"$paper_root/.tools/tectonic" --version
