#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
#
# tools/kicad_cli_docker.sh — kicad-cli from KiCad's official docker image, for a
# machine without KiCad (the GitHub runner). Use it as the fixture suite's witness:
#
#   KICAD_CLI=$PWD/tools/kicad_cli_docker.sh TYPMAX_FIXTURES_DIR=… ctest -R fixtures
#
# The image is kicad/kicad:$TYPMAX_KICAD_CLI_IMAGE_TAG (default 10.0). The
# working directory and every directory in $TYPMAX_KICAD_CLI_MOUNTS (space
# separated) are mounted at the same paths, so absolute paths in the arguments
# resolve inside the container. Runs as the calling user.
set -euo pipefail
image="kicad/kicad:${TYPMAX_KICAD_CLI_IMAGE_TAG:-10.0}"
mounts=(-v "$PWD:$PWD")
for d in ${TYPMAX_KICAD_CLI_MOUNTS:-}; do
  [ -d "$d" ] && mounts+=(-v "$d:$d")
done
exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp "${mounts[@]}" -w "$PWD" \
  --entrypoint kicad-cli "$image" "$@"
