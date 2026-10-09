<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 The typmax-router authors -->
# typmax-router

A PCB routing service built on [KiCad](https://www.kicad.org/)'s push-and-shove
router. A client loads a board as JSON, asks for a route or a drag, and gets back a
**proposal** (the tracks and vias to remove and add), which it may `apply` to chain
further requests. It speaks JSON Lines on stdin/stdout; [PROTOCOL.md](PROTOCOL.md)
is the contract.

- Walkaround, shove and mark-obstacles modes; drags; vias at waypoints; arcs; rules
  per net class plus edge, hole and hole-to-hole clearances.
- Deterministic: the same requests give byte-identical answers across runs and
  between macOS arm64 and Linux x86_64 (a CI gate).
- Errors, never crashes: one answer line per request, with a stable error code; a
  per-request time limit.

**Status:** early, pre-1.0, no tagged releases yet (build from `main`). The protocol
is versioned (`protocol_version` 2); only its guarantees table is stable.

## Build

C++20 compiler, CMake ≥ 3.16, `patch`; macOS or Linux.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build            # -> build/typmax-router
```

On macOS, if the newest Command Line Tools SDK fails to link, name another with
`-DTYPMAX_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX<version>.sdk`.
Or build for Linux in Docker (the image build runs the tests):

```sh
docker build --platform linux/amd64 -f docker/Dockerfile -t typmax-router:linux .
```

## Run

```sh
printf '%s\n' '{"id":1,"protocol_version":2,"method":"ping"}' | build/typmax-router
```

Options: `--time-limit-ms N`, `--max-line-bytes N`. `tools/router_client.py` is a
small Python client; `tools/kicad_to_board.py` turns a `.kicad_pcb` into the board
JSON and `tools/apply_to_kicad.py` writes a proposal back into a copy of one.

## Test

```sh
ctest --test-dir build --output-on-failure
```

[MAINTAINING.md](MAINTAINING.md) has the suites in detail, the tools, and how a new
KiCad release is taken in.

## Credit: KiCad

The routing is done entirely by KiCad's push-and-shove router (`pcbnew/router/`, by
Tomasz Wlostowski / CERN and the KiCad developers) and the geometry libraries it
uses, vendored unmodified from the KiCad release that [SOURCE.md](SOURCE.md) names.
This project adds the protocol, a board model that stands in for KiCad's, and a few
small fixes carried as patches (`patches/`). It is not affiliated with or endorsed by
the KiCad project; please report bugs in this service here, not to KiCad.

## Licences

The service binary is GPL-3.0-or-later; the protocol, tools and tests are not. Every
file names its licence in an `SPDX-License-Identifier` line (or in
[REUSE.toml](REUSE.toml)); the texts are in [LICENSES/](LICENSES/).

| Paths | Licence |
|---|---|
| `vendor/kicad/` | as each file says, listed in `vendor/kicad.manifest` (mostly GPL-3.0/2.0-or-later; Clipper2 BSL-1.0) |
| `src/`, `shim/`, `patches/`, `CMakeLists.txt`, the binary | GPL-3.0-or-later |
| `PROTOCOL.md` | CC0-1.0: anyone may implement it |
| `tools/`, `tests/`, `docker/`, `.github/` and repository config | MIT |
| `CODE_OF_CONDUCT.md` | CC-BY-4.0 (Contributor Covenant 2.1) |
| other documents | GPL-3.0-or-later |

A client talks to the service only through the protocol, so no GPL code needs to
enter the client. KiCad's demo boards used by the fixture tests (CC BY-SA 4.0) are
downloaded, never committed.

## Contributing and security

See [CONTRIBUTING.md](CONTRIBUTING.md) (DCO sign-off required; KiCad's code changes
only as a patch in `patches/`). Report vulnerabilities privately
([SECURITY.md](SECURITY.md)). Participation is under the
[Code of Conduct](CODE_OF_CONDUCT.md).
