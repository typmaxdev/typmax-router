<!-- SPDX-License-Identifier: CC0-1.0 -->
<!-- SPDX-FileCopyrightText: 2026 The typmax-router authors -->
# typmax-router protocol, version 2

This is the contract a client codes against. It is a neutral format of
our own: nothing in it is a PNS or KiCad internal structure, and a client never
needs to know what engine answers it.

## Transport

- The service is one process: `typmax-router [--time-limit-ms N] [--max-line-bytes N]`.
- **JSON Lines over stdio.** Each request is one line of UTF-8 JSON on stdin;
  each response is one line on stdout. Requests are served strictly in order,
  one at a time, and every request line gets exactly one response line.
- Strings must be valid UTF-8 (no overlong forms, no surrogates, nothing past
  U+10FFFF; a `\u` escape may not be a lone surrogate): anything else is
  `parse_error`. Every response line is valid UTF-8.
- stdout carries protocol lines only. Diagnostics go to stderr.
- Blank lines are ignored. A line longer than `--max-line-bytes` (default
  64 MiB) is answered with `request_too_large`; the service never holds more
  than that much of a line, so the cap bounds memory as the line streams in.
- `--time-limit-ms` (1..600000) and `--max-line-bytes` (≥ 1) take decimal
  integers; anything else is a usage error (exit 2) before any line is read.
- EOF on stdin ends the process with exit code 0.
- **One process = one board = one session.** The envelope carries no session
  or board id; a client keeps one process per board it works on, and restarts
  and reloads it after a fatal error.

## Envelope

Request:

```json
{"id": 7, "protocol_version": 2, "method": "route", "params": {...}}
```

| Field | Type | Meaning |
|---|---|---|
| `id` | string, integer or null | echoed in the response; the client's correlation key |
| `protocol_version` | integer | must be `2`; anything else is `unsupported_protocol_version` |
| `method` | string | `ping`, `stats`, `load_board`, `route`, `drag`, `apply`, `route_pair`, `tune` (`ping.methods` lists them) |
| `params` | object (optional) | per method; two keys are common to all methods, below |

Common params: `time_limit_ms` (integer 1..600000, default the
`--time-limit-ms` flag, itself defaulting to 10000) and `timing` (boolean,
default false: adds `elapsed_ms`, and for `route`/`drag`/`route_pair`/`tune` `world_ms`, to the
result; timings are the only nondeterministic bytes a response can carry, so
they are opt-in).

Success:

```json
{"id": 7, "protocol_version": 2, "ok": true, "result": {...}}
```

Failure (never a crash, never a missing line):

```json
{"id": 7, "protocol_version": 2, "ok": false,
 "error": {"code": "start_not_routable", "message": "PNS refused to start routing here",
           "detail": "The routing start point violates DRC."}}
```

`code` is the stable, machine-readable part; `message` is for people and may
change; `detail` (optional) is the engine's own text, which may change with
the engine. A client branches on `code` only. An error with `"fatal": true` means the process exits right after the
line (see "Time limits and crashes").

## Units and conventions

- **All coordinates and sizes are integer nanometres**, written as JSON
  integers. A number with a fraction or an exponent (`5000000.0`, `1e7`) is
  not an integer, whatever its value: `invalid_board` / `invalid_params`. |coordinate| ≤ 1e9
  (1 m); sizes (widths, diameters, clearances, pad sizes) are 0 < s ≤ 1e8.
- Axes are KiCad's: x to the right, **y down**.
- `rotation` is degrees, KiCad's convention: positive turns counter-clockwise
  *as seen on screen* (y down). A point (x, y) rotated by θ is
  `(x cos θ + y sin θ, −x sin θ + y cos θ)`. Pad rotation is absolute (board
  frame), not relative to its footprint.
- Layers are named; `layers` on the board lists the copper layers **top to
  bottom** (1..32 names, unique). The names are free-form (`"F.Cu"`, `"In1.Cu"`,
  `"B.Cu"` in KiCad terms), but the order is the physical stack.
- Item ids are strings of 1..200 characters, **unique across all pads, tracks,
  vias, keepouts and copper** of a board.
- A net is a string; `null` (or absent) means no net.

## The board (`load_board`)

```json
{
  "layers": ["F.Cu", "B.Cu"],
  "outline": [[[0, 0], [30000000, 0], [30000000, 20000000], [0, 20000000]]],
  "rules": {
    "default_class": "Default",
    "classes": {
      "Default": {"clearance": 200000, "track_width": 250000, "via_diameter": 600000, "via_drill": 300000},
      "Power":   {"clearance": 300000, "track_width": 500000, "via_diameter": 800000, "via_drill": 400000}
    },
    "net_classes": {"VCC": "Power"},
    "edge_clearance": 300000,
    "hole_clearance": 250000,
    "hole_to_hole": 250000
  },
  "pads": [
    {"id": "U1.1", "footprint": "U1", "pad": "1", "net": "A", "layers": ["F.Cu"],
     "shape": "rect", "position": [5000000, 10000000], "rotation": 0,
     "size": [1000000, 1000000], "drill": null}
  ],
  "tracks": [
    {"id": "trk-1", "net": "C", "layer": "F.Cu", "start": [0, 0], "end": [1000000, 0], "width": 250000}
  ],
  "vias": [
    {"id": "via-1", "net": "C", "position": [1000000, 0], "diameter": 600000, "drill": 300000,
     "layers": ["F.Cu", "B.Cu"]}
  ],
  "keepouts": [
    {"id": "ko-1", "layers": ["F.Cu"], "polygon": [[0, 0], [1000000, 0], [1000000, 1000000]]}
  ],
  "copper": [
    {"id": "logo-1", "net": null, "layer": "F.Cu", "polygon": [[...], [...], [...]]}
  ]
}
```

| Key | Required | Meaning |
|---|---|---|
| `layers` | yes | copper layers, top to bottom |
| `rules` | yes | see below |
| `outline` | no | board edge: a list of polygons (each ≥ 3 points, implicitly closed). Copper keeps `edge_clearance` from every edge |
| `pads` | no | `id`, `layers` (non-empty), `shape`, `position`, `size` `[w, h]` required; `footprint`, `pad`, `net`, `rotation` (default 0), `drill` (default none = SMD) optional. A pad with a `drill` blocks every copper layer (its hole), whatever its `layers` |
| `tracks` | no | `id`, `layer`, `start`, `end` (≠ `start`: a zero-length track is `invalid_board`; give a dot of copper as `copper`), `width` required; `net`, `mid` (an arc through `mid`), `locked` (never moved by shove) optional |
| `vias` | no | `id`, `position`, `diameter`, `drill` (< diameter), `layers` (the two end layers) required; `net`, `locked` optional |
| `keepouts` | no | `id`, `layers`, `polygon`: no new track or via inside the polygon on those layers (a polygon that cannot be triangulated, e.g. self-intersecting, blocks only crossing its boundary) |
| `copper` | no | fixed copper that is none of the above (copper text, graphics; later zone fills): `id`, `layer`, `polygon` required, `net` optional. Never moved or removed |

Pad shapes: `circle` (diameter = max(w, h)), `rect`, `roundrect` (treated as
its bounding `rect`: conservative), `oval` (a stadium along the longer side),
`polygon` (needs `polygon`: vertices relative to `position`, unrotated;
`rotation` applies). Polygons — pad `polygon`, `copper` — are obstacles as
their **convex hull** (the engine's own model; conservative for concave shapes).

Rules:

| Key | Required | Meaning |
|---|---|---|
| `classes` | yes | name → `clearance`, `track_width`, `via_diameter`, `via_drill` (drill < diameter) |
| `default_class` | yes | the class of every net not in `net_classes` (and of no-net copper) |
| `net_classes` | no | net → class name. Naming a net here does not make it a net of the board: only an item's `net` does (`counts.nets`, `unknown_net`) |
| `edge_clearance` | no | copper to board edge; default: the default class's clearance |
| `hole_clearance` | no | copper to a drilled hole; default: the default class's clearance |
| `hole_to_hole` | no | drilled hole to drilled hole; default: the default class's clearance |

Clearance between two items of different nets is the larger of their two
classes' clearances; items of the same net never collide.

What the router guarantees, and what it does not — a client's own checker
must enforce the second list:

| Rule | Guaranteed for new copper |
|---|---|
| copper clearance, different nets | yes, to within 0.5 µm: the engine takes KiCad's clearance epsilon off its collision checks (KiCad's DRC allows the same) |
| `edge_clearance` | yes (the edge is a zero-width line, as KiCad models Edge.Cuts) |
| `hole_clearance` (copper to a hole), different nets | yes |
| `hole_to_hole`, different nets | yes (the engine) |
| `hole_to_hole`, same net or no net | yes: the engine checks every hole pair whatever the nets, and the service checks every hole a proposal **adds** behind it (`hole_too_close`) |
| `hole_clearance`, same net (copper to its own net's hole) | **no** — the engine skips same-net pairs; whether KiCad's DRC requires it is unverified |
| keepout interior | yes, for new tracks and vias (since service 0.3.0); a pad inside a keepout is the board's own |
| no-net copper against no-net copper | **no** — the engine never lets two no-net items collide; `drag` of a no-net item is refused for this reason |
| violations already on the loaded board | not reported |

Result: `{"revision": 0, "counts": {"layers", "pads", "tracks", "vias", "keepouts", "copper", "nets"}}`.
Loading a board replaces the previous one and resets `revision` to 0.

## `route`

Runs the push-and-shove line placer from `start` to `end` and returns a
**proposal**. It never changes the loaded board; `apply` does.

| Param | Required | Meaning |
|---|---|---|
| `net` | yes | the net being routed; some item of the board must carry it (else `unknown_net`) |
| `layer` | yes | the layer the route starts on |
| `start` | yes | `{"pad": id}` (must be on `net` and on `layer`) or `{"point": [x, y]}` (a free point; if it lands on copper of `net` on `layer`, the route starts from that item, else the route starts there, on `net`, with `net`'s class rules. A point inside another net's copper is `start_not_routable`) |
| `end` | no | same shape as `start`; omitted = the route ends at the last waypoint (a dangling route) |
| `waypoints` | no | up to 1000 `{"point": [x, y], "via": bool, "layer": name}` in order. Each waypoint is a click: the route so far is fixed there. Fixing costs more the more is fixed: the time grows faster than the count (on a two-pad test board, measured: 100 waypoints 0.07 s, 200 0.5 s, 400 5 s, 800 64 s), so the default 10 s limit sustains a few hundred, not 1000; a client sending more raises `time_limit_ms`, or the request ends in the fatal `timeout`. `via: true` places a through via at the point and continues on `layer` (required with `via`); a board with one copper layer takes no via (`invalid_params`) |
| `mode` | no | `walkaround` (default): route around obstacles; `shove`: push other nets' tracks and vias aside (they appear in the proposal's `remove`/`add`); `mark_obstacles`: route straight, refuse with `collision` if that violates clearance |
| `via_allowed` | no | default false; a `via` waypoint with `via_allowed: false` is `via_not_allowed` |
| `width` | no | track width; default the net class's `track_width` |
| `via_diameter`, `via_drill` | no | default the net class's; after the defaults, `via_drill` must be smaller than `via_diameter` (else `invalid_params`, since service 0.3.1) |
| `remove_loops` | no | default true: as in KiCad, a new route that closes a loop with existing copper of the same net removes the redundant old part (it appears in `remove`) |
| `step` | no | default 0. > 0 feeds the cursor toward each target in steps of at most `step` nm, the way a mouse drag feeds an interactive router. Slower; on the video demo board a 1 mm step recovered 3 of 24 failed routes at ~60x the cost (measured on service 0.2.0) |

Either `end` or at least one waypoint is required.

Result:

```json
{"revision": 0,
 "engine": {"name": "typmax-router", "version": "0.4.0",
            "pns": "KiCad 10.0.7 pcbnew/router @ 69cafb907ebc", "protocol": 2},
 "proposal": {
   "remove": ["trk-c1", "trk-c2"],
   "add": [
     {"kind": "track", "id": "t20bd8c909d096d9a", "net": "A", "layer": "F.Cu",
      "start": [5000000, 10000000], "end": [6825011, 11825011], "width": 250000},
     {"kind": "via", "id": "v8b0f3c...", "net": "A", "position": [15000000, 10000000],
      "diameter": 600000, "drill": 300000, "layers": ["F.Cu", "B.Cu"]}
   ]}}
```

- `revision` is the board revision the proposal was computed against.
- `engine` names what computed it; `version` moves whenever the same request
  could get a different answer. Store it with a proposal: a replay against
  another `version` may differ.
- A proposal may be **empty** (`{"remove": [], "add": []}` with `ok: true`):
  the route already exists.
- `remove`: ids of existing tracks and vias the proposal deletes (shoved,
  merged or loop-removed copper), sorted. Pads, keepouts and copper are never
  removed.
- `add`: new tracks (a `mid` key means an arc) then new vias, each sorted by
  (net, layer, coordinates). A track that shove moved comes back as a remove
  plus an add.
- New ids are `t`/`v` + 16 hex digits of a hash of the item's geometry and net,
  so the same proposal always carries the same ids. An id that collides with
  an existing item gets a `-1`, `-2`… suffix.

**Determinism.** The same board and the same request give byte-identical
response lines (without `timing`), within one process, across processes, and
across the macOS arm64 and Linux x86_64 builds — a gate, not a measurement:
the test suite's golden lines are compared in both builds, which compile with
floating-point contraction off. The engine hands its commit over in pointer
order; the service sorts it. Nothing in a route depends on the clock: shove's
wall-clock cap is off (it stops on its iteration limit), and the request's
time limit ends the process rather than changing an answer.

## `drag`

Drags one existing track segment or via with the push-and-shove dragger and
returns a proposal of the same shape as `route`.

| Param | Required | Meaning |
|---|---|---|
| `item` | yes | id of a track or via (not a pad); a `locked` item is `locked`; an item with no net is `drag_refused` |
| `to` | yes | the point the grab point moves to |
| `from` | no | the grab point; default the segment's midpoint or the via's centre. Grabbing near a segment's end drags the corner |
| `mode` | no | `shove` (default), `walkaround`, `mark_obstacles` |

## `apply`

Commits a proposal into the loaded board so later requests see it.

| Param | Required | Meaning |
|---|---|---|
| `proposal` | yes | `{"remove": [...], "add": [...]}`, as returned (a client may filter or edit it; every added item is validated like a board item) |
| `base_revision` | yes | the revision the proposal was computed against; not the current revision: `stale_proposal`, nothing applied |

Atomic: every `remove` id must exist and every added id must be new after the
removals, else `stale_proposal` and the board is unchanged. Result:
`{"revision": n+1, "removed": k, "added": m, "counts": {...}}`.

## `route_pair`

Runs the differential-pair placer (KiCad's `DIFF_PAIR_PLACER`) from a pair of
start ends to a pair of end ends and returns a **proposal** holding both
members' copper. It never changes the loaded board; `apply` does.

| Param | Required | Meaning |
|---|---|---|
| `p`, `n` | yes | the pair's two nets, different (else `invalid_params`); each carried by some item of the board (else `unknown_net`). The service couples exactly these two: no net-name suffix is read |
| `layer` | yes | the layer the pair starts on |
| `start` | yes | `{"p": end, "n": end}`, each end of `route`'s shape: `{"pad": id}` (on that member's net, else `pair_mismatch`; on `layer`, else `layer_mismatch`) or `{"point": [x, y]}` (the member's copper there, on `layer`, which the pair starts from as the engine does: from its free end; else a point of the member's net). The engine couples `start.p` with the **nearest item of net `n` of the same kind** (pad with pad, via with via, track with track; a free point counts as a pad): `start.n` must be that item, else `pair_not_coupled` |
| `end` | no | the same shape; the pair lands on both ends, on the layer the last via waypoint names (else `layer`), and the same coupling rule holds. **With an `end`, both members land on their end items, each joined to its start, or the answer is `unroutable`**: never a proposal without the end leg. Omitted = the pair ends at its last waypoint (dangling) |
| `waypoints` | no | as `route`'s: up to 1000 `{"point": [x, y], "via": bool, "layer": name}`, each a click that fixes the pair so far; `point` is where the pair's **centreline** goes. The pair must reach it: a centreline that ends farther than half the pair's pitch ((`gap` + width) / 2) from the point is `unroutable`, as `route` refuses a waypoint its trace does not end on. `via: true` places twin through vias, one per member, `via_gap` apart, and continues on `layer`. A waypoint equal to the one before it is `invalid_params` |
| `gap` | yes | the members' gap, edge to edge, nm. Under the larger of the two classes' `clearance` is `gap_too_small` (equal to it is admitted) |
| `via_gap` | no | the twin vias' copper, edge to edge, nm, 1..1e8. Default, and least: what the rules admit, `max(clearance, hole_to_hole − 2a, hole_clearance − a)` with `a = (via_diameter − via_drill) / 2` (integer nm) and `clearance` the larger class's; under it is `gap_too_small` |
| `mode` | no | `walkaround` (default) or `shove` (other nets' tracks and vias pushed aside, in the proposal's `remove`/`add`); `mark_obstacles` is `invalid_params` |
| `via_allowed` | no | default false; a `via` waypoint without it is `via_not_allowed` |

Either `end` or at least one waypoint is required.

Sizes come from the members' net classes, as `route`'s defaults do: `track_width`,
`via_diameter` and `via_drill`. The engine draws both members at one size, so two
classes that differ in any of the three are `pair_class_mismatch`; there is no
`width` override.

Result: `route`'s shape (`revision`, `engine`, `proposal`), both members' new
copper in one `add`, sorted and named as `route`'s. The guarantees table holds for
both members (P against N is two nets: the larger clearance), and the service's
`hole_too_close` check covers the twin vias.

- Where the pair is coupled its members run at `gap`. Around an obstacle the
  engine walks each member on its own: a pair may split, one member each side
  of an obstacle, rather than go round it together (`tests/boards/pair.json`).
- **One bend per leg after a waypoint.** Each leg the pair placer lays after a fixed waypoint is an entry at the
  fixed point, at most one 45° bend, and the entry onto the next waypoint or the end. A route that needs a further
  bend in one leg needs a waypoint at that bend, as in KiCad's own router; without one the answer is `unroutable`
  once an `end` (or a later waypoint) follows. For example, a single waypoint off the pads' line followed by an end
  back on it needs two bends in its last leg, so it is `unroutable`. With a second waypoint where the pair turns back
  (off the line, along it, back, then the end), it routes. A pair with no `end` is routed to its last waypoint
  (dangling). The pair can bend at a fixed point on either diagonal because of patches/0005 (SOURCE.md).
- **The proposal holds the final copper only.** The pair placer commits each fixed leg into the engine's world at
  once, and a later leg may merge, re-optimise or shove it again; the service drops such a leg from the proposal, so
  each member's copper is laid once.
- Determinism and the golden-line gate are `route`'s.

## `tune`

Meanders a span of one track toward a target length with KiCad's meander
placers and returns a proposal plus the length the engine counts.

| Param | Required | Meaning |
|---|---|---|
| `aim` | yes | `length`: one net (the single-track tuner, `MEANDER_PLACER`); `pair_length`: a pair, both members meandered together (`DP_MEANDER_PLACER`); `skew`: one member of a pair against its partner (`MEANDER_SKEW_PLACER`) |
| `net` | for `length` | the net tuned; with `p`/`n` it is `invalid_params` |
| `p`, `n` | for `pair_length`, `skew` | the pair's nets, different; with `net` it is `invalid_params`. A net no item carries is `unknown_net` |
| `target` | yes | integer nm. `length`, `pair_length`: the length wanted, 1..1e10. `skew`: the tuned member's length less its partner's, −1e9..1e9 |
| `tolerance` | yes | integer nm, 0..1e9: the window is `target ± tolerance` (for `skew`, `abs(target) + tolerance` ≤ 1e9) |
| `track` | yes | the id of the track the span is on (else `unknown_item`): on `net` (else `net_mismatch`), or on `p` or `n` (else `pair_mismatch`); a `locked` track is `locked`. For `skew` the member tuned is this track's net. The engine never changes locked copper: a tune whose answer would remove a locked track or via (a `pair_length` tune's partner line, or another track of the tuned line) is `locked`, and nothing is proposed |
| `from`, `to` | yes | two different points on this track's copper (within its width, else `invalid_params`). The engine snaps `from` onto the track's centreline (an arc: to its nearer end) and meanders the span from there to `to`'s place along the track. It rewrites the track's whole line (the run of tracks between pads, vias and branches; see `proposal` below) |
| `spacing` | no | nm, 1..1e8: the meanders' pitch; default 600000 (the engine's). The engine never goes under width + clearance |
| `amplitude` | no | `[min, max]`, nm, 0 < min ≤ max ≤ 1e8: the meanders' height; default `[200000, 1000000]` (the engine's) |
| `corners` | no | `chamfer` (default): 45° corners of KiCad's default size (80 % of half the spacing); `square`: the sharpest corner the engine draws, a 45° chamfer of half the track width (it refuses any sharper, KiCad issue 8629). Arcs are not offered yet |
| `single_sided` | no | default false: every meander on one side of the line |
| `mode` | no | `walkaround` only (the default): the meander placers fit a meander only where it collides with nothing and never shove; anything else is `invalid_params` |

`pair_length` and `skew` need a pair the engine can assemble from the track: a
partner track of the other member, parallel, at the same width, on the same layer
(else `pair_not_coupled`), and a coupled section inside the span (else
`pair_not_coupled`).

Result:

```json
{"revision": 0, "engine": {...},
 "proposal": {"remove": ["trk-a1"], "add": [...]},
 "length": 25000014, "status": "tuned"}
```

- `length` (integer nm): the tuned member's length **as the engine counts it,
  track length only**: every segment and arc of the engine's tuning path (one path
  from pad to pad, through vias) at its drawn length; vias add 0, pads add no
  pad-to-die length, and no part inside a pad is clipped off. A branch or stub off
  that path is not counted, so on a branched net it is less than the sum over every
  track of the net. With a proposal, it is the length the proposal gives. For
  `pair_length` it is the **longer** member's (the engine tunes a pair by it); for
  `skew`, the tuned member's.
- `skew` (for `aim: skew` only, integer nm): the tuned member's length less its
  partner's, counted the same way.
- `status`: `tuned` (inside the window), `too_short` (the span had no room for
  enough meanders: the proposal adds what fits, and `length` is short of the
  window), `too_long` (the line is already longer than the window allows; meanders
  only add length, so the proposal is empty and `length` is the length now).
- `proposal`: the engine rewrites the whole line it tunes; tracks it gives back
  unchanged are left out (neither removed nor added). An empty proposal changes
  nothing.
- Determinism and the golden-line gate are `route`'s.

## `ping`, `stats`

`ping` → `{"pong": true, "service": "typmax-router", "version": "...", "engine": {...}, "methods": [...]}` (the same `engine` object as a proposal's;
`methods`, since service 0.4.0: every method this binary serves, as strings, in this document's order —
`["ping", "stats", "load_board", "route", "drag", "apply", "route_pair", "tune"]` — so a client learns what a
binary offers without probing for `unknown_method`; a binary older than 0.4.0 has no `methods` key).
`stats` → `{"loaded", "revision", "counts", "requests", "last_elapsed_ms", "default_time_limit_ms"}`
(`stats` carries timings and is not deterministic).

## Error codes

| Code | When |
|---|---|
| `parse_error` | the line is not JSON, or not valid UTF-8 (`id` is null) |
| `invalid_request` | not an object; bad `id`, `method` or `params` type |
| `unsupported_protocol_version` | `protocol_version` missing or not 2 |
| `unknown_method` | |
| `request_too_large` | line over `--max-line-bytes` |
| `invalid_params` | a param missing, of the wrong type or out of range |
| `invalid_board` | the board fails validation; the message names the JSON path |
| `no_board` | `route`/`drag`/`apply`/`route_pair`/`tune` before `load_board` |
| `unknown_net` | `route`, `route_pair` or `tune` on a net no board item carries (a net named only in `net_classes` included) |
| `unknown_item` | a pad/track/via id that does not exist (a `tune` `track` that is no track) |
| `net_mismatch` | a start/end pad on another net; a `tune` `track` (aim `length`) on another net |
| `layer_mismatch` | a start/end pad not on the route's (or the pair's) layer |
| `start_not_routable` | the engine refuses to start there (e.g. inside other copper), the pair's start included |
| `unroutable` | the engine could not reach the end (or a waypoint) in this mode, land a pair on its end, or bring a pair's centreline to a waypoint |
| `collision` | `mark_obstacles` and the straight route violates clearance |
| `via_not_allowed` | a via waypoint without `via_allowed` (`route`, `route_pair`) |
| `drag_refused` | the dragger refuses the item/grab point, or the item has no net |
| `hole_too_close` | the proposal would add a hole closer than `hole_to_hole` to a hole of its own net (or of no net) |
| `locked` | dragging a locked item; tuning a locked track, or a tune that would change locked copper (a `pair_length` partner's line) |
| `pair_mismatch` | a `route_pair` end pad not on its member's net; a `tune` `track` on neither member of the pair |
| `pair_not_coupled` | a `route_pair` `start`/`end` whose `n` end is not the item the engine couples with its `p` end; a `tune` pair aim with no partner track the engine assembles, or no coupled section in the span |
| `pair_class_mismatch` | the two members' classes differ in `track_width`, `via_diameter` or `via_drill` |
| `gap_too_small` | a `route_pair` `gap` under the pair's clearance, or a `via_gap` under the least the rules admit |
| `tune_refused` | the meander placer refuses the span (e.g. `from` snaps onto the centreline where `to` is) |
| `stale_proposal` | `apply` with a wrong `base_revision`, a missing remove id, or a duplicate id |
| `timeout` | **fatal**: the request exceeded its time limit (or its line did not parse within the default limit: `id` null) |
| `internal_error` | an engine exception; the service continues |
| `internal_crash` | **fatal**: a signal (segfault, abort) in the engine |

## Time limits and crashes

Each request runs on one worker thread (64 MiB stack: the engine recurses
deeply). Its line is parsed there first, under the default time limit (the
request's own limit is not known until it parses); then the method runs under
the request's limit. The engine cannot be interrupted
safely and keeps a process-wide router singleton, so a request over its limit
is answered with a fatal `timeout` error and the process **exits with code 3**.
A fatal signal is answered with `internal_crash` (the id of the request in
flight, or null while no request's id is known) and exit code 70. In both cases the loaded board is lost: the client
restarts the service and sends `load_board` again (and replays its applies).

## Versioning

`protocol_version` changes on any incompatible change to this document.
Additive changes (a new method, a new optional param, a new result key, a new
error code) keep the version; clients ignore keys they do not know, and learn a
binary's methods from `ping.methods` (a binary without the key predates 0.4.0 and
serves the six methods of service 0.3.1).

## Changelog

### service 0.4.0 (protocol 2, additive)

- Methods `route_pair` (KiCad's differential-pair placer) and `tune` (its
  single-track, pair-length and pair-skew meander placers); error codes
  `pair_mismatch`, `pair_not_coupled`, `pair_class_mismatch`, `gap_too_small` and
  `tune_refused`.
- `ping` carries `methods`.
- Versioning names a new method as an additive change.
- With an `end`, `route_pair` checks that both members reached their end items and that no member's copper is laid
  twice; a waypoint off the pads' line followed by an `end` is `unroutable` (PR #11 review round 2).
- No answer to an existing request changed beyond `engine.version` (and `ping`'s
  new key): the golden lines of the earlier requests differ from 0.3.1's only in
  the version string.

### service 0.3.1 (protocol 2, unchanged on the wire)

- `route` refuses a via whose `via_drill` is not smaller than its `via_diameter`
  (after the net class's defaults) with `invalid_params`; it used to propose one
  that `apply` then refused.
- A route around an arc keeps about 9 µm less extra distance from it (the arc
  hull's margin is half the polyline's error, no longer all of it; patches/0002).
- Virtual vias (KiCad's width-change markers) are created in a fixed order on
  every platform (patches/0004); no golden answer moved.
- The `waypoints` row states what a long waypoint list costs.

### service 0.3.0 (protocol 2, unchanged on the wire)

The engine is KiCad 10.0.7's own router (it was an older copy of it), so the
same request may get a different answer; `engine.version` and `engine.pns` say
which. No message, key or error code changed. Behaviour:
- A keepout blocks its whole area for new tracks and vias, not only crossing
  its boundary.
- The board edge is a zero-width line (it was a 10 nm wall).
- Same-net `hole_to_hole` is also checked by the engine (a too-close via now
  usually fails as `unroutable`; the service's `hole_too_close` stays behind it).
- Copper clearance holds to within KiCad's 0.5 µm epsilon.
- Shove moves arcs and plain dangling tracks it used to refuse; more routes
  succeed on dense boards (the video demo: 25 of 45 where 21 did).

### 2 (service 0.2.0)

Incompatible:
- `apply` requires `base_revision`.
- A number with a fraction or an exponent is never an integer (version 1's
  parser took `5000000.0` and `1e7` as coordinates against its own text).
- A net named only in `rules.net_classes` is not a net of the board:
  `unknown_net`, and `counts.nets` no longer counts it.
- A zero-length track is `invalid_board` (version 1 loaded it and the engine
  silently dropped it as an obstacle).
- Strings must be valid UTF-8 (`parse_error`).
- A free-point `start` on no copper routes on the requested `net` with its
  class rules (version 1 routed a net-less line: `"net": null`, the Default
  class's clearance, and every pad of the net an obstacle).
- Shove's 1000 ms wall-clock cap is off; a shove that hit it may now answer
  differently (it is now the same on every machine).
- `--max-line-bytes` defaults to 64 MiB (was 512 MiB); a malformed flag value
  exits 2.

Additive:
- `route`/`drag` results and `ping` carry an `engine` object (`ping`'s
  `engine` was a string).
- `error.detail`: the engine's own text, no longer appended to `message`.
- Error code `hole_too_close`.
- `drag` of a no-net item is `drag_refused`; a via waypoint on a one-layer
  board is `invalid_params`.
- Parsing runs under the default time limit (a fatal `timeout` with a null id).
- The guarantees table under "The board", and "one process = one board".

### 1 (service 0.1.0)

The first version.
