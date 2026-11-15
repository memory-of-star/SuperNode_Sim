#!/usr/bin/env bash
set -euo pipefail
paper_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "$(uname -s)/$(uname -m)" != Linux/x86_64 ]]; then
  printf 'The pinned toolchain requires Linux x86_64. Use latexmk with TeX Live on other platforms.\n' >&2
  exit 1
fi
mkdir -p "$paper_root/.tools"
archive="$paper_root/.tools/tectonic-0.17.0.tar.gz"
expected='8533d07f9ccbd7a65824b9e0459041bca34af1eb33daba48f59215593753a3b7'
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 3 \
    'https://github.com/tectonic-typesetting/tectonic/releases/download/tectonic%400.17.0/tectonic-0.17.0-x86_64-unknown-linux-musl.tar.gz' \
    --output "$archive.part"
  mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$expected" "$archive" | sha256sum --check --status
tar -xzf "$archive" -C "$paper_root/.tools" tectonic
"$paper_root/.tools/tectonic" --version
