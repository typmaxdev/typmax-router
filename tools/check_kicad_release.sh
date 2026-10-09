#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
#
# tools/check_kicad_release.sh — is there a newer stable KiCad than the one vendored?
#
# Reads the tag vendor/kicad.manifest records, asks GitLab's tags API for
# KiCad's tags, and prints exactly one line:
#
#   current                  no newer stable release            exit 0
#   patch-release <tag>      a newer X.Y.Z with the same X.Y    exit 10
#   major-release <tag>      a newer X.Y (major or minor)       exit 11
#   error: <why>             the manifest could not be read     exit 2
#   error: <why>             the tags could not be fetched      exit 3
#   error: <why>             the list lacks the vendored tag     exit 6
#
# A patch release wins over a major one when both exist (it is the one the
# watcher can take on its own). Stable means X.Y.Z with three numbers and Y
# not 99 (KiCad's development series is X.99); release candidates (-rcN) and
# four-part hotfix tags are ignored. A failed fetch is never "current", and
# neither is a list that does not contain the vendored tag itself (an empty
# list, another project's tags, a page that no longer reaches back to it):
# the vendored tag is a stable tag of the same repository, so a list without
# it is not a list to judge by.
# Read-only: it writes nothing anywhere.
#
# Environment (tests and the CI workflow):
#   TYPMAX_KICAD_MANIFEST    the manifest to read (default vendor/kicad.manifest)
#   TYPMAX_KICAD_TAGS_FILE   read the tag list from this JSON file instead of the API
#   TYPMAX_KICAD_TAGS_URL    the API URL (default: GitLab, kicad/code/kicad, newest first)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MANIFEST="${TYPMAX_KICAD_MANIFEST:-$ROOT/vendor/kicad.manifest}"
URL="${TYPMAX_KICAD_TAGS_URL:-https://gitlab.com/api/v4/projects/kicad%2Fcode%2Fkicad/repository/tags?order_by=updated&sort=desc&per_page=100}"
PY="$(command -v python3.11 || command -v python3 || true)"
[ -n "$PY" ] || { echo "error: python3 is required"; exit 2; }

CURRENT="$(sed -n 's/^tag //p' "$MANIFEST" 2>/dev/null | head -1)"
[ -n "$CURRENT" ] || { echo "error: no tag in $MANIFEST"; exit 2; }

if [ -n "${TYPMAX_KICAD_TAGS_FILE:-}" ]; then
  TAGS_JSON="$(cat "$TYPMAX_KICAD_TAGS_FILE" 2>/dev/null)" || { echo "error: cannot read $TYPMAX_KICAD_TAGS_FILE"; exit 3; }
else
  TAGS_JSON="$(curl -fsS --max-time 30 --retry 2 "$URL" 2>/dev/null)" || { echo "error: could not fetch the tag list from GitLab"; exit 3; }
fi

printf '%s' "$TAGS_JSON" | "$PY" -c '
import json, re, sys
current = sys.argv[1]
STABLE = re.compile(r"^([0-9]+)\.([0-9]+)\.([0-9]+)$")
def ver(t):
    m = STABLE.match(t)
    if not m or int(m.group(2)) == 99:
        return None
    return tuple(int(x) for x in m.groups())
try:
    tags = [t["name"] for t in json.load(sys.stdin)]
except Exception:
    print("error: the tag list is not the JSON the GitLab API returns"); sys.exit(3)
cur = ver(current)
if cur is None:
    print(f"error: the vendored tag {current} is not a stable X.Y.Z"); sys.exit(2)
if current not in tags:
    print(f"error: the tag list ({len(tags)} tags) does not contain the vendored tag {current}"); sys.exit(6)
stable = sorted({v for v in map(ver, tags) if v})
patch = [v for v in stable if v[:2] == cur[:2] and v > cur]
major = [v for v in stable if v[:2] > cur[:2]]
fmt = lambda v: ".".join(map(str, v))
if patch:
    print(f"patch-release {fmt(patch[-1])}"); sys.exit(10)
if major:
    print(f"major-release {fmt(major[-1])}"); sys.exit(11)
print("current"); sys.exit(0)
' "$CURRENT"
