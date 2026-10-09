<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 The typmax-router authors -->
# SOURCE: where the router comes from

The router is KiCad's own push-and-shove router (PNS: `pcbnew/router/`, by Tomasz
Wlostowski / CERN and the KiCad developers), built from a KiCad release tag with
the parts of KiCad it includes replaced by small stand-ins. Phase 1 prototyped on
Horizon EDA's copy of PNS; it was replaced by KiCad 10.0.7.

## The vendored release

<!-- BEGIN update_kicad.sh (generated; do not edit) -->
- **Tag:** `10.0.7`
- **Commit:** `69cafb907ebc09e7722a2a464b1d466fc2e3608d` (2026-10-06)
- **Repository:** https://gitlab.com/kicad/code/kicad.git
- **Files:** 160 (tools/kicad_closure.txt), each with its sha256 and licence in `vendor/kicad.manifest`
- **Patches:** 4 (patches/series)
<!-- END update_kicad.sh -->

`vendor/kicad/` holds those files **byte for byte**, at their upstream paths:
`pcbnew/router/` (the router core: every `pns_*` file but the GUI tool, the board
interface and the preview items), the `libs/kimath` and `libs/core` headers and
sources it reaches, seven headers of `include/` and `pcbnew/`, Clipper2 and the R-tree
from `thirdparty/`, and the licence texts. `tools/kicad_closure.txt` is the list,
`tools/update_kicad.sh` copies it, and `vendor/kicad.manifest` records every file's
sha256 and the licence its header states. Nothing in `vendor/kicad/` is ever edited:
our changes are `patches/` (applied by CMake to a copy in the build directory) and
`shim/`. README.md, "Updating to a new KiCad release", is the routine.

The closure was measured, not guessed: every router `.cpp` compiled against KiCad's
full tree with the stand-ins first on the include path, the compiler's dependency
files read for the headers each one used, and the linker's undefined symbols followed
from the router objects into kimath and Clipper2.

## What is not KiCad's

### Stand-ins (`shim/`, 28 files, 794 lines)

KiCad's router core includes the board model, wxWidgets, the settings system, the
thread pool and the advanced configuration. `shim/include/` is first on the include
path and stands in for each with the minimum surface the core uses; a header the core
includes but uses nothing from is an empty stand-in that says so.

| Stand-in | For | What it is |
|---|---|---|
| `board_item.h`, `board_connected_item.h`, `pcb_track.h`, `pad.h`, `zone.h`, `netinfo.h`, `board.h`, `padstack.h` | KiCad's board model | `BOARD_ITEM` (type, layer, uuid) and its connected/track/arc/via/pad/zone kinds, the net (`NETINFO_ITEM`: code + name), the padstack enums. `PNS::ITEM::Parent()` is a `BOARD_ITEM*`; `src/engine.cpp` gives every item a parent of the right kind and keeps its own map from parent to board id. `PCB_TRACK`'s solder-mask getters read no state, because `NODE::FixupVirtualVias` casts *every* segment's parent to `PCB_TRACK*` |
| `settings/nested_settings.h`, `settings/parameters.h`, `settings/settings_manager.h` | KiCad's JSON settings | `ROUTING_SETTINGS` registers its persistent parameters; here they live in memory and are set by path (`Set("shove_time_limit", …)`), the way KiCad loads them from its settings file |
| `advanced_config.h` | `ADVANCED_CFG` | only the members the core reads, at KiCad's defaults, **except** the two wall-clock timeouts (walkaround cluster 100 ms, topology walk 500 ms), which are off: the service needs the same answer on a loaded machine and bounds a request with its own time limit |
| `thread_pool.h` | KiCad's thread pool | runs a submitted loop inline, in index order (`NODE::NearestObstacle` is its one user and reduces in index order) |
| `wx/*.h`, `kiid.h`, `units_provider.h`, `config.h` | wxWidgets, KIID, units, the generated `config.h` | a `std::string`-based `wxString` with `Format`; logging compiled out; the `wxCHECK` family keeps its early returns |
| `gal/*.h`, `pcb_painter.h`, `router_preview_item.h` | the GAL view | the colour type and the display flags the core names |
| `length_delay_calculation/*.h` | length/delay tuning | the pad/via clipping helpers tuning calls; the service does not tune, so they leave a line unchanged |
| `shim/time_limit.cpp` | `pcbnew/router/time_limit.cpp` | `std::chrono::steady_clock` instead of wxWidgets' clock; `INT_MAX` ms means "no limit"; the test hook `TYPMAX_ROUTER_TEST_CLOCK=expired` |

### Patches (`patches/`, 4 files, 321 lines; 108 lines added and 12 removed in upstream code, comments and the notices included)

| Patch | Upstream file | Why | Upstream? |
|---|---|---|---|
| `0001-shove-search-arc-obstacles` | `pns_shove.cpp` | shove's obstacle search never included `ARC_T`: a shove route went straight through an arc of another net | yes (same code on master) |
| `0002-archull-margin-covers-polyline-error` | `pns_utils.cpp` | `ArcHull`'s margin (5 µm) did not cover its polyline's error (half of 20 µm on either side of the arc): a walkaround past an arc never settled; the margin is now 10 µm + 1 µm | yes |
| `0003-obstacle-order-by-item-serial` | `pns_item.h`, `pns_node.h` | `NODE::OBSTACLES` was ordered by address, so a route could differ by 1 nm between processes (ASLR); items now carry a creation serial (a copy gets a new one, an assignment keeps the target's) and obstacles are ordered by it | yes |
| `0004-virtual-vias-in-a-stable-order` | `pns_joint.h`, `pns_node.cpp` | `FixupVirtualVias` created its virtual vias in the order of a hash table keyed on a net POINTER (and laid out differently by each standard library), and 0003 makes creation order the obstacle order; they are now sorted by position, layer and diameter, and the hash no longer reads the pointer | yes |

Each patch file's header says the same at more length. Each patch also adds a notice
to the top of every file it modifies ("Modified by the typmax-router authors, <date>:
…"), so the patched copies the build compiles carry the notice GPL-3.0 §5(a) asks for,
while `vendor/kicad/` itself stays byte for byte upstream.

**Known, dormant:** `TOPOLOGY::NearestUnconnectedItem` (`pns_topology.cpp`) breaks a
tie between equally near items in the order of a `std::set<ITEM*>`, an address order.
Its callers are `ROUTER::GetNearestRatnestAnchor` and the GUI's finish-route helper;
the service calls neither (no `suggest_finish`), so no answer depends on it today. A
feature that does (a "finish this route" method) needs the same treatment as 0003
first. The other pointer-keyed sets in the router are membership tests or order-free. Shove's 1000 ms wall-clock cap
needs no patch in KiCad 10: it is the persistent parameter `shove_time_limit`, which
`src/engine.cpp` sets to `INT_MAX` through the settings stand-in.

### Build

`CMakeLists.txt` compiles every `.cpp` the closure lists, plus `shim/time_limit.cpp`,
into `libpns.a`, C++20 (as KiCad 10), with `-DNDEBUG -D_USE_MATH_DEFINES -DUSINGZ -w`
(asserts off; Clipper2 with Z as KiCad builds it; vendored warnings silenced instead of
edited) and `-ffp-contract=off` on every target (no fused multiply-add, so the arm64
and x86-64 builds round the same expressions). The engine identity every answer
carries (`engine.pns`) is read from `vendor/kicad.manifest` at configure time.

## Licences

The vendored files keep their own headers; `vendor/kicad.manifest` lists each one's
licence as its header states it. Of the 155 source files: 89 GPL-3.0-or-later, 53
GPL-2.0-or-later (most of kimath), 10 BSL-1.0 (Clipper2, `thirdparty/clipper2/LICENSE`),
`libs/kimath/include/math/util.h` GPL-2.0-or-later with a portion under Apache-2.0
(Bruce Dawson's float comparison), and `polygon_triangulation.h` / `vertex_set.h`
GPL-3.0-or-later with portions under ISC (Mapbox earcut), as KiCad's own
`LICENSE.README` (vendored) lists them. The R-tree is KiCad's GPL-3.0 relicensing
(`thirdparty/rtree/README.txt`). The texts are in `LICENSES/`. The combined work, the
`typmax-router` binary, is GPL-3.0-or-later (`LICENSE`).

This repository's own files: `src/`, `shim/`, `patches/` and `CMakeLists.txt` are
GPL-3.0-or-later (they link, build or modify PNS; `shim/time_limit.cpp` keeps its
upstream header); `PROTOCOL.md` is CC0-1.0; `tools/`, `tests/`, `docker/` and `.github/` are
MIT. Every other file of ours carries an `SPDX-License-Identifier` line, or is
named in `REUSE.toml` (files that cannot carry one); `tests/run_tests.py` `t_spdx_headers` checks
every tracked file. README.md, "Licences", is the summary.
