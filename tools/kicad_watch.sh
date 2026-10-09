#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
#
# tools/kicad_watch.sh — act on a new stable KiCad release. Never merges.
#
#   tools/kicad_watch.sh [--dry-run]
#
# Runs tools/check_kicad_release.sh, then:
#   current        nothing to do.
#   patch-release  (same X.Y as vendored) creates the local branch
#                  update/kicad-<tag> from the base branch in a scratch
#                  worktree, measures the base (build + fixtures when
#                  TYPMAX_FIXTURES_DIR is set), runs tools/update_kicad.sh <tag>
#                  there (re-vendor, patches, rebuild, gates, golden diff),
#                  commits what it changed plus reports/kicad-<tag>.md on that
#                  branch, and leaves it for review. A branch that already
#                  exists, locally OR on the remote (refs/remotes/origin/...,
#                  what a fresh CI checkout has), is not touched again: a
#                  person may have edited or rejected it.
#   major-release  (a new X.Y) writes reports/kicad-<tag>.md only: a port is
#                  needed; the report holds a dry run's patch and build check.
#   error          (no network, no manifest) is logged as such, never "current".
# Every run appends one line to reports/watch.log; a lock
# (reports/.watch.lock) keeps two runs from overlapping. It is the script
# .github/workflows/kicad-watch.yml calls on a GitHub runner (the plan); a
# local run works the same way.
#
# Exit: 0 done (or nothing to do), 1 usage, 2/3/6 the check failed (as
# check_kicad_release.sh), 4 another run holds the lock, 5 the update ran but
# a gate or a patch failed (the branch and the report say which).
#
# Environment: TYPMAX_WATCH_BASE (base branch, default main),
# TYPMAX_KICAD_CACHE (as update_kicad.sh), TYPMAX_FIXTURES_DIR and KICAD_CLI
# (the fixture gate). For tests:
# TYPMAX_WATCH_CHECK / TYPMAX_WATCH_UPDATE replace the check and update
# commands.
set -uo pipefail

DRY=0
case "${1:-}" in
  --dry-run) DRY=1 ;;
  "") ;;
  *) sed -n '5,7p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPORTS="$ROOT/reports"
LOG="$REPORTS/watch.log"
LOCK="$REPORTS/.watch.lock"
BASE="${TYPMAX_WATCH_BASE:-main}"
CACHE="${TYPMAX_KICAD_CACHE:-${TMPDIR:-/tmp}/typmax-kicad-cache}"
CACHE="${CACHE%/}"
CHECK="${TYPMAX_WATCH_CHECK:-$ROOT/tools/check_kicad_release.sh}"
UPDATE="${TYPMAX_WATCH_UPDATE:-tools/update_kicad.sh}"
PY="$(command -v python3.11 || command -v python3)"
mkdir -p "$REPORTS"

now() { date -u +%Y-%m-%dT%H:%M:%SZ; }
log() { printf '%s %s\n' "$(now)" "$*" >>"$LOG"; }
finish() { local code="$1"; shift; log "$*"; echo "kicad_watch: $*"; exit "$code"; }

# --- the lock: a directory (atomic), with the holder's pid; a dead holder's lock is taken over
if ! mkdir "$LOCK" 2>/dev/null; then
  holder="$(cat "$LOCK/pid" 2>/dev/null || true)"
  if [ -n "$holder" ] && kill -0 "$holder" 2>/dev/null; then
    log "locked: run $holder is still going"; echo "kicad_watch: another run ($holder) holds $LOCK" >&2; exit 4
  fi
  rm -rf "$LOCK"; mkdir "$LOCK" || { echo "kicad_watch: cannot take $LOCK" >&2; exit 4; }
fi
echo $$ >"$LOCK/pid"
WT=""
cleanup() {
  [ -n "$WT" ] && [ -d "$WT" ] && git -C "$ROOT" worktree remove --force "$WT" >/dev/null 2>&1
  rm -rf "$LOCK"
}
trap cleanup EXIT

# --- the check
line="$("$CHECK")"; rc=$?
kind="${line%% *}"; tag="${line#* }"
case "$rc" in
  0) finish 0 "current: vendored KiCad $(sed -n 's/^tag //p' "$ROOT/vendor/kicad.manifest" 2>/dev/null) is the newest stable release" ;;
  10|11) ;;
  *) finish "$rc" "check failed: $line" ;;
esac
case "$tag" in *[!0-9.]*|"") finish 3 "check printed an unexpected line: $line" ;; esac
REPORT="reports/kicad-$tag.md"
BRANCH="update/kicad-$tag"

if [ "$kind" = "major-release" ]; then
  [ $DRY -eq 1 ] && { echo "would write $REPORT (a port to $tag is needed) after a dry-run update"; exit 0; }
  out="$CACHE/watch-$tag.log"; mkdir -p "$CACHE"
  (cd "$ROOT" && "$UPDATE" --dry-run --try-build "$tag") >"$out" 2>&1; urc=$?
  "$PY" "$ROOT/tools/watch_report.py" --tag "$tag" --kind major --update-log "$out" --update-rc "$urc" \
    >"$ROOT/$REPORT" || finish 5 "major-release $tag: the report could not be written"
  finish 0 "major-release $tag: a port is needed; $REPORT (dry-run update exit $urc)"
fi

# --- a patch release: prepare a branch, never merge
for ref in "refs/heads/$BRANCH" "refs/remotes/origin/$BRANCH"; do
  if git -C "$ROOT" rev-parse --verify --quiet "$ref" >/dev/null; then
    finish 0 "patch-release $tag: branch $BRANCH already exists ($ref); nothing done"
  fi
done
[ $DRY -eq 1 ] && { echo "would create $BRANCH from $BASE, run $UPDATE $tag there and commit $REPORT"; exit 0; }

mkdir -p "$CACHE"
WT="$CACHE/watch-worktree-$tag"
rm -rf "$WT"; git -C "$ROOT" worktree prune
git -C "$ROOT" worktree add --quiet -b "$BRANCH" "$WT" "$BASE" || finish 5 "patch-release $tag: could not create $BRANCH from $BASE"

before="$CACHE/watch-before-$tag"; rm -rf "$before"; mkdir -p "$before"
if [ -z "${TYPMAX_WATCH_UPDATE:-}" ]; then
  # the base, measured on the same box just before the update
  if cmake -S "$WT" -B "$before/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$before/build.log" 2>&1 \
     && ninja -C "$before/build" -j"${TYPMAX_JOBS:-4}" >>"$before/build.log" 2>&1; then
    "$PY" "$WT/tests/run_tests.py" --binary "$before/build/typmax-router" --record-golden "$before/golden.jsonl" >/dev/null 2>&1
    if [ -n "${TYPMAX_FIXTURES_DIR:-}" ]; then
      TYPMAX_FIXTURES_OUT="$before/fixtures" "$PY" "$WT/tests/run_tests.py" --binary "$before/build/typmax-router" \
        --suite fixtures >"$before/fixtures.log" 2>&1
    fi
  fi
fi

out="$CACHE/watch-$tag.log"
mkdir -p "$WT/reports"
(cd "$WT" && "$UPDATE" "$tag") >"$out" 2>&1; urc=$?
"$PY" "$ROOT/tools/watch_report.py" --tag "$tag" --kind patch --branch "$BRANCH" --update-log "$out" --update-rc "$urc" \
  --gates "$WT/build/kicad-update" --before "$before" --golden-old "$WT/tests/golden/responses.jsonl" \
  >"$WT/$REPORT" || finish 5 "patch-release $tag: the report could not be written"
cp "$WT/$REPORT" "$ROOT/$REPORT"
# the new golden lines, for a second platform to compare against (the CI's macOS gate)
[ -f "$WT/build/kicad-update/golden.jsonl" ] && cp "$WT/build/kicad-update/golden.jsonl" "$ROOT/reports/kicad-$tag.golden.jsonl"
for p in vendor SOURCE.md; do [ -e "$WT/$p" ] && git -C "$WT" add -A -- "$p"; done
git -C "$WT" add -f "$REPORT"
git -C "$WT" commit --quiet --signoff -m "chore(vendor): KiCad $tag (prepared by tools/kicad_watch.sh; not reviewed)" \
  -m "update_kicad.sh exit $urc. $REPORT has the gates, the patches and the golden-line diff. Golden lines and engine.version are left for the reviewer (README.md, \"Updating to a new KiCad release\")." \
  || finish 5 "patch-release $tag: nothing to commit on $BRANCH"
[ "$urc" -eq 0 ] && finish 0 "patch-release $tag: branch $BRANCH ready for review, gates green; $REPORT"
finish 5 "patch-release $tag: branch $BRANCH needs work (update_kicad.sh exit $urc); $REPORT"
