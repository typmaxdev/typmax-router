#!/usr/bin/env python3.11
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
"""Regenerate the synthetic boards in tests/boards/ (our own, small, and free of
any third-party content). Run after changing a board; the JSON is committed."""
import json
import pathlib

MM = 1_000_000
OUT = pathlib.Path(__file__).parent / "boards"

RULES = {
    "default_class": "Default",
    "classes": {
        "Default": {"clearance": 200_000, "track_width": 250_000,
                    "via_diameter": 600_000, "via_drill": 300_000},
        "Power": {"clearance": 300_000, "track_width": 500_000,
                  "via_diameter": 800_000, "via_drill": 400_000},
    },
    "net_classes": {"VCC": "Power"},
    "edge_clearance": 300_000,
    "hole_clearance": 250_000,
    "hole_to_hole": 250_000,
}


def mm(x, y):
    return [round(x * MM), round(y * MM)]


def outline(w, h):
    return [[mm(0, 0), mm(w, 0), mm(w, h), mm(0, h)]]


def smd(pid, ref, name, net, x, y, w=1.0, h=1.0, layer="F.Cu", shape="rect", rot=0):
    return {"id": pid, "footprint": ref, "pad": name, "net": net, "layers": [layer],
            "shape": shape, "position": mm(x, y), "rotation": rot,
            "size": [round(w * MM), round(h * MM)], "drill": None}


def tht(pid, ref, name, net, x, y, d=1.6, drill=0.8):
    return {"id": pid, "footprint": ref, "pad": name, "net": net, "layers": ["F.Cu", "B.Cu"],
            "shape": "circle", "position": mm(x, y), "rotation": 0,
            "size": [round(d * MM), round(d * MM)], "drill": round(drill * MM)}


def track(tid, net, layer, a, b, w=0.25):
    return {"id": tid, "net": net, "layer": layer, "start": mm(*a), "end": mm(*b), "width": round(w * MM)}


def npth(pid, x, y, drill):
    """A non-plated mounting hole: no net, copper the size of the drill."""
    return {"id": pid, "footprint": pid.split(".")[0], "pad": "", "net": None, "layers": ["F.Cu", "B.Cu"],
            "shape": "circle", "position": mm(x, y), "rotation": 0,
            "size": [round(drill * MM), round(drill * MM)], "drill": round(drill * MM)}


def arc(tid, net, layer, a, m, b, w=0.25):
    t = track(tid, net, layer, a, b, w)
    t["mid"] = mm(*m)
    return t


def board(pads, tracks=(), vias=(), keepouts=(), w=30, h=20):
    return {"layers": ["F.Cu", "B.Cu"], "outline": outline(w, h), "rules": RULES,
            "pads": list(pads), "tracks": list(tracks), "vias": list(vias), "keepouts": list(keepouts)}


BOARDS = {
    # A big pad of another net sits on the straight line between the two A pads.
    "walkaround": board([
        smd("U1.1", "U1", "1", "A", 5, 10),
        smd("U2.1", "U2", "1", "A", 25, 10),
        smd("U3.1", "U3", "1", "B", 15, 10, 3, 3),
    ]),
    # A net-C track runs 0.3 mm beside the A pads' centre line (too close for a
    # 0.25 mm track at 0.2 mm clearance); shove must push it, walkaround avoids it.
    "shove": board([
        smd("U1.1", "U1", "1", "A", 5, 10),
        smd("U2.1", "U2", "1", "A", 25, 10),
        tht("J1.1", "J1", "1", "C", 10, 15),
        tht("J2.1", "J2", "1", "C", 20, 15),
    ], tracks=[
        track("trk-c1", "C", "F.Cu", (10, 15), (10, 10.3)),
        track("trk-c2", "C", "F.Cu", (10, 10.3), (20, 10.3)),
        track("trk-c3", "C", "F.Cu", (20, 10.3), (20, 15)),
    ]),
    # Top-side pad to bottom-side pad: needs a via.
    "via": board([
        smd("U1.1", "U1", "1", "A", 5, 10),
        smd("U2.1", "U2", "1", "A", 25, 10, layer="B.Cu"),
        smd("U3.1", "U3", "1", "VCC", 15, 4, 2, 2, rot=45),
    ]),
    # Pad A1 boxed in by a closed ring of net-B track: no route exists.
    "unroutable": board([
        smd("U1.1", "U1", "1", "A", 8, 10),
        smd("U2.1", "U2", "1", "A", 25, 10),
    ], tracks=[
        track("ring-1", "B", "F.Cu", (5, 7), (11, 7)),
        track("ring-2", "B", "F.Cu", (11, 7), (11, 13)),
        track("ring-3", "B", "F.Cu", (11, 13), (5, 13)),
        track("ring-4", "B", "F.Cu", (5, 13), (5, 7)),
    ], keepouts=[
        {"id": "ko-1", "layers": ["F.Cu", "B.Cu"], "polygon": [mm(20, 0.5), mm(22, 0.5), mm(22, 2), mm(20, 2)]},
    ]),
    # A Power-class net (clearance 0.3 mm) beside Default-class copper: the net-D
    # track is 0.25 mm from where a straight 0.5 mm VCC track would run, legal
    # for Default (0.2 mm) and not for Power, so the route must keep 0.3 mm.
    "power": board([
        smd("P1.1", "P1", "1", "VCC", 5, 10),
        smd("P2.1", "P2", "1", "VCC", 25, 10),
    ], tracks=[
        track("trk-d1", "D", "F.Cu", (8, 10.625), (22, 10.625)),
    ]),
    # Holes: a THT pad of net A (same-net hole_to_hole), and a non-plated
    # mounting hole on the straight line between the two net-E pads (hole_clearance).
    "holes": board([
        tht("J1.1", "J1", "1", "A", 10, 6),
        smd("U2.1", "U2", "1", "A", 20, 6, layer="B.Cu"),
        smd("U3.1", "U3", "1", "E", 5, 14),
        smd("U4.1", "U4", "1", "E", 25, 14),
        npth("H1.1", 15, 14, 3.0),
    ]),
    # An arc of net B across the straight line between the two A pads.
    "arc": board([
        smd("U1.1", "U1", "1", "A", 5, 10),
        smd("U2.1", "U2", "1", "A", 25, 10),
    ], tracks=[
        arc("arc-b1", "B", "F.Cu", (12, 13), (15, 8), (18, 13)),
    ]),
}


def main():
    OUT.mkdir(exist_ok=True)
    for name, b in BOARDS.items():
        (OUT / f"{name}.json").write_text(json.dumps(b, indent=1, sort_keys=False) + "\n")
        print("wrote", OUT / f"{name}.json")


if __name__ == "__main__":
    main()
