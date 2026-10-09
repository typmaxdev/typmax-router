#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
#
# tools/update_kicad.sh — move vendor/kicad/ to a KiCad release tag.
#
#   tools/update_kicad.sh [--dry-run] [--no-gates] [--try-build] <tag>
#
# 1. fetch: a blobless, sparse, depth-1 clone of the tag into the cache
#    ($TYPMAX_KICAD_CACHE, default ${TMPDIR:-/tmp}/typmax-kicad-cache), only the
#    paths tools/kicad_closure.txt lists. Nothing outside the cache and this
#    repository is written.
# 2. stage: copy those files byte for byte into a staging tree; a listed path
#    the tag does not have stops the update, named.
# 3. patches: apply patches/series in order to a scratch copy of the staged
#    tree; the first patch that does not apply stops the update, named
#    (exit 3). vendor/kicad/ itself is never patched — CMake applies the
#    series to a copy in the build directory.
# 4. vendor: replace vendor/kicad/ with the staged tree and rewrite
#    vendor/kicad.manifest (tag, commit, sha256 and licence of every file) and
#    the generated block of SOURCE.md. Same tag, same files: nothing changes,
#    and the run says so and stops (a no-op).
# 5. gates (unless --no-gates): configure + build (ninja -j4), ctest synthetic
#    and determinism, the fixture suite when TYPMAX_FIXTURES_DIR is set, and a
#    golden-line diff written to build/kicad-update/golden.diff. The golden
#    lines are never re-recorded here: MAINTAINING.md "Updating to a new KiCad
#    release" says how to review and re-record them.
#
# --dry-run: steps 1-3 and a report of what step 4 would change; the
#            repository is not touched. --try-build adds a scratch build of
#            the staged tree (in the cache) to a dry run.
#
# Exit codes: 0 done (or nothing to do), 1 usage/environment, 2 fetch or
# stage failed, 3 a patch does not apply, 4 build failed, 5 a gate failed.
set -euo pipefail

usage() { sed -n '5,8p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

DRY=0; GATES=1; TRY_BUILD=0; TAG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY=1 ;;
    --no-gates) GATES=0 ;;
    --try-build) TRY_BUILD=1 ;;
    -h|--help) usage ;;
    -*) echo "update_kicad: unknown option $1" >&2; usage ;;
    *) [ -z "$TAG" ] || usage; TAG="$1" ;;
  esac
  shift
done
[ -n "$TAG" ] || usage
case "$TAG" in
  *[!0-9A-Za-z._-]*|-*) echo "update_kicad: not a tag name: $TAG" >&2; exit 1 ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="${TYPMAX_KICAD_CACHE:-${TMPDIR:-/tmp}/typmax-kicad-cache}"
CACHE="${CACHE%/}"
URL="${TYPMAX_KICAD_URL:-https://gitlab.com/kicad/code/kicad.git}"
CLOSURE="$ROOT/tools/kicad_closure.txt"
SERIES="$ROOT/patches/series"
VENDOR="$ROOT/vendor/kicad"
MANIFEST="$ROOT/vendor/kicad.manifest"
JOBS="${TYPMAX_JOBS:-4}"

say() { printf 'update_kicad: %s\n' "$*"; }
die() { local code="$1"; shift; printf 'update_kicad: %s\n' "$*" >&2; exit "$code"; }

if command -v sha256sum >/dev/null 2>&1; then sha256() { sha256sum "$1" | cut -d' ' -f1; }
else sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }; fi

closure_paths() { grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$CLOSURE"; }
series_patches() { [ -f "$SERIES" ] && grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$SERIES" || true; }

# --- 1. fetch ---------------------------------------------------------------
mkdir -p "$CACHE"
SRC="$CACHE/kicad-$TAG"
if [ ! -d "$SRC/.git" ]; then
  say "fetching $TAG from $URL into $SRC"
  rm -rf "$SRC.partial"
  # (git warns that an annotated tag "is not a commit"; it clones the tagged commit)
  git clone --quiet --filter=blob:none --no-checkout --depth 1 --branch "$TAG" "$URL" "$SRC.partial" 2>"$CACHE/clone.log" \
    || { cat "$CACHE/clone.log" >&2; die 2 "could not fetch tag $TAG from $URL"; }
  mv "$SRC.partial" "$SRC"
fi
# the sparse set is re-applied every run, so an edited closure is honoured
git -C "$SRC" sparse-checkout init --no-cone >/dev/null 2>&1 || true
closure_paths | sed 's|^|/|' | git -C "$SRC" sparse-checkout set --no-cone --stdin \
  || die 2 "sparse checkout failed in $SRC"
git -C "$SRC" checkout --quiet || die 2 "checkout of $TAG failed in $SRC"
COMMIT="$(git -C "$SRC" rev-parse HEAD)"
TAG_COMMIT="$(git -C "$SRC" rev-parse "refs/tags/$TAG^{commit}" 2>/dev/null || echo "$COMMIT")"
[ "$COMMIT" = "$TAG_COMMIT" ] || die 2 "cache $SRC is at $COMMIT, not at tag $TAG ($TAG_COMMIT); delete it and re-run"
COMMIT_DATE="$(git -C "$SRC" log -1 --format=%cs HEAD)"
say "tag $TAG = commit $COMMIT ($COMMIT_DATE)"

# --- 2. stage ---------------------------------------------------------------
STAGE="$CACHE/stage-$TAG"
rm -rf "$STAGE"; mkdir -p "$STAGE/kicad"
missing=0
while IFS= read -r p; do
  if [ ! -f "$SRC/$p" ]; then echo "update_kicad: $TAG has no $p (listed in tools/kicad_closure.txt)" >&2; missing=1; continue; fi
  mkdir -p "$STAGE/kicad/$(dirname "$p")"
  cp -p "$SRC/$p" "$STAGE/kicad/$p"
done < <(closure_paths)
if [ $missing -ne 0 ]; then
  [ $DRY -eq 1 ] || die 2 "the closure names files $TAG does not have; edit tools/kicad_closure.txt (MAINTAINING.md)"
  say "dry run: continuing without the missing files (a real update stops here)"
fi

licence_of() {  # a file's licence, read from its header (a disclosure, not a legal opinion)
  local f="$1" flat lic extra=""
  case "$f" in
    */LICENSE*|*/README.txt|LICENSE*) echo "text"; return ;;
  esac
  # the first 60 lines, comment furniture and line breaks folded into single spaces
  flat="$(head -60 "$STAGE/kicad/$f" | tr '\n' ' ' | sed 's/[[:space:]*/]\{1,\}/ /g')"
  case "$flat" in
    *"SPDX-License-Identifier: "*) lic="$(printf '%s' "$flat" | sed 's/.*SPDX-License-Identifier: \([A-Za-z0-9.+-]*\).*/\1/')" ;;
    *"either version 3"*|*"version 3 of the License"*) lic="GPL-3.0-or-later" ;;
    *"either version 2"*|*"version 2 of the License"*) lic="GPL-2.0-or-later" ;;
    *"Boost Software License"*|*"boost.org/LICENSE_1_0"*) lic="BSL-1.0" ;;
    # a GPL header that names no version this reader knows is not guessed (review r3
    # R-2): it is NOASSERTION, and the update says so below, for a person to read
    *) case "$f" in
         thirdparty/clipper2/*) lic="BSL-1.0" ;;           # Clipper2's own LICENSE beside it
         thirdparty/rtree/*) lic="GPL-3.0-or-later" ;;     # KiCad's relicensing, its README.txt
         *) lic="NOASSERTION" ;;
       esac ;;
  esac
  # portions under another licence, as KiCad's LICENSE.README lists them
  case "$flat" in *"Apache 2.0 license"*|*"Apache License"*) extra="$extra+Apache-2.0" ;; esac
  case "$flat" in *"ISC License"*|*", ISC "*) extra="$extra+ISC" ;; esac
  echo "$lic$extra"
}

NEW_MANIFEST="$STAGE/kicad.manifest"
{
  echo "# Generated by tools/update_kicad.sh; do not edit. SPDX-License-Identifier: MIT"
  echo "tag $TAG"
  echo "commit $COMMIT"
  echo "commit-date $COMMIT_DATE"
  echo "source $URL"
  echo "files $(closure_paths | wc -l | tr -d ' ')"
  echo "# sha256 licence path  (licence: read from the file's header; A+B = the file is A with a portion under B)"
  while IFS= read -r p; do
    [ -f "$STAGE/kicad/$p" ] || continue
    printf '%s %s %s\n' "$(sha256 "$STAGE/kicad/$p")" "$(licence_of "$p")" "$p"
  done < <(closure_paths | LC_ALL=C sort)
} > "$NEW_MANIFEST"
unknown="$(awk '$2 ~ /NOASSERTION/ {print $3}' "$NEW_MANIFEST")"
if [ -n "$unknown" ]; then
  say "WARNING: no licence could be read from these files' headers (NOASSERTION in the manifest); read them and say what they are in SOURCE.md before committing:"
  printf '  %s\n' $unknown
fi

# --- 3. patches (on a scratch copy) -----------------------------------------
check_patches() {
  local tree="$1" scratch="$STAGE/patched"
  rm -rf "$scratch"; cp -R "$tree" "$scratch"
  local n=0 p
  while IFS= read -r p; do
    n=$((n + 1))
    [ -f "$ROOT/patches/$p" ] || die 3 "patches/series names $p, which does not exist"
    if ! patch -d "$scratch" -p1 --forward --silent --batch -r - < "$ROOT/patches/$p" >"$STAGE/patch.log" 2>&1; then
      cat "$STAGE/patch.log" >&2
      die 3 "patch $n ($p) does not apply to $TAG: rebase it onto the new upstream file, or drop it if $TAG fixed the reason (MAINTAINING.md)"
    fi
    say "patch $n applies: $p"
  done < <(series_patches)
  [ $n -gt 0 ] || say "patches/series is empty: nothing to apply"
}
check_patches "$STAGE/kicad"

# --- what would change --------------------------------------------------------
changed=0; added=0; removed=0
if [ -f "$MANIFEST" ]; then
  OLD_TAG="$(sed -n 's/^tag //p' "$MANIFEST")"; OLD_COMMIT="$(sed -n 's/^commit //p' "$MANIFEST")"
  diff_out="$(diff <(grep -v '^#' "$MANIFEST" | awk 'NF==3{print $3, $1}' | LC_ALL=C sort) \
                   <(grep -v '^#' "$NEW_MANIFEST" | awk 'NF==3{print $3, $1}' | LC_ALL=C sort) || true)"
  changed="$(join <(grep -v '^#' "$MANIFEST" | awk 'NF==3{print $3, $1}' | LC_ALL=C sort) \
                  <(grep -v '^#' "$NEW_MANIFEST" | awk 'NF==3{print $3, $1}' | LC_ALL=C sort) | awk '$2!=$3' | wc -l | tr -d ' ')"
  added="$(comm -13 <(grep -v '^#' "$MANIFEST" | awk 'NF==3{print $3}' | LC_ALL=C sort) <(grep -v '^#' "$NEW_MANIFEST" | awk 'NF==3{print $3}' | LC_ALL=C sort) | wc -l | tr -d ' ')"
  removed="$(comm -23 <(grep -v '^#' "$MANIFEST" | awk 'NF==3{print $3}' | LC_ALL=C sort) <(grep -v '^#' "$NEW_MANIFEST" | awk 'NF==3{print $3}' | LC_ALL=C sort) | wc -l | tr -d ' ')"
  say "vendor/kicad: ${OLD_TAG:-none} (${OLD_COMMIT:0:12}) -> $TAG (${COMMIT:0:12}): $changed changed, $added added, $removed removed"
  if [ "$changed" -gt 0 ] && [ $DRY -eq 1 ]; then
    join <(grep -v '^#' "$MANIFEST" | awk 'NF==3{print $3, $1}' | LC_ALL=C sort) \
         <(grep -v '^#' "$NEW_MANIFEST" | awk 'NF==3{print $3, $1}' | LC_ALL=C sort) | awk '$2!=$3{print "  changed: " $1}'
  fi
else
  say "vendor/kicad: none -> $TAG"
fi

if [ $DRY -eq 1 ]; then
  if [ $TRY_BUILD -eq 1 ]; then
    TB="$CACHE/trybuild-$TAG"; rm -rf "$TB"; mkdir -p "$TB/src"
    (cd "$ROOT" && tar cf - CMakeLists.txt src shim patches tests tools PROTOCOL.md) | (cd "$TB/src" && tar xf -)
    mkdir -p "$TB/src/vendor"; cp -R "$STAGE/kicad" "$TB/src/vendor/kicad"; cp "$NEW_MANIFEST" "$TB/src/vendor/kicad.manifest"
    if cmake -S "$TB/src" -B "$TB/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$TB/build.log" 2>&1 \
       && ninja -C "$TB/build" -j"$JOBS" >>"$TB/build.log" 2>&1; then
      say "try-build: $TAG builds ($TB/build/typmax-router)"
    else
      grep -m5 -E 'error:|fatal' "$TB/build.log" >&2 || true
      die 4 "try-build: $TAG does not build with this repository's shims; log: $TB/build.log"
    fi
  fi
  say "dry run: the repository was not touched"
  exit 0
fi

# --- 4. vendor ---------------------------------------------------------------
write_source_block() {  # the generated block of SOURCE.md, between its markers
  local src="$ROOT/SOURCE.md"
  [ -f "$src" ] || return 0
  python3 - "$src" "$TAG" "$COMMIT" "$COMMIT_DATE" "$URL" "$(closure_paths | wc -l | tr -d ' ')" "$(series_patches | wc -l | tr -d ' ')" <<'PY'
import sys, re
path, tag, commit, date, url, nfiles, npatches = sys.argv[1:]
s = open(path, encoding="utf-8").read()
block = (f"<!-- BEGIN update_kicad.sh (generated; do not edit) -->\n"
         f"- **Tag:** `{tag}`\n- **Commit:** `{commit}` ({date})\n- **Repository:** {url}\n"
         f"- **Files:** {nfiles} (tools/kicad_closure.txt), each with its sha256 and licence in "
         f"`vendor/kicad.manifest`\n- **Patches:** {npatches} (patches/series)\n"
         f"<!-- END update_kicad.sh -->")
new, n = re.subn(r"<!-- BEGIN update_kicad\.sh.*?<!-- END update_kicad\.sh -->", block, s, flags=re.S)
if n != 1:
    sys.exit("SOURCE.md has no update_kicad.sh block (its BEGIN/END markers)")
if new != s:
    open(path, "w", encoding="utf-8").write(new)
PY
}

same=0
if [ -f "$MANIFEST" ] && cmp -s "$MANIFEST" "$NEW_MANIFEST" && [ -d "$VENDOR" ]; then
  # the manifest matches: are the vendored bytes themselves untouched?
  if (cd "$VENDOR" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) | cmp -s - <(closure_paths | LC_ALL=C sort) \
     && diff -rq "$VENDOR" "$STAGE/kicad" >/dev/null; then
    same=1
  fi
fi
if [ $same -eq 1 ]; then
  write_source_block
  say "vendor/kicad is already $TAG ($COMMIT), byte for byte: nothing to do"
  [ $GATES -eq 1 ] && [ "${TYPMAX_GATES_ON_NOOP:-0}" = 1 ] || exit 0
else
  rm -rf "$VENDOR"; mkdir -p "$(dirname "$VENDOR")"
  cp -R "$STAGE/kicad" "$VENDOR"
  cp "$NEW_MANIFEST" "$MANIFEST"
  write_source_block
  say "vendor/kicad is now $TAG ($COMMIT); review with: git status vendor/ && git diff --stat"
fi
[ $GATES -eq 1 ] || exit 0

# --- 5. gates ----------------------------------------------------------------
B="$ROOT/build"; OUT="$B/kicad-update"; mkdir -p "$OUT"
say "building (ninja -j$JOBS)"
if ! { cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C "$B" -j"$JOBS"; } >"$OUT/build.log" 2>&1; then
  grep -m10 -E 'error:|fatal|CMake Error' "$OUT/build.log" >&2 || true
  die 4 "build failed with $TAG; log: $OUT/build.log. A missing header is a closure or shim question (MAINTAINING.md)"
fi
PY3="$(command -v python3.11 || command -v python3)"
fail=0
# determinism here is within and across processes; the committed golden lines are
# compared below as a DIFF, since a new release is expected to move routes
for suite in synthetic determinism; do
  sel=(--suite "$suite"); [ "$suite" = determinism ] && sel=(--only t_determinism)
  if "$PY3" "$ROOT/tests/run_tests.py" --binary "$B/typmax-router" "${sel[@]}" >"$OUT/$suite.log" 2>&1; then
    say "gate $suite: PASS"; echo "PASS" >"$OUT/$suite.status"
  else
    say "gate $suite: FAIL (log: $OUT/$suite.log)"; echo "FAIL" >"$OUT/$suite.status"; fail=1
  fi
done
if [ -n "${TYPMAX_FIXTURES_DIR:-}" ]; then
  if TYPMAX_FIXTURES_OUT="$OUT/fixtures" "$PY3" "$ROOT/tests/run_tests.py" --binary "$B/typmax-router" --suite fixtures >"$OUT/fixtures.log" 2>&1; then
    say "gate fixtures: PASS"; echo "PASS" >"$OUT/fixtures.status"
  else
    say "gate fixtures: FAIL (log: $OUT/fixtures.log)"; echo "FAIL" >"$OUT/fixtures.status"; fail=1
  fi
else
  say "gate fixtures: skipped (TYPMAX_FIXTURES_DIR unset)"; echo "SKIP" >"$OUT/fixtures.status"
fi
# golden lines: record the new binary's lines beside the committed ones, never over them
if "$PY3" "$ROOT/tests/run_tests.py" --binary "$B/typmax-router" --record-golden "$OUT/golden.jsonl" >"$OUT/golden.log" 2>&1; then
  if diff "$ROOT/tests/golden/responses.jsonl" "$OUT/golden.jsonl" >"$OUT/golden.diff"; then
    say "golden lines: unchanged"; echo "SAME" >"$OUT/golden.status"
  else
    say "golden lines: $(grep -c '^>' "$OUT/golden.diff") of $(wc -l <"$OUT/golden.jsonl" | tr -d ' ') differ ($OUT/golden.diff): classify them, then re-record (MAINTAINING.md)"
    echo "DIFF" >"$OUT/golden.status"
  fi
else
  say "golden lines: could not record (log: $OUT/golden.log)"; echo "FAIL" >"$OUT/golden.status"; fail=1
fi
[ $fail -eq 0 ] || die 5 "a gate failed"
say "done: vendor/kicad at $TAG, gates green; commit it (MAINTAINING.md)"
