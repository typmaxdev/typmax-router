<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 The typmax-router authors -->
# typmax-router

A server-side PCB routing service built on KiCad's push-and-shove router (PNS),
taken from a KiCad release (10.0.7; SOURCE.md). A client loads a board as neutral
JSON, asks for a route or a drag, and gets back a **proposal** — the tracks and vias
to remove and add — which it may `apply` to chain further requests. The service
speaks JSON Lines on stdin/stdout; [PROTOCOL.md](PROTOCOL.md) is the contract.

## Status

Early and pre-1.0. The protocol is versioned (`protocol_version`, currently 2;
PROTOCOL.md, "Versioning" and "Changelog") and its guarantees table is what a client
may rely on; anything not in that table may change between commits. The service
version (`engine.version` in every answer) moves whenever the same request could get
a different answer. There are no tagged releases yet: build from `main`. Every pull
request is built and tested on GitHub's Ubuntu runners (`.github/workflows/ci.yml`:
the synthetic, determinism and watcher suites).

## Credit: KiCad

The routing is done entirely by [KiCad](https://www.kicad.org/)'s push-and-shove
router (`pcbnew/router/`, by Tomasz Wlostowski / CERN and the KiCad developers) and
the geometry libraries it uses (kimath, Clipper2, an R-tree), taken unmodified from
the KiCad release tag that SOURCE.md and `vendor/kicad.manifest` name. This project
adds the protocol, a board model that stands in for KiCad's, and four small fixes
carried as patches (`patches/`). It is not affiliated with or endorsed by the KiCad
project; bugs in this service should be reported here, not to KiCad.

## Licences

The service binary is GPL; the protocol, the tools and the tests are not. Each file
names its licence in an `SPDX-License-Identifier` line (files that cannot carry one
are listed in [REUSE.toml](REUSE.toml); `vendor/kicad/` files keep KiCad's own headers, listed in
`vendor/kicad.manifest`); the texts are in [LICENSES/](LICENSES/). `t_spdx_headers`
holds this for every tracked file.

| Paths | Licence | Why |
|---|---|---|
| `vendor/kicad/` | as each file says (`vendor/kicad.manifest` lists them): KiCad's router GPL-3.0-or-later, most of kimath GPL-2.0-or-later (one file with an Apache-2.0 portion, two with ISC portions), Clipper2 BSL-1.0, the R-tree GPL-3.0 | KiCad's, byte for byte (SOURCE.md); never relicensed |
| `src/`, `shim/`, `patches/`, `CMakeLists.txt` | GPL-3.0-or-later | they link, build or modify PNS; `shim/time_limit.cpp` is a KiCad-derived file |
| the `typmax-router` binary and any image holding it | GPL-3.0-or-later (the combined work, [LICENSE](LICENSE)) | |
| `PROTOCOL.md` | CC0-1.0 | the contract a client codes against: anyone may implement it |
| `tools/`, `tests/` (code, boards, golden lines), `docker/`, `.github/`, `.gitignore`, `.gitattributes`, `.dockerignore`, `REUSE.toml` | MIT | ours, linking nothing from PNS: a client may copy or imitate them |
| `CODE_OF_CONDUCT.md` | CC-BY-4.0 | the Contributor Covenant 2.1, adapted only in its contact line |
| the other documents (README, SOURCE, CONTRIBUTING, SECURITY, `docs/`) | GPL-3.0-or-later (the repository default) | |

Nothing under an MIT or CC0 path was copied or adapted from KiCad;
`tools/kicad_to_board.py` reads KiCad's file format and re-implements its rotation
convention from the format's documentation, not from KiCad code.

- It runs **only as a server-side subprocess**. Its binary is never distributed to
  users (no download, no WASM bundle, no desktop app), so nothing is conveyed. The
  runtime image (`docker/Dockerfile`) holds the GPL binary: it stays in a **private**
  registry or on the host it was built for; pushing it anywhere a third party can pull
  it is conveying and must come with the source (this repository at the image's
  commit).
- A client talks to it only through the documented message protocol (neutral JSON,
  never PNS internal structures); no GPL code from here needs to enter the client. A
  client may copy or imitate the MIT tools and implement the CC0 protocol.
- Test fixtures from KiCad's demos (CC BY-SA 4.0) are **never committed**:
  `tools/fetch_fixtures.sh` downloads them into the gitignored `fixtures/`.

## Build

Native (macOS or Linux; a C++20 compiler (tested: Apple clang 21, Debian clang 14), CMake ≥ 3.16,
`patch`, Ninja optional):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build -j4          # -> build/typmax-router (one binary, ~1.9 MB, 1.6 MB stripped)
```

CMake applies `patches/series` to a copy of `vendor/kicad/` in the build directory
(`build/kicad-patched/`) at configure time, and re-applies it when the series, a patch
or the manifest changes. On macOS, if the default SDK fails to link (the stubs of a
newest Command Line Tools SDK do on some installs), name another one:
`-DTYPMAX_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX<version>.sdk`
(or the same path in the `TYPMAX_OSX_SYSROOT` environment variable). Linux x86_64,
built and tested in Docker:

```sh
docker build --platform linux/amd64 -f docker/Dockerfile -t typmax-router:linux .
docker run --rm -i --platform linux/amd64 typmax-router:linux < requests.jsonl
```

The image build runs the synthetic and determinism suites; a red test fails it.

## Run

```sh
build/typmax-router [--time-limit-ms 10000] [--max-line-bytes N]
```

```sh
printf '%s\n' '{"id":1,"protocol_version":2,"method":"ping"}' | build/typmax-router
```

`tools/router_client.py` is a small stdlib client (spawn, request, call).

## Tools (python3, stdlib only, and bash)

- `tools/kicad_to_board.py board.kicad_pcb -o board.json` — a .kicad_pcb (KiCad
  6–10) to the board JSON: copper layers, Edge.Cuts, pads, tracks, arcs, vias,
  keepout zones, copper text/graphics, net classes from the .kicad_pro beside it
  (or KiCad's defaults). Copper zone fills are not converted (the protocol does not
  model them). A zero-length segment becomes a `copper` disc; the board-level
  minimums in the .kicad_pro floor every net class; a `.kicad_dru` beside it is
  warned about.
- `tools/apply_to_kicad.py in.kicad_pcb proposal.json out.kicad_pcb` — writes a
  proposal (or a list of them, composed) into a **copy** of a board so kicad-cli can
  check it.
- `tools/fetch_fixtures.sh` — downloads the CC BY-SA demo boards for the fixture
  tests.
- `tools/update_kicad.sh <tag>`, `tools/check_kicad_release.sh`,
  `tools/kicad_watch.sh`, `tools/kicad_cli_docker.sh`, `tools/golden_diff.py`,
  `tools/watch_report.py` — the KiCad update path, below.

## Test

```sh
ctest --test-dir build --output-on-failure    # synthetic, determinism, fixtures, watcher
python3 tests/run_tests.py --binary build/typmax-router --suite synthetic
```

- `synthetic`: seven boards of our own in `tests/boards/` (walkaround, shove, via,
  unroutable, power, holes, arc — regenerate with `tests/gen_boards.py`), the
  protocol's error paths, apply/chain, drag, free-point starts, the timeout and crash
  paths, and a regression test for each defect found in review;
  every proposal is checked by an independent rule checker (`tests/drc_lite.py`:
  copper, hole-to-hole, hole and edge clearance), which a self-test proves can fail.
- `determinism`: the same requests twice in one process and in two processes,
  byte-identical, and equal to the committed golden lines
  `tests/golden/responses.jsonl` — the same file gates the docker linux/amd64 build,
  so a macOS/Linux difference is a red test. After a deliberate change:
  `run_tests.py --binary … --update-golden`, then read the diff
  (`tools/golden_diff.py old.jsonl new.jsonl` sorts it).
- Test hooks (environment, off by default): `TYPMAX_ROUTER_TEST_HOOKS=1` enables two
  crash triggers (`internal_crash` end to end); `TYPMAX_ROUTER_TEST_CLOCK=expired`
  makes every finite engine time limit read as expired (shove must not depend on it).
- `fixtures`: skipped (exit 77) unless `TYPMAX_FIXTURES_DIR` names a directory with
  `pic_programmer.kicad_pcb` and `video.kicad_pcb`. Strips a few nets, re-routes them
  through the service, runs `kicad-cli pcb drc` before and after (no new
  clearance/short-class violation allowed), replays the run in three more processes
  (`TYPMAX_FIXTURES_REPLAYS`; byte-identical), and records latency in
  `build/fixtures-out/<board>-result.json`. `KICAD_CLI` overrides the kicad-cli path.
- `watcher`: the release watcher's decisions against mocked tag lists and a scratch
  repository, and the workflow's shape (offline; `tests/test_watcher.py`).

```sh
tools/fetch_fixtures.sh
TYPMAX_FIXTURES_DIR=$PWD/fixtures ctest --test-dir build -R fixtures --output-on-failure
```

## Updating to a new KiCad release

The router is KiCad's, so a KiCad release is an update of `vendor/kicad/`, done by
`tools/update_kicad.sh` and reviewed by a person. The script never edits our code and
never re-records the golden lines.

1. **Look first** (writes nothing in the repository):

   ```sh
   tools/update_kicad.sh --dry-run --try-build 10.0.8
   ```

   It fetches the tag into the cache (`$TYPMAX_KICAD_CACHE`, default
   `${TMPDIR:-/tmp}/typmax-kicad-cache`; a blobless sparse clone of the listed paths
   only), and prints which vendored files changed, any file the tag no longer has,
   whether every patch still applies, and (with `--try-build`) whether the tag builds
   with our stand-ins.
2. **Update** on a branch (`git switch -c update/kicad-10.0.8`):

   ```sh
   TYPMAX_FIXTURES_DIR=$PWD/fixtures tools/update_kicad.sh 10.0.8
   ```

   It replaces `vendor/kicad/` byte for byte, rewrites `vendor/kicad.manifest` (tag,
   commit, sha256 and licence per file) and the generated block of SOURCE.md, then
   builds and runs the gates: synthetic, determinism (within and across processes),
   fixtures (when `TYPMAX_FIXTURES_DIR` is set), and records the new golden lines in
   `build/kicad-update/golden.jsonl` with a diff beside them. The same tag twice is a
   no-op ("nothing to do"). Exit codes: 2 fetch/stage, 3 a patch does not apply
   (named), 4 build, 5 a gate.
3. **When it stops:**
   - *a patch does not apply* (exit 3): read the upstream change to that file. If the
     release fixed what the patch fixed, delete the patch and its line in
     `patches/series`; otherwise remake it against the new file (patches/README.md).
   - *a file is missing from the tag*, or *the build fails with
     "'<path>' file not found"*: KiCad moved or added an include. If it is KiCad
     code the router needs, add the path to `tools/kicad_closure.txt`; if it drags
     in the board model, wxWidgets or the GUI, add a stand-in under `shim/include/`
     with the minimum surface (SOURCE.md lists the existing ones). A compile error
     *in* a stand-in (a member it lacks) means the core now uses more of it: add that
     member, at KiCad's default where it is a setting (`advanced_config.h`).
   - *a gate fails*: it is a behaviour change of the router; read it like any other.
4. **Golden lines.** They change when routes change:

   ```sh
   python3 tools/golden_diff.py tests/golden/responses.jsonl build/kicad-update/golden.jsonl
   ```

   sorts every line into identical / better / worse / changed. Read every
   non-identical line's geometry and classify it (better, equivalent, worse); a worse
   line is a finding to fix or to accept explicitly. Then bump `SERVICE_VERSION` in
   `src/main.cpp` (the `engine.version` every answer carries: it moves whenever the
   same request could get a different answer), rebuild, and re-record:

   ```sh
   python3 tests/run_tests.py --binary build/typmax-router --update-golden
   ctest --test-dir build --output-on-failure
   docker build --platform linux/amd64 -f docker/Dockerfile -t typmax-router:linux .
   ```

   The docker build compares the same golden file on Linux, so the re-recorded lines
   must come out byte-identical there.
5. **Commit** the vendor update, the patch changes, the golden lines (with the
   classification in the commit body) and the version bump as separate conventional
   commits, and record the fixture numbers before/after in the report of the update.

## Release watch

The watch runs on **GitHub-hosted runners**, not on a developer's machine:
`.github/workflows/kicad-watch.yml` (daily and by hand; the repository's own
`GITHUB_TOKEN`, nothing else; every action pinned by commit SHA).

- **`check`** (ubuntu): `tools/check_kicad_release.sh` asks GitLab's tags API for
  KiCad's tags and prints `current`, `patch-release <tag>` (exit 10) or
  `major-release <tag>` (exit 11) against the vendored tag. Stable means `X.Y.Z` (no
  `-rc`, no `X.99` development series). A failed fetch is exit 3, and a tag list that
  does not contain the vendored tag (empty, another project's, paged past it) is
  exit 6; both are a **red job**, never `current`. The script writes nothing.
- **`update`** (ubuntu, clang, on a patch release): skipped, with the reason in the
  job log, when the branch `update/kicad-<tag>` already exists on the remote or a pull
  request for it exists in any state (open, closed or merged). A rejected update is
  therefore never re-opened, and a branch a person pushed to is never overwritten (the
  push is never forced); to redo a tag, delete its branch. It fetches the fixture
  boards at the vendored tag (`tools/fetch_fixtures.sh` into the runner's
  temp directory; CC BY-SA, never committed) and uses `kicad-cli` from KiCad's official
  image (`kicad/kicad:<tag>`, or `<major.minor>` until the tag's image exists) through
  `tools/kicad_cli_docker.sh`; runs `tools/kicad_watch.sh`, which creates the branch
  `update/kicad-<tag>` from the default branch in a scratch worktree, measures the base,
  runs `tools/update_kicad.sh <tag>` (re-vendor, patches, build, ctest, fixtures,
  golden-line diff) and commits the update with `reports/kicad-<tag>.md`; then the
  linux/amd64 docker gate on the branch, which records the image's own golden lines and
  compares them with the native build's; then pushes the branch and opens a pull request
  with the report as its body. **Nothing is merged**: a person reviews it and finishes
  steps 4–5 of the routine above (classify the golden lines, re-record, bump
  `engine.version`).
- **`macos-gate`** (macos-14, arm64): builds the same branch and records its golden
  lines, which must be byte-identical to both Linux sets from `update` (the Ubuntu clang
  build's and the docker image's); the result is a comment on the pull request. Every
  build the watcher compares is a clang build.
- **`major`** (on a major/minor release): a dry-run update (which files changed, which
  patches still apply, whether it builds) and an issue "KiCad <tag>: port needed" with
  that report, opened once (deduplicated by title).

`tools/kicad_watch.sh` is the script the workflow calls; it also runs locally (one
line per run in `reports/watch.log`, a lock against overlapping runs), but the plan is
the runner. `tests/test_watcher.py` checks its decisions offline and the workflow's
shape (pinned actions, per-job permissions, no `${{ }}` inside a script, no
force-push).

Repository settings and limits the workflow depends on:

- **Pull requests from Actions.** `gh pr create` with the workflow's `GITHUB_TOKEN`
  needs *Settings → Actions → General → Workflow permissions → "Allow GitHub Actions
  to create and approve pull requests"* (and the same at the organisation level, if the
  repository belongs to one). Without it the `update` job fails at that step, red; the
  branch is pushed and the next run skips it, so after enabling the setting open the
  pull request by hand (or delete the branch and re-run).
- **Scheduled runs are not forever.** GitHub disables a workflow's `schedule` trigger
  after 60 days without repository activity in a *public* repository, and says so only
  by e-mail and in the Actions tab; a watch-only repository is exactly the one that goes
  quiet. In this public repository the rule applies: after any quiet stretch, look at
  the Actions tab and re-enable the workflow there. No keep-alive is built in:
  the only one that counts as activity is a commit, and a bot committing to the default
  branch on a timer is noise in the history and needs `contents: write` on every run.

## Contributing and security

[CONTRIBUTING.md](CONTRIBUTING.md) covers the build, the tests, the DCO sign-off and
how a change to KiCad's code is made (only as a patch in `patches/`, never by editing
`vendor/kicad/`). Report vulnerabilities privately ([SECURITY.md](SECURITY.md)).
Participation is under the [Code of Conduct](CODE_OF_CONDUCT.md).
