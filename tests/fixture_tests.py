# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
"""Fixture tests against real KiCad boards (never committed: CC BY-SA 4.0).

TYPMAX_FIXTURES_DIR names a directory holding pic_programmer.kicad_pcb and
video.kicad_pcb (KiCad's demos; tools/fetch_fixtures.sh downloads them). Unset
or missing -> the suite prints why and exits 77 (skipped).

For each board: strip every track and via of a few nets (and all zones, which
the protocol does not model), DRC the stripped board with kicad-cli (baseline),
re-route each stripped net pad to pad through the service (a minimum spanning
tree over its pads, each edge one `route` + `apply`), write the routed board
and DRC it again. The assertion: no NEW clearance/short-class violation.
Latency per route is recorded. Results land in $TYPMAX_FIXTURES_OUT (default
build/fixtures-out/) as <board>-result.json."""
import collections
import json
import math
import os
import pathlib
import shutil
import statistics
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))
import apply_to_kicad  # noqa: E402
import kicad_to_board  # noqa: E402
from router_client import RouterClient  # noqa: E402

SKIP = 77
# violation classes a router can cause and must not
COPPER_CLASSES = {"clearance", "shorting_items", "tracks_crossing", "hole_clearance", "copper_edge_clearance",
                  "hole_to_hole", "drill_out_of_range", "via_diameter", "track_width", "annular_width"}
KICAD_CLI_DEFAULT = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"

BOARDS = {
    # board -> how many nets to strip and re-route
    "pic_programmer": 12,
    "video": 25,
}


def kicad_cli():
    c = os.environ.get("KICAD_CLI") or (KICAD_CLI_DEFAULT if os.path.exists(KICAD_CLI_DEFAULT) else shutil.which("kicad-cli"))
    return c


def drc(cli, pcb, out_json):
    subprocess.run([cli, "pcb", "drc", "--format", "json", "--severity-all", "-o", str(out_json), str(pcb)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return json.loads(pathlib.Path(out_json).read_text())


def vkey(v):
    return (v["type"], tuple(sorted(i.get("uuid", i.get("description", "")) for i in v.get("items", []))))


def pick_nets(board, n):
    pads_by_net = collections.defaultdict(list)
    for p in board["pads"]:
        if p["net"]:
            pads_by_net[p["net"]].append(p)
    tracks_by_net = collections.Counter(t["net"] for t in board["tracks"] if t["net"])
    cands = sorted(net for net, ps in pads_by_net.items() if 2 <= len(ps) <= 6 and tracks_by_net[net] > 0)
    if len(cands) <= n:
        return cands
    step = len(cands) / n  # spread the picks across the name order
    return [cands[int(i * step)] for i in range(n)]


def mst_edges(pads):
    """Prim's MST over pad centres; ties broken by pad id (deterministic)."""
    pads = sorted(pads, key=lambda p: p["id"])
    inside = {pads[0]["id"]}
    edges = []
    while len(inside) < len(pads):
        best = None
        for a in pads:
            if a["id"] not in inside:
                continue
            for b in pads:
                if b["id"] in inside:
                    continue
                d = math.dist(a["position"], b["position"])
                k = (d, a["id"], b["id"])
                if best is None or k < best[0]:
                    best = (k, a, b)
        _, a, b = best
        edges.append((a, b))
        inside.add(b["id"])
    return edges


def lerp(a, b, t):
    return [round(a[0] + (b[0] - a[0]) * t), round(a[1] + (b[1] - a[1]) * t)]


def attempts(a, b, layers):
    """Route parameter sets to try for one pad pair, cheapest first: single-layer
    walkaround then shove (bottom layer first), then one via on the a-b line to a
    layer of b, then a detour through another layer with two vias."""
    common = [l for l in reversed(layers) if l in a["layers"] and l in b["layers"]]
    for mode in ("walkaround", "shove"):
        for layer in common:
            yield {"layer": layer, "mode": mode}
    pa, pb = a["position"], b["position"]
    for la in reversed(a["layers"]):
        for lb in reversed(b["layers"]):
            if la == lb:
                continue
            for t in (0.5, 0.3, 0.7):
                yield {"layer": la, "mode": "walkaround", "via_allowed": True,
                       "waypoints": [{"point": lerp(pa, pb, t), "via": True, "layer": lb}]}
            break
        break
    for la in common[:1]:
        for mid in [l for l in layers if l != la][:2]:
            yield {"layer": la, "mode": "walkaround", "via_allowed": True,
                   "waypoints": [{"point": lerp(pa, pb, 0.25), "via": True, "layer": mid},
                                 {"point": lerp(pa, pb, 0.75), "via": True, "layer": la}]}


def is_copper_zone(node):
    """A filled copper zone (stripped: fills are not modelled); a keepout zone
    stays, so the converter's keepout path and the engine's keepout walls are
    judged by KiCad's DRC too (review T-7)."""
    return node[0] == "zone" and not any(isinstance(x, list) and x and x[0] == "keepout" for x in node[1:])


def run_board(binary, cli, src, n_nets, out_dir):
    name = src.stem
    text = src.read_text()
    full = kicad_to_board.convert(src)
    nets = pick_nets(full, n_nets)
    strip_ids = {t["id"] for t in full["tracks"] if t["net"] in nets} | {v["id"] for v in full["vias"] if v["net"] in nets}
    stripped_pcb = out_dir / f"{name}-stripped.kicad_pcb"
    stripped_pcb.write_text(apply_to_kicad.rewrite(text, strip_ids, [], drop=is_copper_zone))
    board = kicad_to_board.convert(stripped_pcb)
    base = drc(cli, stripped_pcb, out_dir / f"{name}-stripped.drc.json")

    routes, failures, proposals = [], [], []
    with RouterClient(binary) as c:
        t0 = time.perf_counter()
        c.call("load_board", {"board": board, "time_limit_ms": 60000})
        load_ms = (time.perf_counter() - t0) * 1000
        pads_by_net = collections.defaultdict(list)
        for p in board["pads"]:
            pads_by_net[p["net"]].append(p)
        for net in nets:
            for a, b in mst_edges(pads_by_net[net]):
                done = False
                tried = []
                for att in attempts(a, b, board["layers"]):
                    layer, mode = att["layer"], att["mode"]
                    t = time.perf_counter()
                    resp = c.request("route", {"net": net, "start": {"pad": a["id"]}, "end": {"pad": b["id"]},
                                               "timing": True, "time_limit_ms": 30000, **att})
                    wall = (time.perf_counter() - t) * 1000
                    if resp["ok"]:
                        r = resp["result"]
                        routes.append({"net": net, "from": a["id"], "to": b["id"], "layer": layer, "mode": mode,
                                       "vias": len(att.get("waypoints", [])), "params": att,
                                       "elapsed_ms": r["elapsed_ms"], "world_ms": r["world_ms"], "wall_ms": wall,
                                       "removed": len(r["proposal"]["remove"]), "added": len(r["proposal"]["add"])})
                        c.call("apply", {"proposal": r["proposal"], "base_revision": r["revision"]})
                        proposals.append(r["proposal"])
                        done = True
                        break
                    tried.append(f"{layer}/{mode}/{len(att.get('waypoints', []))}v: {resp['error']['code']}")
                if not done:
                    failures.append({"net": net, "from": a["id"], "to": b["id"], "tried": tried})
        stats = c.call("stats")

    # determinism on a real board: other processes replay every successful
    # request (without timing) and must return byte-identical proposals. More
    # than one: an answer that depends on where the allocator put an object
    # (ASLR) differs in only some processes (macOS: 2 runs in 12, before
    # patches/0003)
    replay_identical = True
    for _ in range(int(os.environ.get("TYPMAX_FIXTURES_REPLAYS", "3"))):
        with RouterClient(binary) as c2:
            c2.call("load_board", {"board": board, "time_limit_ms": 60000})
            for rt, prop in zip(routes, proposals):
                params = {"net": rt["net"], "start": {"pad": rt["from"]}, "end": {"pad": rt["to"]}, **rt["params"]}
                r2 = c2.call("route", params)
                if json.dumps(r2["proposal"], sort_keys=False) != json.dumps(prop, sort_keys=False):
                    replay_identical = False
                    break
                c2.call("apply", {"proposal": r2["proposal"], "base_revision": r2["revision"]})
        if not replay_identical:
            break

    routed_pcb = out_dir / f"{name}-routed.kicad_pcb"
    composed = apply_to_kicad.compose(proposals)
    routed_pcb.write_text(apply_to_kicad.rewrite(stripped_pcb.read_text(), composed["remove"], composed["add"]))
    after = drc(cli, routed_pcb, out_dir / f"{name}-routed.drc.json")

    before_keys = collections.Counter(vkey(v) for v in base["violations"])
    after_keys = collections.Counter(vkey(v) for v in after["violations"])
    new = after_keys - before_keys
    new_by_type = collections.Counter(k[0] for k in new.elements())
    gone_by_type = collections.Counter(k[0] for k in (before_keys - after_keys).elements())
    new_copper = {t: n for t, n in new_by_type.items() if t in COPPER_CLASSES}
    new_copper_detail = [v for v in after["violations"] if v["type"] in COPPER_CLASSES and new[vkey(v)] > 0][:20]
    lat = [r["elapsed_ms"] for r in routes]
    result = {
        "board": name, "nets": nets, "stripped_items": len(strip_ids),
        "load_board_ms": round(load_ms, 1),
        "routes_ok": len(routes), "routes_failed": len(failures), "failures": failures,
        "latency_ms": {"median": round(statistics.median(lat), 2) if lat else None,
                       "p90": round(sorted(lat)[int(0.9 * (len(lat) - 1))], 2) if lat else None,
                       "max": round(max(lat), 2) if lat else None,
                       "world_build_median": round(statistics.median(r["world_ms"] for r in routes), 2) if routes else None},
        "drc_before": dict(collections.Counter(v["type"] for v in base["violations"])),
        "drc_after": dict(collections.Counter(v["type"] for v in after["violations"])),
        "unconnected_before": len(base.get("unconnected_items", [])),
        "unconnected_after": len(after.get("unconnected_items", [])),
        "stripped_nets_unconnected_after": sorted({n for n in nets for u in after.get("unconnected_items", [])
                                                   for i in u.get("items", []) if f"[{n}]" in i.get("description", "")}),
        "new_violations_by_type": dict(new_by_type), "gone_violations_by_type": dict(gone_by_type),
        "new_copper_violations": new_copper, "new_copper_detail": new_copper_detail,
        "replay_identical": replay_identical,
        "routes": routes, "service_stats": stats,
    }
    (out_dir / f"{name}-result.json").write_text(json.dumps(result, indent=1))
    return result


def main(binary):
    d = os.environ.get("TYPMAX_FIXTURES_DIR")
    if not d:
        print("SKIP fixtures: TYPMAX_FIXTURES_DIR is not set (see tools/fetch_fixtures.sh)")
        return SKIP
    cli = kicad_cli()
    if not cli:
        print("SKIP fixtures: no kicad-cli (set KICAD_CLI)")
        return SKIP
    out_dir = pathlib.Path(os.environ.get("TYPMAX_FIXTURES_OUT", HERE.parent / "build" / "fixtures-out"))
    out_dir.mkdir(parents=True, exist_ok=True)
    failed = 0
    ran = 0
    for name, n in BOARDS.items():
        src = pathlib.Path(d) / f"{name}.kicad_pcb"
        if not src.exists():
            print(f"SKIP {name}: {src} not found")
            continue
        ran += 1
        r = run_board(binary, cli, src, n, out_dir)
        lat = r["latency_ms"]
        print(f"{name}: {r['routes_ok']} routes ok, {r['routes_failed']} failed; latency median {lat['median']} ms, "
              f"p90 {lat['p90']} ms, max {lat['max']} ms (world build {lat['world_build_median']} ms); "
              f"unconnected {r['unconnected_before']} -> {r['unconnected_after']}")
        print(f"  DRC new violations: {r['new_violations_by_type']}; gone: {r['gone_violations_by_type']}")
        print(f"  replay in a second process byte-identical: {r['replay_identical']}")
        if not r["replay_identical"]:
            failed += 1
            print(f"FAIL {name}: the replay produced different proposals (nondeterminism)")
        elif r["new_copper_violations"]:
            failed += 1
            print(f"FAIL {name}: new copper violations {r['new_copper_violations']} (see {out_dir}/{name}-result.json)")
        elif r["routes_ok"] == 0:
            failed += 1
            print(f"FAIL {name}: no route succeeded")
        else:
            print(f"PASS {name}")
    if ran == 0:
        print("SKIP fixtures: no fixture boards found")
        return SKIP
    return 1 if failed else 0
