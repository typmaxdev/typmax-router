#!/usr/bin/env python3.11
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
"""typmax-router test runner (stdlib only).

  run_tests.py --binary build/typmax-router --suite synthetic|determinism|fixtures|all

Exit 0 = pass, 1 = fail, 77 = the suite was skipped (ctest's SKIP_RETURN_CODE)."""
import argparse
import json
import math
import os
import pathlib
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))

import drc_lite  # noqa: E402
from router_client import PROTOCOL_VERSION, RouterClient, RouterError  # noqa: E402

BOARDS = HERE / "boards"
GOLDEN = HERE / "golden" / "responses.jsonl"
MM = 1_000_000
SKIP = 77


class Failure(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Failure(msg)


def board(name):
    return json.loads((BOARDS / f"{name}.json").read_text())


def route(c, **p):
    p.setdefault("net", "A")
    p.setdefault("layer", "F.Cu")
    p.setdefault("start", {"pad": "U1.1"})
    p.setdefault("end", {"pad": "U2.1"})
    return c.call("route", p)


def expect_error(c, method, params, code):
    resp = c.request(method, params)
    check(not resp["ok"], f"{method}: expected error {code}, got ok")
    check(resp["error"]["code"] == code, f"{method}: expected {code}, got {resp['error']}")
    return resp["error"]


def clean(b, proposal, what):
    nb = drc_lite.apply(b, proposal)
    ids = {a["id"] for a in proposal["add"]}
    bad = drc_lite.violations(nb, ids)
    check(not bad, f"{what}: clearance violations {bad[:5]}")
    twice = drc_lite.overlaps(nb, ids)
    check(not twice, f"{what}: same-net copper laid twice {twice[:5]}")
    return nb


def connected(proposal, net, a, b):
    """Do the added tracks of `net` form ONE chain from point a to point b?
    (A walk over shared endpoints, not mere endpoint membership: review T-6.)"""
    adj = {}
    for it in proposal["add"]:
        if it["net"] == net and it["kind"] == "track":
            u, v = tuple(it["start"]), tuple(it["end"])
            adj.setdefault(u, set()).add(v)
            adj.setdefault(v, set()).add(u)
    a, b = tuple(a), tuple(b)
    seen, todo = {a}, [a]
    while todo:
        for n in adj.get(todo.pop(), ()):
            if n not in seen:
                seen.add(n)
                todo.append(n)
    return a in adj and b in seen


def raw_run(binary, data, args=(), env=None, timeout=120):
    """Run the binary over raw stdin bytes: (exit code, stdout bytes, seconds)."""
    t = time.perf_counter()
    p = subprocess.run([binary, *args], input=data, capture_output=True, timeout=timeout, env=env)
    return p.returncode, p.stdout, time.perf_counter() - t


def req_line(method, params=None, rid=1):
    r = {"id": rid, "protocol_version": PROTOCOL_VERSION, "method": method}
    if params is not None:
        r["params"] = params
    return json.dumps(r, separators=(",", ":"))


def hooks_env(**kv):
    env = dict(os.environ)
    env.update(kv)
    return env


# ---- synthetic -----------------------------------------------------------------

def t_protocol_errors(binary):
    with RouterClient(binary) as c:
        line = json.loads(c.raw("not json"))
        check(line["error"]["code"] == "parse_error" and line["id"] is None, f"parse_error: {line}")
        line = json.loads(c.raw('{"id":7,"protocol_version":99,"method":"ping"}'))
        check(line["error"]["code"] == "unsupported_protocol_version" and line["id"] == 7, f"version: {line}")
        line = json.loads(c.raw('{"id":"x","protocol_version":%d,"method":"nope"}' % PROTOCOL_VERSION))
        check(line["error"]["code"] == "unknown_method" and line["id"] == "x", f"unknown_method: {line}")
        line = json.loads(c.raw('[1,2]'))
        check(line["error"]["code"] == "invalid_request", f"invalid_request: {line}")
        expect_error(c, "route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U1.1"}}, "no_board")
        expect_error(c, "load_board", {"board": {"layers": []}}, "invalid_board")
        b = board("walkaround")
        b["pads"][0]["position"] = [1.5, 2]
        expect_error(c, "load_board", {"board": b}, "invalid_board")
        b = board("walkaround")
        b["pads"].append(dict(b["pads"][0]))
        err = expect_error(c, "load_board", {"board": b}, "invalid_board")
        check("duplicate" in err["message"], f"duplicate id message: {err}")
        c.call("load_board", {"board": board("walkaround")})
        expect_error(c, "route", {"net": "ZZ", "layer": "F.Cu", "start": {"pad": "U1.1"}, "end": {"pad": "U2.1"}},
                     "unknown_net")
        expect_error(c, "route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U3.1"}, "end": {"pad": "U2.1"}},
                     "net_mismatch")
        expect_error(c, "route", {"net": "A", "layer": "B.Cu", "start": {"pad": "U1.1"}, "end": {"pad": "U2.1"}},
                     "layer_mismatch")
        expect_error(c, "route", {"net": "A", "layer": "X.Cu", "start": {"pad": "U1.1"}}, "invalid_params")
        expect_error(c, "route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U1.1"}}, "invalid_params")
        expect_error(c, "route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U1.1"}, "end": {"pad": "U2.1"},
                                  "mode": "teleport"}, "invalid_params")
        expect_error(c, "drag", {"item": "U1.1", "to": [0, 0]}, "unknown_item")
        expect_error(c, "apply", {"proposal": {"remove": ["nope"], "add": []}, "base_revision": 0}, "stale_proposal")
        expect_error(c, "apply", {"proposal": {"remove": [], "add": []}, "base_revision": 5}, "stale_proposal")
        r = c.call("ping")
        check(r["pong"] is True, "ping after errors")


def t_base_revision_required(binary):
    """Protocol 2: `apply` without base_revision is invalid_params (review P-3)."""
    with RouterClient(binary) as c:
        c.call("load_board", {"board": board("walkaround")})
        expect_error(c, "apply", {"proposal": {"remove": [], "add": []}}, "invalid_params")
        check(c.call("stats")["revision"] == 0, "a refused apply changes nothing")
        check(c.call("apply", {"proposal": {"remove": [], "add": []}, "base_revision": 0})["revision"] == 1,
              "apply with the right base_revision")


def t_walkaround(binary):
    b = board("walkaround")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        r = route(c, mode="walkaround")
        p = r["proposal"]
        check(p["remove"] == [], f"walkaround removes nothing: {p['remove']}")
        check(len(p["add"]) >= 2, f"walkaround detours (>= 2 segments): {p['add']}")
        check(connected(p, "A", b["pads"][0]["position"], b["pads"][1]["position"]), "walkaround joins U1.1-U2.1")
        clean(b, p, "walkaround")
        # mark_obstacles refuses the straight line through U3.1
        expect_error(c, "route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U1.1"}, "end": {"pad": "U2.1"},
                                  "mode": "mark_obstacles"}, "collision")
        # route does not mutate: the same route again gives the same proposal
        check(route(c, mode="walkaround") == r, "route must not mutate the loaded board")


def t_free_point_start(binary):
    """A free-point start on no copper routes ON the requested net (review M-1, T-1):
    free point -> pad of its own net, free -> free, and every added item carries the net."""
    b = board("walkaround")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        start = [2 * MM, 10 * MM]
        r = c.request("route", {"net": "A", "layer": "F.Cu", "start": {"point": start}, "end": {"pad": "U2.1"}})
        check(r["ok"], f"free point -> pad of the same net routes: {r.get('error')}")
        p = r["result"]["proposal"]
        check(p["add"] and all(a["net"] == "A" for a in p["add"]), f"every added item is on net A: {p['add']}")
        check(connected(p, "A", start, b["pads"][1]["position"]), "the chain joins the point and U2.1")
        clean(b, p, "free point -> pad")
        r = c.call("route", {"net": "A", "layer": "F.Cu", "start": {"point": start},
                             "end": {"point": [28 * MM, 10 * MM]}})["proposal"]
        check(r["add"] and all(a["net"] == "A" for a in r["add"]), f"free -> free on net A: {r['add']}")
        check(connected(r, "A", start, [28 * MM, 10 * MM]), "free -> free joins its two points")
        clean(b, r, "free -> free")


def t_power_class_clearance(binary):
    """A Power-class route (clearance 0.3 mm) beside Default copper keeps the LARGER
    of the two classes' clearances, from a pad and from a free point (review T-3, M-1)."""
    b = board("power")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        for start in ({"pad": "P1.1"}, {"point": [3 * MM, 10 * MM]}):
            p = c.call("route", {"net": "VCC", "layer": "F.Cu", "start": start, "end": {"pad": "P2.1"}})["proposal"]
            check(all(a["net"] == "VCC" for a in p["add"]), f"{start}: every added item on VCC: {p['add']}")
            check(all(a["width"] == 500_000 for a in p["add"] if a["kind"] == "track"), f"{start}: Power width")
            clean(b, p, f"Power beside Default from {start}")


def t_shove(binary):
    b = board("shove")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        w = route(c, mode="walkaround")["proposal"]
        check(w["remove"] == [], f"walkaround leaves net C alone: {w['remove']}")
        clean(b, w, "shove board, walkaround")
        s = route(c, mode="shove")["proposal"]
        check(s["remove"], "shove moves the net-C track")
        check(set(s["remove"]) <= {"trk-c1", "trk-c2", "trk-c3"}, f"shove removes only C: {s['remove']}")
        check(any(a["net"] == "C" for a in s["add"]), "shove re-adds the shoved C copper")
        nb = clean(b, s, "shove")
        # apply, then chain: the applied board has the new copper
        res = c.call("apply", {"proposal": s, "base_revision": 0})
        check(res["revision"] == 1, f"apply bumps the revision: {res}")
        check(res["counts"]["tracks"] == len(nb["tracks"]), f"apply counts: {res} vs {len(nb['tracks'])}")
        expect_error(c, "apply", {"proposal": s, "base_revision": 0}, "stale_proposal")
        # drag a shoved C segment back up into the A track: shove mode pushes A
        c_track = next(a for a in s["add"] if a["net"] == "C" and a["start"][1] == a["end"][1])
        mid = [(c_track["start"][0] + c_track["end"][0]) // 2, c_track["start"][1]]
        d = c.call("drag", {"item": c_track["id"], "from": mid, "to": [mid[0], mid[1] - 2_000_000], "mode": "shove"})
        dp = d["proposal"]
        check(c_track["id"] in dp["remove"], f"drag removes the dragged segment: {dp}")
        clean(nb, dp, "drag")
        check(d["revision"] == 1, "drag reports the revision it was computed on")


def shove_lines(binary, env):
    with RouterClient(binary, env=env) as c:
        out = []
        for name, steps in determinism_requests():
            if name not in ("shove", "shove_drag", "arc_shove"):
                continue
            out += run_steps(c, name, steps)
        return out


def t_shove_no_wall_clock(binary):
    """Shove carries no wall-clock cap (review D-1): with every finite engine time
    limit forced to read as expired (TYPMAX_ROUTER_TEST_CLOCK=expired, the limit
    case of a loaded machine), the same shoves give byte-identical answers."""
    idle = shove_lines(binary, None)
    loaded = shove_lines(binary, hooks_env(TYPMAX_ROUTER_TEST_CLOCK="expired"))
    check(len(idle) >= 3, "the shove cases ran")
    for k, (a, b) in enumerate(zip(idle, loaded)):
        check(a == b, f"shove answer {k} moved under an expired engine clock:\n  idle   {a[:300]}\n  loaded {b[:300]}")


def t_via(binary):
    b = board("via")
    wp = [{"point": [15_000_000, 10_000_000], "via": True, "layer": "B.Cu"}]
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        expect_error(c, "route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U1.1"}, "end": {"pad": "U2.1"},
                                  "waypoints": wp}, "via_not_allowed")
        p = route(c, waypoints=wp, via_allowed=True)["proposal"]
        vias = [a for a in p["add"] if a["kind"] == "via"]
        check(len(vias) == 1 and vias[0]["position"] == wp[0]["point"], f"one via at the waypoint: {vias}")
        check(vias[0]["layers"] == ["F.Cu", "B.Cu"], f"through via: {vias[0]}")
        check(vias[0]["diameter"] == 600_000 and vias[0]["drill"] == 300_000, "via sizes from the net class")
        layers = {a["layer"] for a in p["add"] if a["kind"] == "track"}
        check(layers == {"F.Cu", "B.Cu"}, f"tracks on both layers: {layers}")
        clean(b, p, "via")
        # a Power-class net takes its class width
        b2 = board("via")
        b2["pads"][0]["net"] = b2["pads"][1]["net"] = "VCC"
        c.call("load_board", {"board": b2})
        p2 = route(c, net="VCC", waypoints=wp, via_allowed=True)["proposal"]
        check(all(a["width"] == 500_000 for a in p2["add"] if a["kind"] == "track"), "Power class width")
        check(all(a["diameter"] == 800_000 for a in p2["add"] if a["kind"] == "via"), "Power class via")


def t_via_drill_vs_diameter(binary):
    """A route never proposes a via whose drill is not smaller than its diameter (the
    service's own `apply` refuses one): such sizes are `invalid_params` at `route`
    time, after the net class's defaults are filled in (review r3 E-1)."""
    b = board("via")
    wp = [{"point": [15_000_000, 10_000_000], "via": True, "layer": "B.Cu"}]
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        for what, sizes in (("drill > diameter", {"via_diameter": 100_000, "via_drill": 900_000}),
                            ("drill == diameter", {"via_diameter": 400_000, "via_drill": 400_000}),
                            ("drill over the class diameter (600 um)", {"via_drill": 700_000}),
                            ("diameter under the class drill (300 um)", {"via_diameter": 250_000})):
            err = expect_error(c, "route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U1.1"},
                                            "end": {"pad": "U2.1"}, "waypoints": wp, "via_allowed": True, **sizes},
                               "invalid_params")
            check("via_drill" in err["message"] and "via_diameter" in err["message"], f"{what}: {err}")
        # sizes that are legal together, though each differs from the class: proposed, and applied
        r = route(c, waypoints=wp, via_allowed=True, via_diameter=500_000, via_drill=200_000)
        vias = [a for a in r["proposal"]["add"] if a["kind"] == "via"]
        check([(v["diameter"], v["drill"]) for v in vias] == [(500_000, 200_000)], f"the asked-for via: {vias}")
        c.call("apply", {"proposal": r["proposal"], "base_revision": r["revision"]})


def t_same_net_hole_to_hole(binary):
    """A via whose hole is 0.15 mm from a same-net pad hole (rule 0.25 mm) is never
    proposed; one at a legal distance is (review M-2). KiCad 10's engine checks
    hole pairs whatever their nets, so it refuses the via itself (unroutable); the
    service's own check (hole_too_close) stays behind it as a backstop."""
    b = board("holes")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        base = {"net": "A", "layer": "F.Cu", "start": {"pad": "J1.1"}, "end": {"pad": "U2.1"}, "via_allowed": True}
        near = dict(base, waypoints=[{"point": [10_700_000, 6 * MM], "via": True, "layer": "B.Cu"}])
        r = c.request("route", near)
        check(not r["ok"], f"the too-close via is refused: {r}")
        err = r["error"]
        check(err["code"] in ("hole_too_close", "unroutable"), f"refused as hole_too_close or unroutable: {err}")
        if err["code"] == "hole_too_close":
            check("J1.1" in err["message"], f"the message names the other hole: {err}")
        far = dict(base, waypoints=[{"point": [12 * MM, 6 * MM], "via": True, "layer": "B.Cu"}])
        p = c.call("route", far)["proposal"]
        check(any(a["kind"] == "via" for a in p["add"]), "the legal via is proposed")
        clean(b, p, "via at a legal hole distance")


def t_hole_clearance(binary):
    """A route past a non-plated hole keeps hole_clearance from it (review T-2)."""
    b = board("holes")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        p = c.call("route", {"net": "E", "layer": "F.Cu", "start": {"pad": "U3.1"}, "end": {"pad": "U4.1"}})["proposal"]
        check(connected(p, "E", b["pads"][2]["position"], b["pads"][3]["position"]), "E joins U3.1-U4.1")
        clean(b, p, "route past an NPTH")


def t_edge_clearance(binary):
    """A waypoint inside the edge clearance is refused or kept clear of the edge (T-2)."""
    b = board("walkaround")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        r = c.request("route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U1.1"},
                                "waypoints": [{"point": [5 * MM, 200_000]}]})
        if r["ok"]:
            clean(b, r["result"]["proposal"], "route toward the edge")
        else:
            check(r["error"]["code"] in ("unroutable", "start_not_routable"), f"edge: {r['error']}")
        p = c.call("route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U1.1"},
                             "waypoints": [{"point": [5 * MM, 500_000]}]})["proposal"]
        clean(b, p, "route 0.5 mm from the edge")


def t_arc_obstacle(binary):
    b = board("arc")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        p = route(c)["proposal"]
        check(connected(p, "A", b["pads"][0]["position"], b["pads"][1]["position"]), "A joins around the arc")
        clean(b, p, "walkaround past an arc")


def t_arc_obstacle_shove(binary):
    """A shove route across an arc of another net moves the arc or fails; it never
    goes through it (KiCad 10.0.7's shove did not search for arcs:
    patches/0001-shove-search-arc-obstacles.patch)."""
    b = board("arc")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        r = c.request("route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U1.1"}, "end": {"pad": "U2.1"},
                                "mode": "shove"})
        if r["ok"]:
            clean(b, r["result"]["proposal"], "shove past an arc")
        else:
            check(r["error"]["code"] == "unroutable", f"shove past an arc: {r['error']}")


def t_keepout_interior(binary):
    """A keepout blocks its whole area, not only its boundary (KiCad 10 syncs a
    rule area as triangles): a route across the board goes around it, and a route
    wholly inside it is refused."""
    b = board("walkaround")
    box = (13 * MM, 6 * MM, 17 * MM, 14 * MM)
    b["keepouts"] = [{"id": "K1", "layers": ["F.Cu"],
                      "polygon": [[box[0], box[1]], [box[2], box[1]], [box[2], box[3]], [box[0], box[3]]]}]

    def inside(x, y):
        return box[0] < x < box[2] and box[1] < y < box[3]

    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        p = route(c)["proposal"]
        check(connected(p, "A", b["pads"][0]["position"], b["pads"][1]["position"]), "A joins around the keepout")
        for t in p["add"]:
            if t["kind"] != "track" or t["layer"] != "F.Cu":
                continue
            for k in range(11):  # sample each track; none of it may be inside the box
                x = t["start"][0] + (t["end"][0] - t["start"][0]) * k / 10
                y = t["start"][1] + (t["end"][1] - t["start"][1]) * k / 10
                check(not inside(x, y), f"track {t['id']} enters the keepout at {x:.0f},{y:.0f}")
        clean(b, p, "route around a keepout")
        r = c.request("route", {"net": "A", "layer": "F.Cu", "start": {"point": [14 * MM, 8 * MM]},
                                "end": {"point": [16 * MM, 8 * MM]}})
        check(not r["ok"] and r["error"]["code"] in ("start_not_routable", "unroutable"),
              f"a route inside a keepout is refused: {r}")


def t_unroutable(binary):
    b = board("unroutable")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        for mode in ("walkaround", "shove"):
            err = expect_error(c, "route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U1.1"},
                                            "end": {"pad": "U2.1"}, "mode": mode}, "unroutable")
            check(err["message"], "an unroutable error says why")
        # the service is still healthy, and a route that avoids the ring works from a point
        r = c.call("route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U2.1"},
                             "waypoints": [{"point": [25_000_000, 15_000_000]}]})
        clean(b, r["proposal"], "dangling route")
        c.call("ping")


def t_zero_length_track(binary):
    """A zero-length track is refused, at load and in an applied proposal (review M-3)."""
    with RouterClient(binary) as c:
        b = board("walkaround")
        b["tracks"] = [{"id": "dot", "net": "B", "layer": "F.Cu", "start": [15 * MM, 15 * MM],
                        "end": [15 * MM, 15 * MM], "width": 2 * MM}]
        err = expect_error(c, "load_board", {"board": b}, "invalid_board")
        check("zero-length" in err["message"], f"names the cause: {err}")
        c.call("load_board", {"board": board("walkaround")})
        dot = dict(b["tracks"][0], kind="track")
        expect_error(c, "apply", {"proposal": {"remove": [], "add": [dot]}, "base_revision": 0}, "invalid_params")


def t_net_only_in_net_classes(binary):
    """A net named only in rules.net_classes is no net of the board (review M-4)."""
    with RouterClient(binary) as c:
        r = c.call("load_board", {"board": board("walkaround")})  # net_classes names VCC; no item carries it
        check(r["counts"]["nets"] == 2, f"two nets (A, B): {r['counts']}")
        expect_error(c, "route", {"net": "VCC", "layer": "F.Cu", "start": {"point": [2 * MM, 2 * MM]},
                                  "end": {"point": [4 * MM, 2 * MM]}}, "unknown_net")


def t_drag_no_net(binary):
    """Dragging a no-net item is refused (review M-5)."""
    b = board("walkaround")
    b["tracks"] = [{"id": "nn-1", "net": None, "layer": "B.Cu", "start": [5 * MM, 15 * MM],
                    "end": [10 * MM, 15 * MM], "width": 250_000}]
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        expect_error(c, "drag", {"item": "nn-1", "to": [7 * MM, 13 * MM]}, "drag_refused")


def t_one_layer_via(binary):
    """A via waypoint on a one-layer board is invalid_params (review R-6)."""
    b = board("walkaround")
    b["layers"] = ["F.Cu"]
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        expect_error(c, "route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U1.1"}, "end": {"pad": "U2.1"},
                                  "via_allowed": True,
                                  "waypoints": [{"point": [15 * MM, 5 * MM], "via": True, "layer": "F.Cu"}]},
                     "invalid_params")


def t_via_at_trace_end(binary):
    """A via waypoint where the trace already ends is invalid_params, never a crash (review r2 N-1)."""
    b = board("via")
    here = [15_000_000, 10_000_000]
    shapes = [
        ({"pad": "U1.1"}, [{"point": here, "layer": "F.Cu"}, {"point": here, "via": True, "layer": "B.Cu"}]),
        ({"point": here}, [{"point": here, "via": True, "layer": "B.Cu"}]),
    ]
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        for start, wps in shapes:
            expect_error(c, "route", {"net": "A", "layer": "F.Cu", "start": start, "end": {"pad": "U2.1"},
                                      "via_allowed": True, "waypoints": wps}, "invalid_params")
        check(c.call("ping") is not None, "the service is still alive")


def t_integers_only(binary):
    """5000000.0 and 1e7 are not integers (review P-2): refused in a board and in params."""
    with RouterClient(binary) as c:
        for pos in ([5000000.0, 10_000_000], [5_000_000, 1e7]):
            b = board("walkaround")
            b["pads"][0]["position"] = pos
            expect_error(c, "load_board", {"board": b}, "invalid_board")
        b = board("shove")
        b["tracks"][0]["width"] = 250000.0
        expect_error(c, "load_board", {"board": b}, "invalid_board")
        c.call("load_board", {"board": board("walkaround")})
        expect_error(c, "route", {"net": "A", "layer": "F.Cu", "start": {"point": [2e6, 1e7]},
                                  "end": {"pad": "U2.1"}}, "invalid_params")
        expect_error(c, "route", {"net": "A", "layer": "F.Cu", "start": {"pad": "U1.1"}, "end": {"pad": "U2.1"},
                                  "width": 250000.0}, "invalid_params")


def t_utf8(binary):
    """Invalid UTF-8 and lone surrogates are parse errors, and every response line
    is valid UTF-8 (review R-5); valid non-ASCII is echoed intact."""
    ok = req_line("ping", rid="né").encode()
    bad_byte = b'{"id":"a\xe9b","protocol_version":%d,"method":"ping"}' % PROTOCOL_VERSION
    overlong = b'{"id":"\xc0\xaf","protocol_version":%d,"method":"ping"}' % PROTOCOL_VERSION
    lone_lo = b'{"id":"\\udc00","protocol_version":%d,"method":"ping"}' % PROTOCOL_VERSION
    lone_hi = b'{"id":"\\ud800x","protocol_version":%d,"method":"ping"}' % PROTOCOL_VERSION
    stray = b'\xff'
    rc, out, _ = raw_run(binary, b"\n".join([ok, bad_byte, overlong, lone_lo, lone_hi, stray]) + b"\n")
    lines = out.split(b"\n")[:-1]
    check(len(lines) == 6, f"one response per line: {out!r}")
    resp = [json.loads(line.decode("utf-8")) for line in lines]  # raises on invalid UTF-8
    check(resp[0]["ok"] and resp[0]["id"] == "né", f"valid UTF-8 echoed: {resp[0]}")
    for r in resp[1:]:
        check(r["error"]["code"] == "parse_error" and r["id"] is None, f"parse_error: {r}")


def t_duplicate_keys_linear(binary):
    """80 000 keys in one object parse in well under a second (review R-1: the
    quadratic scan took 6.6 s), and a duplicate past the small-object scan is caught."""
    keys = ",".join(f'"k{i}":0' for i in range(80_000))
    big = ('{"id":1,"protocol_version":%d,"method":"ping","params":{%s}}' % (PROTOCOL_VERSION, keys)).encode()
    dup = ",".join(f'"k{i}":0' for i in range(20)) + ',"k3":1'
    dup = ('{"id":2,"protocol_version":%d,"method":"ping","params":{%s}}' % (PROTOCOL_VERSION, dup)).encode()
    rc, out, secs = raw_run(binary, big + b"\n" + dup + b"\n")
    lines = [json.loads(x) for x in out.decode().splitlines()]
    check(lines[0]["ok"], f"80 k keys: {lines[0]}")
    check(secs < 2.0, f"80 k keys took {secs:.2f} s (bound 2 s)")
    check(lines[1]["error"]["code"] == "parse_error" and "duplicate" in lines[1]["error"]["message"],
          f"a duplicate after 20 keys: {lines[1]}")


def t_parse_is_time_limited(binary):
    """Parsing runs under the time limit (review R-1): with --time-limit-ms 1, a
    40 MB line cannot parse in time; the answer is a fatal timeout and exit 3."""
    line = req_line("ping", {"pad": "x" * 40_000_000}, rid=9).encode()
    rc, out, _ = raw_run(binary, line + b"\n", args=("--time-limit-ms", "1"))
    resp = json.loads(out.decode().splitlines()[0])
    check(not resp["ok"] and resp["error"]["code"] == "timeout" and resp["error"].get("fatal") is True,
          f"parse timeout: {resp}")
    check(resp["id"] is None, f"the id is not known yet: {resp}")
    check(rc == 3, f"exit 3 after a timeout, got {rc}")


MAX_LINE_PROBE = r"""
import json, resource, subprocess, sys
p = subprocess.Popen([sys.argv[1], "--max-line-bytes", "1048576"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
chunk = b"x" * (1 << 20)
for _ in range(int(sys.argv[2])):
    p.stdin.write(chunk)
p.stdin.write(b"\n" + sys.argv[3].encode() + b"\n")
p.stdin.close()
out = p.stdout.read()
p.wait()
rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
print(json.dumps({"rc": p.returncode, "out": out.decode(), "rss": rss if sys.platform == "darwin" else rss * 1024}))
"""


def t_max_line_is_bounded(binary):
    """--max-line-bytes bounds memory while the line streams in (review R-2): a
    128 MiB line against a 1 MiB cap is answered request_too_large, the next line
    is served, and the process never holds the line (max RSS < 48 MiB)."""
    p = subprocess.run([sys.executable, "-c", MAX_LINE_PROBE, binary, "128", req_line("ping", rid=2)],
                       capture_output=True, text=True, timeout=300)
    r = json.loads(p.stdout)
    lines = [json.loads(x) for x in r["out"].splitlines()]
    check(len(lines) == 2 and lines[0]["error"]["code"] == "request_too_large", f"too large: {lines}")
    check(lines[1]["ok"] and lines[1]["id"] == 2, f"the next line is served: {lines}")
    check(r["rss"] < 48 * 2**20, f"max RSS {r['rss'] / 2**20:.0f} MiB holding a 128 MiB line")


def t_argv_strict(binary):
    """A malformed flag value is a usage error, not a silent self-DoS (review R-3)."""
    for args in (("--max-line-bytes", "abc"), ("--time-limit-ms", "abc"), ("--time-limit-ms", "0"),
                 ("--max-line-bytes", "-5"), ("--time-limit-ms", "10x")):
        rc, out, _ = raw_run(binary, (req_line("ping") + "\n").encode(), args=args)
        check(rc == 2 and out == b"", f"{args}: exit {rc}, stdout {out[:80]!r}")
    rc, out, _ = raw_run(binary, (req_line("ping") + "\n").encode(), args=("--time-limit-ms", "500"))
    check(rc == 0 and json.loads(out)["ok"], "a valid flag value still works")


def t_internal_crash(binary):
    """A crash answers internal_crash for the request in flight and exits 70 (review
    T-4); a crash before the next request's id is parsed carries a null id, never
    the previous, already answered, request's (review R-4)."""
    env = hooks_env(TYPMAX_ROUTER_TEST_HOOKS="1")
    data = (req_line("ping", rid=1) + "\n" + req_line("__test_crash__", rid=5) + "\n").encode()
    rc, out, _ = raw_run(binary, data, env=env)
    lines = [json.loads(x) for x in out.decode().splitlines()]
    check(lines[0]["ok"] and lines[1]["id"] == 5 and lines[1]["error"]["code"] == "internal_crash"
          and lines[1]["error"]["fatal"] is True, f"dispatch crash: {lines}")
    check(rc == 70, f"exit 70 after a crash, got {rc}")
    data = (req_line("ping", rid=1) + "\n" + req_line("ping", {"__test_crash_in_parse__": 1}, rid=2) + "\n").encode()
    rc, out, _ = raw_run(binary, data, env=env)
    lines = [json.loads(x) for x in out.decode().splitlines()]
    check(lines[0]["ok"] and lines[1]["error"]["code"] == "internal_crash", f"parse crash: {lines}")
    check(lines[1]["id"] is None, f"a crash before the id is parsed names no request: {lines[1]}")
    check(rc == 70, f"exit 70, got {rc}")
    # without the hook variable the hooks do nothing
    rc, out, _ = raw_run(binary, (req_line("__test_crash__", rid=3) + "\n").encode())
    check(rc == 0 and json.loads(out)["error"]["code"] == "unknown_method", f"hooks off: {out!r}")


def t_engine_identity(binary):
    """route/drag results name the engine (review P-4); a route that changes nothing
    is a successful EMPTY proposal (review D-3)."""
    with RouterClient(binary) as c:
        c.call("load_board", {"board": board("walkaround")})
        r = route(c)
        e = r["engine"]
        manifest = dict(l.split(" ", 1) for l in (HERE.parent / "vendor" / "kicad.manifest").read_text().splitlines()
                        if l.startswith(("tag ", "commit ")))
        check(e["name"] == "typmax-router" and e["protocol"] == PROTOCOL_VERSION and e["version"]
              and manifest["tag"] in e["pns"] and manifest["commit"][:12] in e["pns"], f"engine identity: {e}")
        check(c.call("ping")["engine"] == e, "ping names the same engine")
        c.call("apply", {"proposal": r["proposal"], "base_revision": 0})
        again = route(c)
        check(again["proposal"] == {"remove": [], "add": []}, f"empty proposal: {again['proposal']}")


def t_error_detail(binary):
    """An engine refusal keeps PNS's own text in `detail`, apart from `message` (P-5)."""
    with RouterClient(binary) as c:
        c.call("load_board", {"board": board("walkaround")})
        r = c.request("route", {"net": "A", "layer": "F.Cu", "start": {"point": [15 * MM, 10 * MM]},
                                "end": {"pad": "U2.1"}})
        check(not r["ok"] and r["error"]["code"] == "start_not_routable", f"start inside U3.1: {r}")
        check("detail" in r["error"] and r["error"]["detail"] not in r["error"]["message"],
              f"detail apart from message: {r['error']}")


def t_checker_self_test(binary):
    """The independent checker sees every rule the board maps (review T-2), and
    `connected` walks a chain (review T-6): planted violations are found."""
    b = board("holes")
    near_hole = {"id": "x1", "net": "E", "layer": "F.Cu", "start": [10 * MM, 15_800_000],
                 "end": [20 * MM, 15_800_000], "width": 250_000}  # 0.175 mm from H1's hole (1.5 mm radius)
    bad = drc_lite.violations(dict(b, tracks=[near_hole]), {"x1"})
    check(any(v[1] == "H1.1(hole)" for v in bad), f"hole_clearance violation seen: {bad}")
    # a stale copy of a proposal's own copper (review PR #11 r1 M-1): seen, and a mere joint is not
    leg = {"id": "o1", "net": "A", "layer": "F.Cu", "start": [5 * MM, 10 * MM], "end": [8 * MM, 10 * MM], "width": 250_000}
    longer = dict(leg, id="o2", end=[13 * MM, 10 * MM])
    after = dict(leg, id="o3", start=[8 * MM, 10 * MM], end=[13 * MM, 10 * MM])
    check(drc_lite.overlaps({"tracks": [leg, longer]}, {"o2"}) == [("o1", "o2", 3 * MM)], "a laid-twice track is seen")
    check(drc_lite.overlaps({"tracks": [leg, after]}, {"o3"}) == [], "two tracks meeting end to end are not")
    near_edge = dict(near_hole, id="x2", start=[10 * MM, 200_000], end=[20 * MM, 200_000])
    bad = drc_lite.violations(dict(b, tracks=[near_edge]), {"x2"})
    check(any(v[1] == "edge" for v in bad), f"edge_clearance violation seen: {bad}")
    via = {"id": "v1", "net": "A", "position": [10_700_000, 6 * MM], "diameter": 600_000, "drill": 300_000,
           "layers": ["F.Cu", "B.Cu"]}
    bad = drc_lite.violations(dict(b, vias=[via]), {"v1"})
    check(any("J1.1" in v[:2] for v in bad), f"same-net hole_to_hole seen: {bad}")
    arc_b = board("arc")
    straight = {"id": "x3", "net": "A", "layer": "F.Cu", "start": [5 * MM, 8 * MM], "end": [25 * MM, 8 * MM],
                "width": 250_000}  # through the arc's apex (15, 8); clear of its chord at y = 13
    bad = drc_lite.violations(dict(arc_b, tracks=arc_b["tracks"] + [straight]), {"x3"})
    check(any("arc-b1" in v[:2] for v in bad), f"an arc is judged as an arc: {bad}")
    disjoint = {"add": [{"kind": "track", "net": "A", "start": [0, 0], "end": [1, 0]},
                        {"kind": "track", "net": "A", "start": [5, 0], "end": [6, 0]}]}
    check(not connected(disjoint, "A", [0, 0], [6, 0]), "two disjoint segments are not a chain")
    joined = {"add": disjoint["add"] + [{"kind": "track", "net": "A", "start": [1, 0], "end": [5, 0]}]}
    check(connected(joined, "A", [0, 0], [6, 0]), "a chain is a chain")


TINY_PCB = """(kicad_pcb (version 20240108) (generator "pcbnew")
  (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (44 "Edge.Cuts" user))
  (net 0 "") (net 1 "A")
  (segment (start 1 1) (end 1 1) (width 0.2) (layer "F.Cu") (net 1) (uuid "u1"))
  (segment (start 1 1) (end 3 1) (width 0.2) (layer "F.Cu") (net 1) (uuid "u2")))
"""
TINY_PRO = {"board": {"design_settings": {"rules": {"min_clearance": 0.3, "min_track_width": 0.3,
                                                       "min_via_diameter": 0.7, "min_through_hole_diameter": 0.35}}},
            "net_settings": {"classes": [{"name": "Default", "clearance": 0.2, "track_width": 0.25,
                                          "via_diameter": 0.6, "via_drill": 0.3},
                                         {"name": "Power", "clearance": 0.4, "track_width": 0.5,
                                          "via_diameter": 0.8, "via_drill": 0.4}]}}


def t_converter(binary):
    """kicad_to_board floors every class by the board minimums (review K-1) and
    turns a zero-length segment into a copper disc the service accepts (M-3)."""
    import tempfile
    import kicad_to_board
    with tempfile.TemporaryDirectory() as d:
        pcb, pro = pathlib.Path(d, "t.kicad_pcb"), pathlib.Path(d, "t.kicad_pro")
        pcb.write_text(TINY_PCB)
        pro.write_text(json.dumps(TINY_PRO))
        b = kicad_to_board.convert(pcb, pro)
    cls = b["rules"]["classes"]
    check(cls["Default"] == {"clearance": 300_000, "track_width": 300_000, "via_diameter": 700_000,
                             "via_drill": 350_000}, f"Default floored: {cls['Default']}")
    check(cls["Power"] == {"clearance": 400_000, "track_width": 500_000, "via_diameter": 800_000,
                           "via_drill": 400_000}, f"Power above every floor: {cls['Power']}")
    check([t["id"] for t in b["tracks"]] == ["u2"] and [c["id"] for c in b["copper"]] == ["u1"],
          f"the dot is copper: {b['tracks']} {b['copper']}")
    b["outline"] = [[[0, 0], [5 * MM, 0], [5 * MM, 5 * MM], [0, 5 * MM]]]
    with RouterClient(binary) as c:
        check(c.call("load_board", {"board": b})["counts"]["copper"] == 1, "the service loads it")


def t_fixture_zone_filter(binary):
    """The fixture DRC strips copper zones only; keepout zones stay (review T-7)."""
    import fixture_tests
    from kicad_sexpr import parse
    keepout, _ = parse('(kicad_pcb (zone (net 0) (layer "F.Cu") (keepout (tracks not_allowed))))')
    copper, _ = parse('(kicad_pcb (zone (net 1) (net_name "GND") (layer "F.Cu") (filled_polygon)))')
    check(not fixture_tests.is_copper_zone(keepout[1]), "a keepout zone is kept")
    check(fixture_tests.is_copper_zone(copper[1]), "a copper zone is stripped")


LICENCE_OF = {"tools": "MIT", "tests": "MIT", "docker": "MIT", "src": "GPL-3.0-or-later",
              "shim": "GPL-3.0-or-later", "patches": "GPL-3.0-or-later", ".github": "MIT"}


def t_spdx_headers(binary):
    """Every file of ours names its licence, and the split is the one README.md
    states: MIT tools/tests/docker, GPL src/shim, CC0 protocol (review P-1)."""
    root = HERE.parent
    for d, lic in LICENCE_OF.items():
        for f in sorted((root / d).rglob("*")):
            if f.is_dir() or "__pycache__" in f.parts or f.suffix in (".json", ".jsonl"):
                continue
            head = f.read_text(encoding="utf-8").splitlines()[:3]
            check(any(f"SPDX-License-Identifier: {lic}" in x for x in head), f"{f.relative_to(root)}: not {lic}")
    check("SPDX-License-Identifier: CC0-1.0" in (root / "PROTOCOL.md").read_text().splitlines()[0], "PROTOCOL.md")
    check("SPDX-License-Identifier: GPL-3.0-or-later" in (root / "CMakeLists.txt").read_text(), "CMakeLists.txt")
    for f in ("GPL-3.0-or-later.txt", "MIT.txt", "CC0-1.0.txt"):
        check((root / "LICENSES" / f).is_file(), f"LICENSES/{f}")
    # and EVERY file of ours, not only the code directories (review r3 L-1): an SPDX line
    # near its top, or a REUSE.toml annotation. vendor/kicad/ keeps KiCad's headers (the
    # manifest lists each licence); the licence texts are exempt, as REUSE has them.
    globs = reuse_globs(root)
    missing = []
    for rel in repo_files(root):
        if rel.startswith(("vendor/kicad/", "LICENSES/")) or rel == "LICENSE":
            continue
        if any(g.fullmatch(rel) for g in globs):
            continue
        try:
            head = (root / rel).read_text(encoding="utf-8", errors="replace").splitlines()[:5]
        except OSError:
            head = []
        if not any("SPDX-License-Identifier: " in x for x in head):
            missing.append(rel)
    check(not missing, f"files with no SPDX line and no REUSE.toml annotation: {missing}")


def repo_files(root):
    """The repository's files, relative: `git ls-files` in a checkout; in a copy without
    git (the docker build's), every file under root but build output and caches."""
    import subprocess
    try:
        r = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True, timeout=60)
        if r.returncode == 0 and r.stdout:
            return [x for x in r.stdout.decode().split("\0") if x and (root / x).is_file()]
    except (OSError, subprocess.TimeoutExpired):
        pass
    skip = {"__pycache__", ".git", "build", "fixtures", "reports"}
    return [str(f.relative_to(root)) for f in sorted(root.rglob("*"))
            if f.is_file() and not (set(f.relative_to(root).parts) & skip)
            and not f.relative_to(root).parts[0].startswith("build-")]


def reuse_globs(root):
    """Every REUSE.toml's annotation paths as regular expressions over repository paths
    (`**` any path, `*` within a directory, as REUSE reads them). A REUSE.toml below the
    root annotates paths relative to its own directory (REUSE 3.2)."""
    import re
    import tomllib
    out = []
    for rel in sorted(r for r in repo_files(root) if r.split("/")[-1] == "REUSE.toml"):
        base = rel[: -len("REUSE.toml")]
        for ann in tomllib.loads((root / rel).read_text()).get("annotations", []):
            paths = ann["path"] if isinstance(ann["path"], list) else [ann["path"]]
            for g in paths:
                rx = re.escape(base) + "".join(
                    ".*" if t == "**" else "[^/]*" if t == "*" else re.escape(t)
                    for t in re.split(r"(\*\*|\*)", g))
                out.append(re.compile(rx))
    return out


def t_no_local_paths(binary):
    """No file of ours names a developer's machine: no home directory, no temp
    directory of a session (review r3 H-1, H-2). vendor/kicad/ is upstream's."""
    import re
    root = HERE.parent
    bad = re.compile("|".join(["/" + "Users/[A-Za-z]", "/" + "home/[a-z]", "/" + "private/(tmp|var)/"]))
    # a pinned SDK is a build assumption: barred from code and build files, though a
    # document under docs/ may quote the one it found
    sdk = re.compile("/" + "Library/Developer/CommandLineTools/SDKs/MacOSX[0-9]")
    hits = []
    for rel in repo_files(root):
        if rel.startswith("vendor/kicad/"):
            continue
        try:
            text = (root / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for n, line in enumerate(text.splitlines(), 1):
            if bad.search(line) or (sdk.search(line) and not rel.startswith("docs/")):
                hits.append(f"{rel}:{n}")
    check(not hits, f"local paths in the tree: {hits[:20]}")


def t_patch_notices(binary):
    """Every patch in patches/series adds, to every file it modifies, the dated
    "Modified by the typmax-router authors" notice GPL-3.0 section 5(a) asks of a
    modified file, naming the patch (review r3 L-2); vendor/ itself stays upstream."""
    import re
    root = HERE.parent
    series = [l.strip() for l in (root / "patches" / "series").read_text().splitlines()
              if l.strip() and not l.startswith("#")]
    check(series, "patches/series lists patches")
    for name in series:
        text = (root / "patches" / name).read_text(encoding="utf-8")
        files = re.findall(r"^\+\+\+ b/(\S+)", text, re.M)
        check(files, f"{name}: modifies no file")
        for f in files:
            start = text.index(f"+++ b/{f}")
            nxt = text.find("\n--- a/", start)
            hunk = text[start:nxt if nxt >= 0 else len(text)]
            check(re.search(r"^\+ \* Modified by the typmax-router authors, \d{4}-\d{2}-\d{2}: ", hunk, re.M)
                  and f"patches/{name}" in hunk.replace("\n+ *   ", " "),
                  f"{name}: {f} gets no dated notice naming the patch")
            check("Modified by the typmax-router" not in (root / "vendor" / "kicad" / f).read_text(errors="replace"),
                  f"vendor/kicad/{f} is edited in place")


def t_timeout(binary):
    """A request over its time limit gets a fatal `timeout` error and the process exits."""
    big = board("walkaround")
    # 100 k tracks: load_board cannot finish in 1 ms on any machine
    for i in range(100_000):
        big["tracks"].append({"id": f"f{i}", "net": "Z", "layer": "B.Cu", "start": [i * 1000, 0],
                              "end": [i * 1000 + 500, 0], "width": 100})
    c = RouterClient(binary)
    resp = c.request("load_board", {"board": big, "time_limit_ms": 1})
    check(resp["error"]["code"] == "timeout" and resp["error"].get("fatal") is True, f"timeout: {resp}")
    check(c.proc.wait(timeout=10) == 3, "the service exits after a timeout")


# ---- route_pair, tune, ping.methods (issue #10) --------------------------------

METHODS = ["ping", "stats", "load_board", "route", "drag", "apply", "route_pair", "tune"]
PAIR = {"p": "DP", "n": "DN", "layer": "F.Cu", "gap": 250_000,
        "start": {"p": {"pad": "U1.1"}, "n": {"pad": "U1.2"}},
        "end": {"p": {"pad": "U2.1"}, "n": {"pad": "U2.2"}}}
TUNE = {"aim": "length", "net": "A", "target": 25 * MM, "tolerance": 100_000, "track": "trk-a1",
        "from": [8 * MM, 10 * MM], "to": [22 * MM, 10 * MM]}
TUNE_PAIR = {"aim": "pair_length", "p": "DP", "n": "DN", "target": 24 * MM, "tolerance": 100_000,
             "track": "trk-p1", "from": [8 * MM, 10 * MM], "to": [22 * MM, 10 * MM]}


def pad_pos(b, pid):
    return next(p["position"] for p in b["pads"] if p["id"] == pid)


def seg_gap(s, t):
    """Edge-to-edge distance of two straight tracks."""
    return drc_lite._dist(("capsule", (s["start"], s["end"], s["width"] / 2)),
                          ("capsule", (t["start"], t["end"], t["width"] / 2)))


def parallel(s, t):
    """Two straight tracks pointing the same way (to within rounding)."""
    a = (s["end"][0] - s["start"][0], s["end"][1] - s["start"][1])
    b = (t["end"][0] - t["start"][0], t["end"][1] - t["start"][1])
    return abs(a[0] * b[1] - a[1] * b[0]) <= 1e-6 * math.hypot(*a) * math.hypot(*b)


def net_length(board_, net):
    """Track length of a net (every straight track; the boards here hold no arcs)."""
    return sum(math.dist(t["start"], t["end"]) for t in board_["tracks"] if t["net"] == net)


def track_count(board_, net):
    return sum(1 for t in board_["tracks"] if t["net"] == net)


def t_ping_methods(binary):
    """ping lists every method, and every listed method is one the binary serves."""
    with RouterClient(binary) as c:
        got = c.call("ping")["methods"]
        check(got == METHODS, f"ping.methods: {got}")
        for m in got:
            r = c.request(m, {})
            check(r["ok"] or r["error"]["code"] != "unknown_method", f"{m} is listed but unknown")


def t_route_pair(binary):
    """Walkaround around a pad: both members connected end to end, clean, at the
    class width, never closer to each other than the gap, and applicable."""
    b = board("pair")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        res = c.call("route_pair", PAIR)
        pr = res["proposal"]
        check(pr["remove"] == [], f"walkaround removed {pr['remove']}")
        check({a["net"] for a in pr["add"]} == {"DP", "DN"}, f"nets: {pr['add']}")
        check(all(a["width"] == 250_000 for a in pr["add"] if a["kind"] == "track"), "a member off its class width")
        check(connected(pr, "DP", pad_pos(b, "U1.1"), pad_pos(b, "U2.1")), "DP not connected pad to pad")
        check(connected(pr, "DN", pad_pos(b, "U1.2"), pad_pos(b, "U2.2")), "DN not connected pad to pad")
        clean(b, pr, "route_pair walkaround")
        ps = [a for a in pr["add"] if a["net"] == "DP"]
        ns = [a for a in pr["add"] if a["net"] == "DN"]
        closest = min(seg_gap(s, t) for s in ps for t in ns)
        check(closest >= 250_000 - 1_000, f"members {closest:.0f} nm apart, under the 250000 nm gap")
        again = c.request("route_pair", PAIR, raw_response=True)
        check(json.loads(again)["result"] == res, "the same route_pair twice differs")
        applied = c.call("apply", {"proposal": pr, "base_revision": res["revision"]})
        check(applied["added"] == len(pr["add"]), f"apply: {applied}")


def t_route_pair_shove(binary):
    """Shove pushes the net-C track out of the pair's way; walkaround leaves it."""
    b = board("pair_shove")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        pr = c.call("route_pair", dict(PAIR, mode="shove"))["proposal"]
        check("trk-c2" in pr["remove"], f"shove did not move trk-c2: {pr['remove']}")
        check(any(a["net"] == "C" for a in pr["add"]), "shove dropped net C's copper")
        check(connected(pr, "DP", pad_pos(b, "U1.1"), pad_pos(b, "U2.1")), "shove: DP not connected")
        check(connected(pr, "DN", pad_pos(b, "U1.2"), pad_pos(b, "U2.2")), "shove: DN not connected")
        clean(b, pr, "route_pair shove")
        pr = c.call("route_pair", PAIR)["proposal"]
        check(pr["remove"] == [] and {a["net"] for a in pr["add"]} == {"DP", "DN"}, f"walkaround: {pr['remove']}")
        clean(b, pr, "route_pair walkaround on the shove board")


def pair_via_request(via_gap=None):
    r = dict(PAIR, via_allowed=True,
             waypoints=[{"point": [8 * MM, 10_250_000], "via": True, "layer": "B.Cu"}],
             end={"p": {"point": [25 * MM, 4 * MM]}, "n": {"point": [25 * MM, 4_500_000]}})
    if via_gap is not None:
        r["via_gap"] = via_gap
    return r


def t_route_pair_vias(binary):
    """Twin vias at a waypoint, one per member, at via_gap edge to edge (absent:
    the least the rules admit), then free-point ends on the other layer."""
    b = board("pair_shove")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        for via_gap, pitch in ((None, 600_000 + 200_000), (300_000, 600_000 + 300_000)):
            pr = c.call("route_pair", pair_via_request(via_gap))["proposal"]
            vias = [a for a in pr["add"] if a["kind"] == "via"]
            check(sorted(v["net"] for v in vias) == ["DN", "DP"], f"vias: {vias}")
            check(all(v["layers"] == ["F.Cu", "B.Cu"] and v["diameter"] == 600_000 for v in vias), f"vias: {vias}")
            d = math.dist(vias[0]["position"], vias[1]["position"])
            check(abs(d - pitch) <= 2, f"via_gap {via_gap}: vias {d:.0f} nm apart, want {pitch}")
            check(connected(pr, "DP", pad_pos(b, "U1.1"), vias[1]["position"] if vias[1]["net"] == "DP"
                            else vias[0]["position"]), "DP does not reach its via")
            for net, end in (("DP", [25 * MM, 4 * MM]), ("DN", [25 * MM, 4_500_000])):
                check(any(a["kind"] == "track" and a["net"] == net and a["layer"] == "B.Cu" and end in (a["start"], a["end"])
                          for a in pr["add"]), f"{net} does not end at its free point on B.Cu")
            clean(b, pr, f"route_pair vias (via_gap {via_gap})")
        req = pair_via_request()
        del req["via_allowed"]
        expect_error(c, "route_pair", req, "via_not_allowed")


def t_route_pair_dangling(binary):
    """No end: the pair ends at its last waypoint, its centreline there."""
    with RouterClient(binary) as c:
        c.call("load_board", {"board": board("pair")})
        req = {k: v for k, v in PAIR.items() if k != "end"}
        req["waypoints"] = [{"point": [10 * MM, 6 * MM]}]
        pr = c.call("route_pair", req)["proposal"]
        ends = {}
        for a in pr["add"]:
            ends.setdefault(a["net"], set()).update({tuple(a["start"]), tuple(a["end"])})
        far = {n: max(e, key=lambda p: math.dist(p, (5 * MM, 10 * MM))) for n, e in ends.items()}
        mid = [(far["DP"][0] + far["DN"][0]) / 2, (far["DP"][1] + far["DN"][1]) / 2]
        check(math.dist(mid, [10 * MM, 6 * MM]) <= 2, f"pair centreline ends at {mid}, not the waypoint")
        clean(board("pair"), pr, "dangling pair")
        # the coupled run sits at exactly `gap` (review PR #11 r1 n-4), here 0.3 mm, not the pads' 0.25 mm
        pr = c.call("route_pair", dict(req, gap=300_000))["proposal"]
        ps = [a for a in pr["add"] if a["net"] == "DP"]
        ns = [a for a in pr["add"] if a["net"] == "DN"]
        # the long diagonal run to the waypoint (the short fan-out legs beside the pads are not coupled)
        run = max(ps, key=lambda t: math.dist(t["start"], t["end"]))
        check(run["end"][1] != run["start"][1], f"the longest DP leg is not the diagonal run: {run}")
        g = min(seg_gap(run, t) for t in ns if parallel(run, t))
        check(abs(g - 300_000) <= 2, f"coupled gap {g:.1f} nm, want 300000 +- 2")


def pair_legs(waypoints, **kw):
    return dict(PAIR, waypoints=[{"point": p} for p in waypoints], **kw)


def t_route_pair_waypoints(binary):
    """A waypoint, then more routing (review PR #11 r1 M-1): every leg the placer committed
    and then merged or shoved again leaves the proposal, so each member is one chain of
    copper laid once, clean, in walkaround and in shove."""
    cases = [("pair", pair_legs([[8 * MM, 10_250_000]])),
             ("pair", pair_legs([[8 * MM, 10_250_000], [10 * MM, 10_250_000]])),
             ("pair_shove", pair_legs([[12 * MM, 10_250_000]], mode="shove")),
             ("pair_shove", pair_legs([[8 * MM, 10_250_000], [14 * MM, 10_250_000]], mode="shove"))]
    with RouterClient(binary) as c:
        for name, req in cases:
            b = board(name)
            c.call("load_board", {"board": b})
            pr = c.call("route_pair", req)["proposal"]
            what = f"{name} {req.get('mode', 'walkaround')} {len(req['waypoints'])} waypoint(s)"
            clean(b, pr, what)
            check(connected(pr, "DP", pad_pos(b, "U1.1"), pad_pos(b, "U2.1")), f"{what}: DP not connected")
            check(connected(pr, "DN", pad_pos(b, "U1.2"), pad_pos(b, "U2.2")), f"{what}: DN not connected")
            if name == "pair_shove":
                check("trk-c2" in pr["remove"], f"{what}: shove did not move trk-c2")
                cs = [a for a in pr["add"] if a["net"] == "C"]
                check(connected(pr, "C", pad_pos(b, "J1.1"), pad_pos(b, "J2.1")), f"{what}: net C broken")
                check(len(cs) == len({(tuple(a["start"]), tuple(a["end"])) for a in cs}), f"{what}: net C twice")
        # the dangling pair of two collinear waypoints is one run, as route's is
        req = {k: v for k, v in pair_legs([[8 * MM, 10_250_000], [10 * MM, 10_250_000]]).items() if k != "end"}
        c.call("load_board", {"board": board("pair")})
        pr = c.call("route_pair", req)["proposal"]
        clean(board("pair"), pr, "dangling, two collinear waypoints")


OFF_AXIS = [("pair", [[9 * MM, 9_100_000]]), ("pair", [[7 * MM, 10_250_000], [11 * MM, 13_250_000]]),
            ("pair_shove", [[13 * MM, 12_700_000]])]


def t_route_pair_off_axis(binary):
    """A waypoint off the pads' line, then an end (review PR #11 r2 R2-M1): the engine cannot carry the pair on from
    a leg fixed on a diagonal, and its placer answers true for a leg it never lays. Each such request is unroutable,
    in walkaround and in shove, never ok without the end leg; on the line the same request routes, joined and clean."""
    with RouterClient(binary) as c:
        for name, wps in OFF_AXIS:
            b = board(name)
            c.call("load_board", {"board": b})
            for mode in ("walkaround", "shove"):
                err = expect_error(c, "route_pair", pair_legs(wps, mode=mode), "unroutable")
                check("end pair" in err["message"], f"{name} {mode} {wps}: {err}")
        # the same shapes on the pads' line: routed, both members pad to pad, clean (t_route_pair_waypoints' check)
        b = board("pair")
        c.call("load_board", {"board": b})
        for mode in ("walkaround", "shove"):
            pr = c.call("route_pair", pair_legs([[9 * MM, 10_250_000], [11 * MM, 10_250_000]], mode=mode))["proposal"]
            clean(b, pr, f"on the pads' line, {mode}")
            check(connected(pr, "DP", pad_pos(b, "U1.1"), pad_pos(b, "U2.1")), f"{mode}: DP not joined")
            check(connected(pr, "DN", pad_pos(b, "U1.2"), pad_pos(b, "U2.2")), f"{mode}: DN not joined")


def final_copper(b, pr):
    """The proposal's copper once applied: its added tracks and the board's tracks it keeps."""
    gone = set(pr["remove"])
    return {"add": pr["add"] + [dict(t, kind="track") for t in b["tracks"] if t["id"] not in gone]}


def stubs(b, xs):
    """`b` with in-line DP/DN stubs from x0 to x1 mm on the pads' lines (y 10 and 10.5 mm), one per (x0, x1)."""
    tracks = []
    for k, (x0, x1) in enumerate(xs):
        for net, y in (("DP", 10 * MM), ("DN", 10_500_000)):
            tracks.append({"id": f"stub-{net.lower()}{k}", "net": net, "layer": "F.Cu", "start": [x0 * MM, y],
                           "end": [x1 * MM, y], "width": 250_000})
    return dict(b, tracks=b["tracks"] + tracks)


def t_route_pair_from_stubs(binary):
    """A pair started from, or landed on, in-line track ends (review PR #11 r3 R3-M1): KiCad merges the old track into
    the new segment, which then runs through the anchor, so the service's own check walks the final copper (added
    plus kept) and matches an anchor anywhere along a track. Each request is ok, clean, and joined pad to pad over the
    final copper, in walkaround and in shove, with and without a waypoint."""
    start = dict(PAIR, start={"p": {"point": [8 * MM, 10 * MM]}, "n": {"point": [8 * MM, 10_500_000]}})
    end = dict(PAIR, end={"p": {"point": [22 * MM, 10 * MM]}, "n": {"point": [22 * MM, 10_500_000]}})
    cases = [(stubs(board("pair"), [(5, 8)]), start), (stubs(board("pair"), [(5, 8)]), dict(start, waypoints=[{"point": [12 * MM, 10_250_000]}])),
             (stubs(board("pair"), [(22, 25)]), end)]
    with RouterClient(binary) as c:
        for b, req in cases:
            c.call("load_board", {"board": b})
            for mode in ("walkaround", "shove"):
                what = f"{mode} {req['start']} → {req['end']}"
                pr = c.call("route_pair", dict(req, mode=mode))["proposal"]
                clean(b, pr, what)
                whole = final_copper(b, pr)
                check(connected(whole, "DP", pad_pos(b, "U1.1"), pad_pos(b, "U2.1")), f"{what}: DP not joined")
                check(connected(whole, "DN", pad_pos(b, "U1.2"), pad_pos(b, "U2.2")), f"{what}: DN not joined")


def open_pair():
    """The `pair` board without its middle obstacles (U3 and U4): room for a pair to leave the pads' line and come
    back (review PR #11 r3 R3-M2)."""
    b = board("pair")
    return dict(b, pads=[p for p in b["pads"] if not p["id"].startswith(("U3", "U4"))])


# off the pads' line, along, back onto it, then the end: one bend per leg (PROTOCOL.md, route_pair)
DETOURS = [[[9 * MM, 9_100_000], [12 * MM, 9_100_000], [14 * MM, 10_250_000]],
           [[9 * MM, 11_400_000], [12 * MM, 11_400_000], [14 * MM, 10_250_000]]]


def t_route_pair_detour(binary):
    """A detour off the pads' line and back, a waypoint at each bend (review PR #11 r3 R3-M2): KiCad's continuation
    gateways after a fixed waypoint led by sin(22.5°) of the pitch where a 45° turn needs tan(22.5°), and covered one
    diagonal only, so the pair could not bend at a fixed point (patches/0005). Above and below the line, in walkaround
    and in shove: ok, clean, both members joined pad to pad."""
    b = open_pair()
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        for wps in DETOURS:
            for mode in ("walkaround", "shove"):
                what = f"{mode} {wps}"
                pr = c.call("route_pair", pair_legs(wps, mode=mode))["proposal"]
                clean(b, pr, what)
                check(connected(pr, "DP", pad_pos(b, "U1.1"), pad_pos(b, "U2.1")), f"{what}: DP not joined")
                check(connected(pr, "DN", pad_pos(b, "U1.2"), pad_pos(b, "U2.2")), f"{what}: DN not joined")


def t_route_pair_errors(binary):
    """Every refusal of route_pair, each with its code."""
    with RouterClient(binary) as c:
        expect_error(c, "route_pair", PAIR, "no_board")
        b = board("pair")
        c.call("load_board", {"board": b})
        bad = [dict(PAIR, n="DP"), {k: v for k, v in PAIR.items() if k != "gap"}, dict(PAIR, gap=0),
               dict(PAIR, gap=1.5e5), dict(PAIR, mode="mark_obstacles"), dict(PAIR, start={"p": {"pad": "U1.1"}}),
               dict(PAIR, start={"p": {"pad": "U1.1"}, "n": {"pad": "U1.2", "point": [0, 0]}}),
               {k: v for k, v in PAIR.items() if k != "end"}, dict(PAIR, layer="In9.Cu"), dict(PAIR, via_gap=-1),
               dict(PAIR, waypoints=[{"point": [10 * MM, 6 * MM]}, {"point": [10 * MM, 6 * MM]}])]
        for req in bad:
            expect_error(c, "route_pair", req, "invalid_params")
        expect_error(c, "route_pair", dict(PAIR, p="ZZ"), "unknown_net")
        expect_error(c, "route_pair", dict(PAIR, start={"p": {"pad": "U9.9"}, "n": {"pad": "U1.2"}}), "unknown_item")
        err = expect_error(c, "route_pair", dict(PAIR, start={"p": {"pad": "U1.2"}, "n": {"pad": "U1.1"}}),
                           "pair_mismatch")
        check("U1.2" in err["message"], f"pair_mismatch names the pad: {err}")
        expect_error(c, "route_pair", dict(PAIR, end={"p": {"pad": "U2.1"}, "n": {"pad": "U3.1"}}), "pair_mismatch")
        expect_error(c, "route_pair", dict(PAIR, layer="B.Cu"), "layer_mismatch")
        expect_error(c, "route_pair", dict(PAIR, gap=199_999), "gap_too_small")
        # a waypoint inside the net-B pad: the placer stops short of it (review PR #11 r1 M-2)
        err = expect_error(c, "route_pair", pair_legs([[15 * MM, 10_250_000]]), "unroutable")
        check("waypoint 0" in err["message"], f"unroutable names the waypoint: {err}")
        c.call("route_pair", dict(PAIR, gap=200_000))  # at the clearance: admitted
        expect_error(c, "route_pair", dict(PAIR, via_gap=199_999), "gap_too_small")
        expect_error(c, "route_pair", dict(PAIR, end={"p": {"pad": "U2.1"}, "n": {"pad": "U4.1"}}), "pair_not_coupled")
        # a free point of DN farther from U1.1 than U1.2 is: the engine couples U1.2
        expect_error(c, "route_pair", dict(PAIR, start={"p": {"pad": "U1.1"}, "n": {"point": [5 * MM, 11_500_000]}}),
                     "pair_not_coupled")
        base = board("pair")["rules"]["classes"]["Default"]
        for differs in ({"track_width": 300_000}, {"via_diameter": 700_000}, {"via_drill": 350_000}):
            wide = board("pair")
            classes = dict(wide["rules"]["classes"], Other=dict(base, **differs))
            wide["rules"] = dict(wide["rules"], classes=classes, net_classes={"DN": "Other"})
            c.call("load_board", {"board": wide})
            expect_error(c, "route_pair", PAIR, "pair_class_mismatch")
        same = board("pair")
        same["rules"] = dict(same["rules"], classes=dict(same["rules"]["classes"], Other=dict(base, clearance=210_000)),
                             net_classes={"DN": "Other"})
        c.call("load_board", {"board": same})
        c.call("route_pair", dict(PAIR, gap=210_000))  # one size, two clearances: the larger one holds
        expect_error(c, "route_pair", dict(PAIR, gap=205_000), "gap_too_small")
        boxed = board("pair")
        boxed["tracks"] = [{"id": f"ring-{k}", "net": "B", "layer": "F.Cu", "start": a, "end": z, "width": 250_000}
                           for k, (a, z) in enumerate([([3 * MM, 8 * MM], [7 * MM, 8 * MM]), ([7 * MM, 8 * MM], [7 * MM, 13 * MM]),
                                                       ([7 * MM, 13 * MM], [3 * MM, 13 * MM]), ([3 * MM, 13 * MM], [3 * MM, 8 * MM])])]
        c.call("load_board", {"board": boxed})
        expect_error(c, "route_pair", PAIR, "unroutable")
        covered = board("pair")
        covered["copper"] = [{"id": "logo-1", "net": "B", "layer": "F.Cu",
                              "polygon": [[4_900_000, 9_900_000], [5_100_000, 9_900_000], [5 * MM, 10_100_000]]}]
        c.call("load_board", {"board": covered})
        expect_error(c, "route_pair", PAIR, "start_not_routable")


def t_tune_length(binary):
    """The single-track tuner: tuned within tolerance, `length` the track length
    of what it proposes, clean; too long proposes nothing; too short says so."""
    b = board("tune")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        res = c.call("tune", TUNE)
        check(res["status"] == "tuned" and abs(res["length"] - TUNE["target"]) <= TUNE["tolerance"], f"tune: {res}")
        check(res["proposal"]["remove"] == ["trk-a1"], f"remove: {res['proposal']['remove']}")
        nb = clean(b, res["proposal"], "tune length")
        got = net_length(nb, "A")
        check(abs(got - res["length"]) <= track_count(nb, "A"), f"length {res['length']}, tracks measure {got:.0f}")
        check(connected(res["proposal"], "A", [5 * MM, 10 * MM], [25 * MM, 10 * MM]), "tuned track broken")
        far = c.call("tune", dict(TUNE, target=60 * MM))
        check(far["status"] == "too_short" and far["length"] < 60 * MM and far["proposal"]["add"], f"too_short: {far}")
        clean(b, far["proposal"], "tune too short")
        short = c.call("tune", dict(TUNE, target=10 * MM))
        check(short["status"] == "too_long" and short["length"] == 20 * MM, f"too_long: {short}")
        check(short["proposal"] == {"remove": [], "add": []}, f"too_long proposes: {short['proposal']}")
        # two collinear tracks: the engine would give them back merged into one
        split = board("tune")
        split["tracks"] = [dict(split["tracks"][0], end=[15 * MM, 10 * MM]),
                           dict(split["tracks"][0], id="trk-a2", start=[15 * MM, 10 * MM])]
        c.call("load_board", {"board": split})
        short = c.call("tune", dict(TUNE, target=10 * MM, to=[14 * MM, 10 * MM]))
        check(short["status"] == "too_long" and short["proposal"] == {"remove": [], "add": []},
              f"too_long on two tracks proposes: {short['proposal']}")
        c.call("load_board", {"board": b})
        line = c.request("tune", TUNE, raw_response=True)
        check(json.loads(line)["result"] == res, "the same tune twice differs")
        c.call("apply", {"proposal": res["proposal"], "base_revision": 0})


def t_tune_keeps_untouched(binary):
    """The engine rewrites the whole line it tunes; the tracks of it that come back
    unchanged are neither removed nor added."""
    b = board("tune")
    b["pads"][1] = dict(b["pads"][1], position=[25 * MM, 12 * MM])
    b["tracks"] = [{"id": "trk-a1", "net": "A", "layer": "F.Cu", "start": [5 * MM, 10 * MM], "end": [20 * MM, 10 * MM],
                    "width": 250_000},
                   {"id": "trk-a2", "net": "A", "layer": "F.Cu", "start": [20 * MM, 10 * MM], "end": [22 * MM, 12 * MM],
                    "width": 250_000},
                   {"id": "trk-a3", "net": "A", "layer": "F.Cu", "start": [22 * MM, 12 * MM], "end": [25 * MM, 12 * MM],
                    "width": 250_000}]
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        res = c.call("tune", dict(TUNE, target=24 * MM, **{"from": [7 * MM, 10 * MM], "to": [18 * MM, 10 * MM]}))
        pr = res["proposal"]
        check(res["status"] == "tuned" and pr["remove"] == ["trk-a1"], f"remove: {pr['remove']} ({res['status']})")
        kept = {(tuple(t["start"]), tuple(t["end"])) for t in b["tracks"][1:]}
        check(not any((tuple(a["start"]), tuple(a["end"])) in kept or (tuple(a["end"]), tuple(a["start"])) in kept
                      for a in pr["add"]), "an unchanged track came back as an add")
        nb = clean(b, pr, "tune keeps untouched")
        check(abs(net_length(nb, "A") - res["length"]) <= track_count(nb, "A"), "length of the line")


def diagonal_legs(proposal, net):
    return [abs(a["end"][0] - a["start"][0]) for a in proposal["add"]
            if a["net"] == net and a["kind"] == "track" and a["end"][0] != a["start"][0] and a["end"][1] != a["start"][1]]


def t_tune_shape(binary):
    """corners, single_sided, amplitude and spacing reach the engine."""
    b = board("tune")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        sq = c.call("tune", dict(TUNE, corners="square"))["proposal"]
        ch = c.call("tune", dict(TUNE, corners="chamfer"))["proposal"]
        legs_sq, legs_ch = diagonal_legs(sq, "A"), diagonal_legs(ch, "A")
        check(legs_sq and min(legs_sq) >= 125_000 - 2, f"square corners under half the width: {min(legs_sq or [0])}")
        check(max(legs_sq) < min(legs_ch), f"square corners not sharper than chamfer: {max(legs_sq)} vs {min(legs_ch)}")
        one = c.call("tune", dict(TUNE, single_sided=True, amplitude=[300_000, 800_000], spacing=700_000))
        check(one["status"] == "tuned", f"single-sided: {one['status']}")
        ys = [p[1] for a in one["proposal"]["add"] for p in (a["start"], a["end"])]
        check(min(ys) >= 10 * MM or max(ys) <= 10 * MM, "single_sided meanders on both sides")
        check(max(abs(y - 10 * MM) for y in ys) <= 800_000 + 125_000, "amplitude over its max")
        for pr in (sq, ch, one["proposal"]):
            clean(b, pr, "tune shapes")


def t_tune_pair(binary):
    """Pair length (the longer member's) and skew (the tuned member less its partner)."""
    b = board("tune_pair")
    with RouterClient(binary) as c:
        c.call("load_board", {"board": b})
        res = c.call("tune", TUNE_PAIR)
        check(res["status"] == "tuned" and abs(res["length"] - 24 * MM) <= 100_000, f"pair_length: {res}")
        check(res["proposal"]["remove"] == ["trk-n1", "trk-p1"], f"remove: {res['proposal']['remove']}")
        nb = clean(b, res["proposal"], "tune pair_length")
        longer = max(net_length(nb, "DP"), net_length(nb, "DN"))
        check(abs(longer - res["length"]) <= track_count(nb, "DP") + track_count(nb, "DN"),
              f"pair length {res['length']}, the longer member measures {longer:.0f}")
        for track, net in (("trk-p1", "DP"), ("trk-n1", "DN")):
            y = 10 * MM if net == "DP" else 10_500_000
            sk = c.call("tune", dict(TUNE_PAIR, aim="skew", target=1 * MM, track=track,
                                     **{"from": [8 * MM, y], "to": [22 * MM, y]}))
            check(sk["status"] == "tuned" and abs(sk["skew"] - 1 * MM) <= 100_000, f"skew {net}: {sk}")
            check(sk["proposal"]["remove"] == [track], f"skew touched the partner: {sk['proposal']['remove']}")
            nb = clean(b, sk["proposal"], f"tune skew {net}")
            other = "DN" if net == "DP" else "DP"
            got = net_length(nb, net)
            check(abs(got - sk["length"]) <= track_count(nb, net), f"skew length {sk['length']} vs {got:.0f}")
            check(abs(got - net_length(nb, other) - sk["skew"]) <= track_count(nb, net) + 1, f"skew {sk['skew']}")


def t_tune_errors(binary):
    """Every refusal of tune, each with its code."""
    with RouterClient(binary) as c:
        expect_error(c, "tune", TUNE, "no_board")
        c.call("load_board", {"board": board("tune")})
        bad = [{k: v for k, v in TUNE.items() if k != "aim"}, dict(TUNE, aim="width"), dict(TUNE, target=0),
               dict(TUNE, target=1e7), dict(TUNE, tolerance=-1), dict(TUNE, p="A", n="B"),
               dict(TUNE_PAIR, net="A"), dict(TUNE_PAIR, n="DP"), dict(TUNE, to=TUNE["from"]),
               dict(TUNE, to=[22 * MM, 11 * MM]), dict(TUNE, mode="shove"), dict(TUNE, corners="round"),
               dict(TUNE, amplitude=[800_000, 300_000]), dict(TUNE, amplitude=[0, 300_000]), dict(TUNE, spacing=0),
               dict(TUNE, aim="skew", p="A", n="B", net=None, target=2_000_000_000),
               {k: v for k, v in TUNE.items() if k != "from"}]
        for req in bad:
            expect_error(c, "tune", req, "invalid_params")
        expect_error(c, "tune", dict(TUNE, track="nope"), "unknown_item")
        expect_error(c, "tune", dict(TUNE, net="ZZ"), "unknown_net")
        expect_error(c, "tune", dict(TUNE, net="B"), "net_mismatch")
        expect_error(c, "tune", dict(TUNE_PAIR, p="A", n="B", track="trk-a1"), "pair_not_coupled")
        locked = board("tune")
        locked["tracks"][0]["locked"] = True
        c.call("load_board", {"board": locked})
        expect_error(c, "tune", TUNE, "locked")
        # `from` on the track's copper off its centreline snaps onto it, there
        # where `to` is: the engine has no span to tune
        c.call("load_board", {"board": board("tune")})
        expect_error(c, "tune", dict(TUNE, **{"from": [8 * MM, 10_100_000], "to": [8 * MM, 10 * MM]}), "tune_refused")
        c.call("load_board", {"board": board("tune_pair")})
        expect_error(c, "tune", dict(TUNE_PAIR, p="DP", n="A"), "unknown_net")
        expect_error(c, "tune", dict(TUNE_PAIR, track="trk-n1", n="DX", p="DP"), "unknown_net")
        held = board("tune_pair")
        held["tracks"][1] = dict(held["tracks"][1], locked=True)  # trk-n1, the partner
        c.call("load_board", {"board": held})
        err = expect_error(c, "tune", TUNE_PAIR, "locked")
        check("trk-n1" in err["message"], f"locked names the partner: {err}")
        c.call("tune", dict(TUNE_PAIR, aim="skew", target=1 * MM))  # skew leaves the partner: admitted
        uncoupled = board("tune_pair")
        uncoupled["tracks"][1] = dict(uncoupled["tracks"][1], end=[25 * MM, 15 * MM])  # not parallel
        c.call("load_board", {"board": uncoupled})
        expect_error(c, "tune", TUNE_PAIR, "pair_not_coupled")
        pm = board("tune_pair")
        pm["tracks"].append({"id": "trk-x", "net": "A", "layer": "F.Cu", "start": [5 * MM, 18 * MM],
                             "end": [25 * MM, 18 * MM], "width": 250_000})
        c.call("load_board", {"board": pm})
        expect_error(c, "tune", dict(TUNE_PAIR, track="trk-x", **{"from": [8 * MM, 18 * MM], "to": [22 * MM, 18 * MM]}),
                     "pair_mismatch")


SYNTHETIC = [t_checker_self_test, t_protocol_errors, t_base_revision_required, t_walkaround, t_free_point_start, t_power_class_clearance,
             t_shove, t_shove_no_wall_clock, t_via, t_via_drill_vs_diameter, t_same_net_hole_to_hole, t_hole_clearance, t_edge_clearance,
             t_arc_obstacle, t_arc_obstacle_shove, t_keepout_interior, t_unroutable, t_zero_length_track, t_net_only_in_net_classes, t_drag_no_net,
             t_one_layer_via, t_via_at_trace_end, t_integers_only, t_utf8, t_duplicate_keys_linear, t_parse_is_time_limited,
             t_max_line_is_bounded, t_argv_strict, t_internal_crash, t_engine_identity, t_error_detail, t_converter, t_fixture_zone_filter, t_spdx_headers, t_no_local_paths, t_patch_notices, t_timeout,
             t_ping_methods, t_route_pair, t_route_pair_shove, t_route_pair_vias, t_route_pair_dangling,
             t_route_pair_waypoints, t_route_pair_off_axis, t_route_pair_from_stubs, t_route_pair_detour, t_route_pair_errors, t_tune_length, t_tune_keeps_untouched, t_tune_shape, t_tune_pair, t_tune_errors]


# ---- determinism ---------------------------------------------------------------

def determinism_requests():
    """(board, steps): each step is (method, params); "apply_last" applies the
    previous step's proposal. Every response line is compared byte for byte:
    within a process, across processes, and against tests/golden/responses.jsonl
    (committed; the same file gates the native and the linux/amd64 builds)."""
    A = {"net": "A", "layer": "F.Cu", "start": {"pad": "U1.1"}, "end": {"pad": "U2.1"}}
    via = {"waypoints": [{"point": [15 * MM, 10 * MM], "via": True, "layer": "B.Cu"}], "via_allowed": True}
    return [
        ("walkaround", [("route", A),
                        ("route", dict(A, start={"point": [2 * MM, 10 * MM]})),
                        ("route", dict(A, mode="mark_obstacles")),
                        ("route", dict(A, step=1 * MM))]),
        ("shove", [("route", dict(A, mode="shove")),
                   ("apply_last", {}),
                   ("route", dict(A, mode="shove"))]),
        ("shove_drag", [("drag", {"item": "trk-c2", "from": [15 * MM, 10_300_000], "to": [15 * MM, 9 * MM]}),
                        ("drag", {"item": "trk-c2", "to": [15 * MM, 12 * MM], "mode": "walkaround"})]),
        ("via", [("route", dict(A, **via))]),
        ("unroutable", [("route", A), ("route", dict(A, mode="shove"))]),
        ("power", [("route", {"net": "VCC", "layer": "F.Cu", "start": {"pad": "P1.1"}, "end": {"pad": "P2.1"}}),
                   ("route", {"net": "VCC", "layer": "F.Cu", "start": {"point": [3 * MM, 10 * MM]},
                              "end": {"pad": "P2.1"}, "mode": "shove"})]),
        ("holes", [("route", {"net": "E", "layer": "F.Cu", "start": {"pad": "U3.1"}, "end": {"pad": "U4.1"}}),
                   ("route", {"net": "A", "layer": "F.Cu", "start": {"pad": "J1.1"}, "end": {"pad": "U2.1"},
                              "via_allowed": True,
                              "waypoints": [{"point": [12 * MM, 6 * MM], "via": True, "layer": "B.Cu"}]})]),
        ("arc", [("route", A)]),
        ("arc_shove", [("route", dict(A, mode="shove"))]),
        # issue #10: ping.methods, route_pair, tune
        ("pair", [("ping", {}),
                  ("route_pair", PAIR),
                  ("route_pair", dict({k: v for k, v in PAIR.items() if k != "end"},
                                      waypoints=[{"point": [10 * MM, 6 * MM]}]))]),
        ("pair_shove", [("route_pair", dict(PAIR, mode="shove")),
                        ("apply_last", {})]),
        ("pair_vias", [("route_pair", pair_via_request()),
                       ("route_pair", pair_via_request(300_000))]),
        ("tune", [("tune", TUNE),
                  ("tune", dict(TUNE, corners="square", single_sided=True, amplitude=[300_000, 800_000],
                                spacing=700_000)),
                  ("tune", dict(TUNE, target=10 * MM))]),
        ("tune_pair", [("tune", TUNE_PAIR),
                       ("tune", dict(TUNE_PAIR, aim="skew", target=1 * MM))]),
        # PR #11 r1 M-1: a waypoint, then more routing, in walkaround and in shove
        ("pair_legs", [("route_pair", pair_legs([[8 * MM, 10_250_000]]))]),
        ("pair_legs_shove", [("route_pair", pair_legs([[8 * MM, 10_250_000], [14 * MM, 10_250_000]], mode="shove"))]),
        # PR #11 r2 R2-M1: off the pads' line, one and two waypoints, walkaround and shove: unroutable
        ("pair_off_axis", [("route_pair", pair_legs(OFF_AXIS[0][1])), ("route_pair", pair_legs(OFF_AXIS[0][1], mode="shove")),
                           ("route_pair", pair_legs(OFF_AXIS[1][1])), ("route_pair", pair_legs(OFF_AXIS[1][1], mode="shove"))]),
        # PR #11 r3 R3-M2: a detour off the line and back, a waypoint at each bend (patches/0005): routed
        ("pair_detour", [("route_pair", pair_legs(DETOURS[0])), ("route_pair", pair_legs(DETOURS[1], mode="shove"))]),
    ]


# cases whose board is made from a committed one in code
MADE = {"pair_detour": open_pair}
BOARD_OF = {"shove_drag": "shove", "arc_shove": "arc", "pair_vias": "pair_shove", "pair_legs": "pair", "pair_off_axis": "pair",
            "pair_legs_shove": "pair_shove"}


def run_steps(c, name, steps):
    """Load the case's board and run its steps; the raw response lines."""
    c.call("load_board", {"board": MADE[name]() if name in MADE else board(BOARD_OF.get(name, name))})
    lines, last = [], None
    for method, params in steps:
        c.next_id = 100  # the same id everywhere, so whole lines compare
        if method == "apply_last":
            if not json.loads(last)["ok"]:
                lines.append(last := "(apply_last skipped: the previous step failed)")
                continue
            res = json.loads(last)["result"]
            line = c.request("apply", {"proposal": res["proposal"], "base_revision": res["revision"]},
                             raw_response=True)
        else:
            line = c.request(method, params, raw_response=True)
        lines.append(line)
        last = line
    return lines


def all_lines(binary):
    with RouterClient(binary) as c:
        return [line for name, steps in determinism_requests() for line in run_steps(c, name, steps)]


def t_determinism(binary):
    """Same requests twice in one process, and in two processes: byte-identical."""
    with RouterClient(binary) as c:
        twice = []
        for name, steps in determinism_requests():
            twice.append((run_steps(c, name, steps), run_steps(c, name, steps)))
    for k, (a, b) in enumerate(twice):
        check(a == b, f"same process, case {k}: responses differ")
    check(all_lines(binary) == all_lines(binary), "two processes: responses differ")


def t_golden(binary, update=False, record_to=None):
    """The response lines equal the committed golden lines, byte for byte (review
    T-5). The same file is compared by the native ctest and inside the
    linux/amd64 docker build, so a cross-platform difference is a red test."""
    lines = all_lines(binary)
    if record_to:  # tools/update_kicad.sh: the new lines beside the committed ones, never over them
        pathlib.Path(record_to).write_text("".join(x + "\n" for x in lines), encoding="utf-8")
        return
    if update:
        GOLDEN.parent.mkdir(exist_ok=True)
        GOLDEN.write_text("".join(x + "\n" for x in lines), encoding="utf-8")
        print(f"wrote {GOLDEN} ({len(lines)} lines)")
        return
    want = GOLDEN.read_text(encoding="utf-8").splitlines()
    check(len(lines) == len(want), f"{len(lines)} response lines, golden has {len(want)}")
    for k, (got, exp) in enumerate(zip(lines, want)):
        check(got == exp, f"golden line {k + 1} differs:\n  got  {got[:400]}\n  want {exp[:400]}")


DETERMINISM = [t_determinism, t_golden]


def run(tests, binary):
    failed = 0
    for t in tests:
        try:
            t(binary)
            print(f"PASS {t.__name__}")
        except (Failure, RouterError, AssertionError, KeyError, ValueError, RuntimeError) as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    return failed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--suite", default="all", choices=["synthetic", "determinism", "fixtures", "all"])
    ap.add_argument("--only", help="run only the named tests (comma-separated)")
    ap.add_argument("--update-golden", action="store_true", help="rewrite tests/golden/responses.jsonl")
    ap.add_argument("--record-golden", metavar="PATH", help="write the golden lines this binary gives to PATH")
    a = ap.parse_args()
    if a.record_golden:
        t_golden(a.binary, record_to=a.record_golden)
        return 0
    if a.update_golden:
        t_golden(a.binary, update=True)
        return 0
    if a.only:
        names = set(a.only.split(","))
        tests = [t for t in SYNTHETIC + DETERMINISM if t.__name__ in names]
        if len(tests) != len(names):
            print(f"unknown tests: {names - {t.__name__ for t in tests}}")
            return 2
        failed = run(tests, a.binary)
        print("FAILED" if failed else "OK")
        return 1 if failed else 0
    failed = 0
    if a.suite in ("synthetic", "all"):
        failed += run(SYNTHETIC, a.binary)
    if a.suite in ("determinism", "all"):
        failed += run(DETERMINISM, a.binary)
    if a.suite in ("fixtures", "all"):
        import fixture_tests
        rc = fixture_tests.main(a.binary)
        if rc == SKIP and a.suite == "fixtures":
            return SKIP
        failed += 1 if rc not in (0, SKIP) else 0
    print("FAILED" if failed else "OK")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
