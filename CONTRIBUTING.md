<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 The typmax-router authors -->
# Contributing

Thanks for your interest. Bug reports, reduced reproductions and patches are welcome.
For anything larger than a fix (a new protocol method, a change to a guarantee in
PROTOCOL.md, a new tool), please open an issue first so the shape can be agreed
before you write it.

By participating you agree to the [Code of Conduct](CODE_OF_CONDUCT.md). Security
problems go through private reporting ([SECURITY.md](SECURITY.md)), never a public
issue.

## Sign your commits off (DCO)

This project uses the [Developer Certificate of Origin](https://developercertificate.org/)
1.1 instead of a CLA: every commit carries a `Signed-off-by:` line with your real name
and an e-mail address you can be reached at, which certifies that you wrote the change
or otherwise have the right to submit it under the licence of the files it touches.

```sh
git commit -s -m "fix(engine): ..."
```

The DCO check on every pull request (the [DCO GitHub App](https://github.com/apps/dco))
fails while any commit in it lacks the line. To add it to commits you already made:
`git rebase --signoff main`, then force-push your branch.

Contributions are accepted under the licence each file already states in its
`SPDX-License-Identifier` line (README.md, "Licences"): GPL-3.0-or-later for the
service, the patches and the build; MIT for `tools/`, `tests/`, `docker/` and
`.github/`; CC0-1.0 for PROTOCOL.md. A new file states its licence on its first lines
(or gets an entry in REUSE.toml if it cannot carry a comment); `t_spdx_headers` fails
otherwise.

## Building and testing

README.md, "Build" and "Test", has the commands. Before you open a pull request:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The `ci` workflow runs the synthetic, determinism and watcher suites on every pull
request (Ubuntu, clang). The fixture suite needs KiCad's demo boards, which are
CC BY-SA 4.0 and **must never be committed**: `tools/fetch_fixtures.sh` downloads them
into the gitignored `fixtures/` directory.

The determinism suite compares every answer with `tests/golden/responses.jsonl`. If
your change moves a route on purpose, re-record the lines
(`python3 tests/run_tests.py --binary build/typmax-router --update-golden`), read the
diff with `tools/golden_diff.py`, say in the commit body which lines changed and why
(better, equivalent or worse), and bump `SERVICE_VERSION` in `src/main.cpp`.

## Changing KiCad's code (`vendor/kicad/`)

`vendor/kicad/` is a byte-for-byte copy of files from a KiCad release tag
(`vendor/kicad.manifest` records each file's sha256). **Never edit it directly**, and a
pull request that does will be asked to move the change. There are two ways to change
what is built:

- **A patch**, for a change to KiCad's own behaviour: a unified diff in `patches/`,
  listed in `patches/series`, with the header and the dated GPL-3.0 section 5(a)
  "Modified by" notice that [patches/README.md](patches/README.md) describes
  (`t_patch_notices` checks it). CMake applies the series to a copy in the build
  directory; `tools/update_kicad.sh` re-checks every patch against each new release.
  If the fix belongs upstream, say so in the patch's `Upstream:` line and, ideally,
  propose it to KiCad as well.
- **A stand-in** under `shim/`, for the parts of KiCad the router includes but the
  service replaces (the board model, wxWidgets, settings). SOURCE.md lists each one.

Moving to a new KiCad release is done only by `tools/update_kicad.sh <tag>` (README.md,
"Updating to a new KiCad release"); the release watch opens such a pull request on its
own when KiCad tags a patch release.

## Commits and pull requests

- [Conventional commits](https://www.conventionalcommits.org/): `fix(engine): …`,
  `feat(tools): …`, `docs: …`, `test: …`, `ci: …`; imperative mood, a subject under
  72 characters, the reason in the body.
- One logical change per commit; a vendor update, its patch changes, its golden lines
  and the version bump are separate commits.
- Pull requests are merged with a merge commit once the `ci` check is green and a
  maintainer has reviewed them.
