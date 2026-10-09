#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
# Download KiCad's demo boards used by the fixture tests into ./fixtures/ (gitignored).
# They come from the vendored KiCad tag (vendor/kicad.manifest), not from master:
# master's demos move to the next file format before any kicad-cli 10.0.x reads it.
# TYPMAX_FIXTURES_REF names another tag or commit.
# They are CC BY-SA 4.0 (KiCad demos): never commit them. Then:
#   TYPMAX_FIXTURES_DIR=$PWD/fixtures ctest --test-dir build -R fixtures --output-on-failure
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
dir="${1:-$root/fixtures}"
ref="${TYPMAX_FIXTURES_REF:-$(sed -n 's/^tag //p' "$root/vendor/kicad.manifest" | head -1)}"
case "$ref" in
  ""|-*|*[!A-Za-z0-9._-]*) echo "fetch_fixtures: no usable KiCad ref (vendor/kicad.manifest or TYPMAX_FIXTURES_REF): '$ref'" >&2; exit 2 ;;
esac
mkdir -p "$dir"
for name in pic_programmer video; do
  url="https://gitlab.com/kicad/code/kicad/-/raw/${ref}/demos/${name}/${name}.kicad_pcb"
  echo "fetching $url"
  curl -fsSL "$url" -o "$dir/${name}.kicad_pcb"
done
echo "fixtures in $dir (export TYPMAX_FIXTURES_DIR=$dir)"
