#!/usr/bin/env python3.11
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
"""Convert a .kicad_pcb into typmax-router's neutral board JSON (PROTOCOL.md).

  kicad_to_board.py board.kicad_pcb [-o board.json] [--pro board.kicad_pro]

For tests and fixtures; stdlib only. What it reads: copper layers, Edge.Cuts
(board and footprint graphics, chained into polygons), pads (absolute position
and orientation), segments, arcs, vias, keepout zones, and the net classes of
the .kicad_pro beside the board (or --pro); without one, KiCad's defaults.
What it does not read: copper zone fills (not obstacles in the protocol),
custom-pad primitives beyond their convex hull, blind/buried via spans beyond
their two end layers."""
import argparse
import json
import math
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from kicad_sexpr import Str, child, children, head, parse, value  # noqa: E402

NM = 1_000_000

# KiCad's own defaults for a board with no project file (KiCad 7+ netclass and
# board design settings), in nm.
KICAD_DEFAULTS = {
    "clearance": 200_000, "track_width": 200_000, "via_diameter": 600_000, "via_drill": 300_000,
    "edge_clearance": 500_000, "hole_clearance": 250_000, "hole_to_hole": 250_000,
}


def nm(s):
    return round(float(s) * NM)


def rot(x, y, deg):
    """KiCad's RotatePoint: y down, positive angle turns counter-clockwise on screen."""
    if deg % 360 == 0:
        return x, y
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    if deg % 90 == 0:
        c, s = round(c), round(s)
    return x * c + y * s, -x * s + y * c


def xy(node):
    return nm(node[1]), nm(node[2])


def at(node):
    a = child(node, "at")
    if a is None:
        return 0, 0, 0.0
    return nm(a[1]), nm(a[2]), float(a[3]) if len(a) > 3 else 0.0


def uuid_of(node):
    return value(node, "uuid") or value(node, "tstamp")


def copper_layers(root):
    names = []
    for entry in child(root, "layers")[1:]:
        if isinstance(entry, list) and len(entry) >= 3 and str(entry[1]).endswith(".Cu"):
            names.append(str(entry[1]))
    inner = sorted((n for n in names if re.fullmatch(r"In\d+\.Cu", n)), key=lambda n: int(n[2:-3]))
    out = (["F.Cu"] if "F.Cu" in names else []) + inner + (["B.Cu"] if "B.Cu" in names else [])
    return out


def net_names(root):
    """KiCad <= 9 declares (net N "name"); KiCad 10 names nets inline."""
    return {str(n[1]): str(n[2]) for n in children(root, "net") if len(n) >= 3}


def net_of(node, codes):
    n = child(node, "net")
    if n is None or len(n) < 2:
        return None
    if len(n) >= 3:
        return str(n[2]) or None
    tok = n[1]
    if isinstance(tok, Str):
        return str(tok) or None
    return codes.get(str(tok)) or None


def expand_layers(spec, copper):
    out = []
    for l in spec:
        l = str(l)
        if l in ("*.Cu", "F&B.Cu"):
            out.extend(copper if l == "*.Cu" else [x for x in ("F.Cu", "B.Cu") if x in copper])
        elif l in copper:
            out.append(l)
    return sorted(set(out), key=copper.index)


# ---- outline ---------------------------------------------------------------------

def arc_points(s, m, e, step_deg=10):
    (x1, y1), (x2, y2), (x3, y3) = s, m, e
    d = 2 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2))
    if d == 0:
        return [s, e]
    ux = ((x1 * x1 + y1 * y1) * (y2 - y3) + (x2 * x2 + y2 * y2) * (y3 - y1) + (x3 * x3 + y3 * y3) * (y1 - y2)) / d
    uy = ((x1 * x1 + y1 * y1) * (x3 - x2) + (x2 * x2 + y2 * y2) * (x1 - x3) + (x3 * x3 + y3 * y3) * (x2 - x1)) / d
    r = math.hypot(x1 - ux, y1 - uy)
    a1, a2, a3 = (math.atan2(p[1] - uy, p[0] - ux) for p in (s, m, e))

    def ccw(a, b):
        return (b - a) % (2 * math.pi)
    sweep = ccw(a1, a3)
    if ccw(a1, a2) > sweep:  # the mid point is on the other way round
        sweep = sweep - 2 * math.pi
    n = max(2, int(abs(math.degrees(sweep)) / step_deg) + 1)
    pts = [(round(ux + r * math.cos(a1 + sweep * k / n)), round(uy + r * math.sin(a1 + sweep * k / n))) for k in range(n + 1)]
    pts[0], pts[-1] = s, e
    return pts


def edge_pieces(node, tf):
    """Polylines (lists of points) of one Edge.Cuts graphic, transformed by tf."""
    h = head(node)
    kind = h.split("_", 1)[1] if "_" in h else h
    if kind == "line":
        return [[tf(xy(child(node, "start"))), tf(xy(child(node, "end")))]]
    if kind == "arc":
        return [[tf(p) for p in arc_points(xy(child(node, "start")), xy(child(node, "mid")), xy(child(node, "end")))]]
    if kind == "rect":
        (x1, y1), (x2, y2) = xy(child(node, "start")), xy(child(node, "end"))
        return [[tf(p) for p in ((x1, y1), (x2, y1), (x2, y2), (x1, y2), (x1, y1))]]
    if kind == "circle":
        (cx, cy), (ex, ey) = xy(child(node, "center")), xy(child(node, "end"))
        r = math.hypot(ex - cx, ey - cy)
        pts = [(round(cx + r * math.cos(2 * math.pi * k / 36)), round(cy + r * math.sin(2 * math.pi * k / 36))) for k in range(36)]
        return [[tf(p) for p in pts + [pts[0]]]]
    if kind == "poly":
        pts = [tf(xy(p)) for p in children(child(node, "pts"), "xy")]
        return [pts + [pts[0]]] if pts else []
    return []


def chain(pieces, tol=1000):
    """Join polylines end to end into closed polygons (tolerance in nm)."""
    def close(a, b):
        return abs(a[0] - b[0]) <= tol and abs(a[1] - b[1]) <= tol
    pieces = [p for p in pieces if len(p) >= 2]
    polys = []
    while pieces:
        cur = pieces.pop(0)
        grew = True
        while grew and not close(cur[0], cur[-1]):
            grew = False
            for i, p in enumerate(pieces):
                if close(cur[-1], p[0]):
                    cur += p[1:]
                elif close(cur[-1], p[-1]):
                    cur += list(reversed(p))[1:]
                elif close(cur[0], p[-1]):
                    cur = p[:-1] + cur
                elif close(cur[0], p[0]):
                    cur = list(reversed(p))[:-1] + cur
                else:
                    continue
                pieces.pop(i)
                grew = True
                break
        if close(cur[0], cur[-1]) and len(cur) > 1:
            cur = cur[:-1]
        if len(cur) >= 3:
            polys.append([list(p) for p in cur])
        else:
            print(f"kicad_to_board: an Edge.Cuts piece did not close ({len(cur)} points), dropped", file=sys.stderr)
    return polys


# ---- pads ------------------------------------------------------------------------

def convex_hull(pts):
    pts = sorted(set(pts))
    if len(pts) <= 2:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lower, upper = [], []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def pad_record(fp_ref, fp_xy, fp_angle, pad, copper, codes):
    name = str(pad[1])
    ptype = str(pad[2])
    shape = str(pad[3])
    px, py, pang = at(pad)
    dx, dy = rot(px, py, fp_angle)
    pos = [round(fp_xy[0] + dx), round(fp_xy[1] + dy)]
    size = child(pad, "size")
    w, h = nm(size[1]), nm(size[2])
    drill_node = child(pad, "drill")
    drill = None
    if drill_node is not None:
        nums = [nm(t) for t in drill_node[1:] if not isinstance(t, list) and str(t) != "oval"]
        drill = max(nums) if nums else None
    layers = expand_layers(child(pad, "layers")[1:], copper)
    if ptype in ("thru_hole", "np_thru_hole"):
        layers = list(copper)
    rec = {"footprint": fp_ref, "pad": name, "net": None if ptype == "np_thru_hole" else net_of(pad, codes),
           "layers": layers, "position": pos, "rotation": pang, "size": [w, h], "drill": drill}
    if shape in ("circle", "rect", "oval", "roundrect"):
        rec["shape"] = shape
    elif shape == "trapezoid":
        delta = child(pad, "rect_delta")
        ddx, ddy = (nm(delta[1]), nm(delta[2])) if delta is not None else (0, 0)
        hw, hh = w / 2, h / 2
        rec["shape"] = "polygon"
        rec["polygon"] = [[round(-hw - ddy / 2), round(-hh + ddx / 2)], [round(hw + ddy / 2), round(-hh - ddx / 2)],
                          [round(hw - ddy / 2), round(hh + ddx / 2)], [round(-hw + ddy / 2), round(hh - ddx / 2)]]
    elif shape == "custom":
        # anchor + primitives -> the convex hull (a superset of the copper)
        pts = [(-w // 2, -h // 2), (w // 2, -h // 2), (w // 2, h // 2), (-w // 2, h // 2)]
        prims = child(pad, "primitives")
        for g in (prims[1:] if prims is not None else []):
            if not isinstance(g, list):
                continue
            gw = nm(value(g, "width", "0"))
            src = []
            if head(g) == "gr_poly":
                src = [xy(p) for p in children(child(g, "pts"), "xy")]
            elif head(g) in ("gr_line", "gr_rect"):
                src = [xy(child(g, "start")), xy(child(g, "end"))]
            elif head(g) == "gr_circle":
                (cx, cy), (ex, ey) = xy(child(g, "center")), xy(child(g, "end"))
                r = math.hypot(ex - cx, ey - cy)
                src = [(cx - r, cy - r), (cx + r, cy + r), (cx - r, cy + r), (cx + r, cy - r)]
            for (x, y) in src:
                for ox, oy in ((gw / 2, gw / 2), (-gw / 2, -gw / 2), (gw / 2, -gw / 2), (-gw / 2, gw / 2)):
                    pts.append((round(x + ox), round(y + oy)))
        # primitives are in the pad's own frame; the board JSON rotates by `rotation`
        rec["shape"] = "polygon"
        rec["polygon"] = [list(p) for p in convex_hull(pts)]
    else:
        rec["shape"] = "rect"
    if not rec["layers"]:
        return None  # no copper (e.g. a paste-only pad)
    return rec


def footprint_ref(fp):
    for p in children(fp, "property"):
        if len(p) > 2 and str(p[1]) == "Reference":
            return str(p[2])
    for t in children(fp, "fp_text"):
        if len(t) > 2 and str(t[1]) == "reference":
            return str(t[2])
    return "?"


def disc_poly(c, r, n=16):
    """A regular n-gon circumscribing the circle (c, r), integer nm."""
    k = r / math.cos(math.pi / n)
    return [[round(c[0] + k * math.cos(2 * math.pi * i / n)), round(c[1] + k * math.sin(2 * math.pi * i / n))]
            for i in range(n)]


# ---- rules -----------------------------------------------------------------------

# Board-level minimums in a .kicad_pro (design_settings.rules) that floor a net
# class's value: KiCad's DRC judges against the larger of the two (review K-1).
BOARD_MINIMUMS = {"clearance": "min_clearance", "track_width": "min_track_width",
                  "via_diameter": "min_via_diameter", "via_drill": "min_through_hole_diameter"}

def rules_from_project(pro_path):
    r = dict(KICAD_DEFAULTS)
    classes = {"Default": {k: r[k] for k in ("clearance", "track_width", "via_diameter", "via_drill")}}
    net_classes = {}
    if pro_path and pro_path.exists():
        pro = json.loads(pro_path.read_text())
        board_rules = pro.get("board", {}).get("design_settings", {}).get("rules", {})
        if "min_copper_edge_clearance" in board_rules:
            r["edge_clearance"] = nm(board_rules["min_copper_edge_clearance"])
        if "min_hole_clearance" in board_rules:
            r["hole_clearance"] = nm(board_rules["min_hole_clearance"])
        if "min_hole_to_hole" in board_rules:
            r["hole_to_hole"] = nm(board_rules["min_hole_to_hole"])
        ns = pro.get("net_settings", {})
        for c in ns.get("classes", []):
            name = c.get("name", "Default")
            base = classes.get("Default")
            classes[name] = {
                "clearance": nm(c.get("clearance", base["clearance"] / NM)),
                "track_width": nm(c.get("track_width", base["track_width"] / NM)),
                "via_diameter": nm(c.get("via_diameter", base["via_diameter"] / NM)),
                "via_drill": nm(c.get("via_drill", base["via_drill"] / NM)),
            }
            for net in c.get("nets", []) or []:  # KiCad 6
                net_classes[net] = name
        for net, cls in (ns.get("netclass_assignments") or {}).items():  # KiCad 7+
            if isinstance(cls, list):
                cls = cls[0] if cls else "Default"
            if cls in classes:
                net_classes[net] = cls
        for key, rule in BOARD_MINIMUMS.items():
            if rule in board_rules:
                floor = nm(board_rules[rule])
                for c in classes.values():
                    c[key] = max(c[key], floor)
        for c in classes.values():  # a floored drill must stay inside its via
            if c["via_drill"] >= c["via_diameter"]:
                c["via_diameter"] = c["via_drill"] + 1
        dru = pro_path.with_suffix(".kicad_dru")
        if dru.exists():
            print(f"kicad_to_board: warning: {dru.name} holds custom DRC rules this converter does not "
                  "map; the router will not honour them", file=sys.stderr)
    return {"default_class": "Default", "classes": classes, "net_classes": net_classes,
            "edge_clearance": r["edge_clearance"], "hole_clearance": r["hole_clearance"],
            "hole_to_hole": r["hole_to_hole"]}, net_classes


def apply_patterns(pro_path, rules, nets):
    """KiCad 7+ netclass_patterns: wildcard net name -> class."""
    if not pro_path or not pro_path.exists():
        return
    import fnmatch
    pats = json.loads(pro_path.read_text()).get("net_settings", {}).get("netclass_patterns") or []
    for net in sorted(nets):
        if net in rules["net_classes"]:
            continue
        for p in pats:
            if p.get("netclass") in rules["classes"] and fnmatch.fnmatchcase(net, p.get("pattern", "")):
                rules["net_classes"][net] = p["netclass"]
                break


# ---- fixed copper (text, graphics) ----------------------------------------------

def _rect_poly(cx, cy, w, h, ang, tf):
    pts = []
    for dx, dy in ((-w / 2, -h / 2), (w / 2, -h / 2), (w / 2, h / 2), (-w / 2, h / 2)):
        rx, ry = rot(dx, dy, ang)
        pts.append(list(tf((round(cx + rx), round(cy + ry)))))
    return pts


def _seg_poly(a, b, w, tf):
    """The rectangle around a stroke, extended by w/2 at both ends (contains the capsule)."""
    (ax, ay), (bx, by) = a, b
    L = math.hypot(bx - ax, by - ay) or 1.0
    ux, uy = (bx - ax) / L * w / 2, (by - ay) / L * w / 2
    return [list(tf((round(x), round(y)))) for x, y in (
        (ax - ux - uy, ay - uy + ux), (bx + ux - uy, by + uy + ux), (bx + ux + uy, by + uy - ux), (ax - ux + uy, ay - uy - ux))]


def text_poly(node, text, tf, parent_angle=0.0):
    """A conservative box for stroke text: every glyph as wide as the font's size."""
    x, y, ang = at(node)
    eff = child(node, "effects")
    font = child(eff, "font") if eff is not None else None
    size = child(font, "size") if font is not None else None
    hgt, wid = (nm(size[1]), nm(size[2])) if size is not None else (NM, NM)
    th = nm(value(font, "thickness", "0.15")) if font is not None else NM // 7
    lines = str(text).split("\n")
    w = max(len(l) for l in lines) * wid + th
    h = len(lines) * hgt * 1.6 + th
    just = [str(t) for t in (child(eff, "justify") or [])[1:]] if eff is not None else []
    cx = x + (w / 2 if "left" in just else -w / 2 if "right" in just else 0)
    return _rect_poly(cx, y, w, h, ang, tf)


def graphic_polys(g, tf):
    """Polygons for one copper graphic, transformed by tf (footprint or identity)."""
    h = head(g)
    kind = h.split("_", 1)[1]
    width = nm(value(g, "width", "0") or "0")
    stroke = child(g, "stroke")
    if stroke is not None:
        width = nm(value(stroke, "width", "0"))
    if kind == "text":
        return [text_poly(g, g[1] if len(g) > 1 else "", tf)]
    if kind == "line":
        return [_seg_poly(xy(child(g, "start")), xy(child(g, "end")), max(width, 1), tf)]
    if kind in ("arc", "rect", "circle", "poly"):
        out = []
        for piece in edge_pieces(g, lambda p: p):
            filled = value(g, "fill") in ("solid", "yes")
            if kind in ("poly", "rect") or filled:
                out.append([list(tf(p)) for p in piece[:-1]])
            else:
                for i in range(len(piece) - 1):
                    out.append(_seg_poly(piece[i], piece[i + 1], max(width, 1), tf))
        return [p for p in out if len(p) >= 3]
    return []


# ---- main ------------------------------------------------------------------------

def convert(pcb_path, pro_path=None):
    pcb_path = pathlib.Path(pcb_path)
    if pro_path is None:
        cand = pcb_path.with_suffix(".kicad_pro")
        pro_path = cand if cand.exists() else None
    root, _ = parse(pcb_path.read_text())
    copper = copper_layers(root)
    codes = net_names(root)
    edges = []
    pads = []
    seen_ids = set()
    for g in root[1:]:
        if isinstance(g, list) and head(g) in ("gr_line", "gr_arc", "gr_rect", "gr_circle", "gr_poly") \
                and value(g, "layer") == "Edge.Cuts":
            edges.extend(edge_pieces(g, lambda p: p))
    for fp in children(root, "footprint") + children(root, "module"):
        fx, fy, fa = at(fp)
        ref = footprint_ref(fp)

        def tf(p, fx=fx, fy=fy, fa=fa):
            dx, dy = rot(p[0], p[1], fa)
            return (round(fx + dx), round(fy + dy))
        for g in fp[1:]:
            if isinstance(g, list) and head(g) in ("fp_line", "fp_arc", "fp_rect", "fp_circle", "fp_poly") \
                    and value(g, "layer") == "Edge.Cuts":
                edges.extend(edge_pieces(g, tf))
        for pad in children(fp, "pad"):
            rec = pad_record(ref, (fx, fy), fa, pad, copper, codes)
            if rec is None:
                continue
            base = f"{ref}.{rec['pad'] or '~'}"
            pid, k = base, 1
            while pid in seen_ids:
                k += 1
                pid = f"{base}#{k}"
            seen_ids.add(pid)
            pads.append({"id": pid, **rec})
    copper_items = []
    cu_kinds = ("text", "line", "arc", "rect", "circle", "poly")
    for g in root[1:]:
        if isinstance(g, list) and head(g) in tuple("gr_" + k for k in cu_kinds) \
                and str(value(g, "layer", "")) in copper:
            for k, poly in enumerate(graphic_polys(g, lambda p: p)):
                copper_items.append({"id": f"{uuid_of(g)}#{k}", "net": None, "layer": str(value(g, "layer")),
                                     "polygon": poly})
    for fp in children(root, "footprint") + children(root, "module"):
        fx, fy, fa = at(fp)

        def tf(p, fx=fx, fy=fy, fa=fa):
            dx, dy = rot(p[0], p[1], fa)
            return (round(fx + dx), round(fy + dy))
        for g in fp[1:]:
            if not isinstance(g, list) or str(value(g, "layer", "")) not in copper:
                continue
            if head(g) in tuple("fp_" + k for k in cu_kinds):
                polys = graphic_polys(g, tf)
            elif head(g) == "property" and child(g, "hide") is None and "hide" not in [str(x) for x in g]:
                polys = [text_poly(g, g[2], tf)]
            else:
                continue
            for k, poly in enumerate(polys):
                copper_items.append({"id": f"{uuid_of(g) or id(g)}#{k}", "net": None,
                                     "layer": str(value(g, "layer")), "polygon": poly})
    tracks, vias, keepouts = [], [], []
    for s in children(root, "segment") + children(root, "arc"):
        layer = str(value(s, "layer"))
        if layer not in copper:
            continue
        t = {"id": str(uuid_of(s)), "net": net_of(s, codes), "layer": layer,
             "start": list(xy(child(s, "start"))), "end": list(xy(child(s, "end"))),
             "width": nm(value(s, "width"))}
        if head(s) == "arc":
            t["mid"] = list(xy(child(s, "mid")))
        if t["start"] == t["end"]:
            # A zero-length segment is a dot of copper; the service refuses it as a
            # track (the engine would drop it), so it becomes a fixed copper disc:
            # a 16-gon circumscribing the dot (conservative).
            copper_items.append({"id": t["id"], "net": t["net"], "layer": layer,
                                 "polygon": disc_poly(t["start"], t["width"] / 2)})
            continue
        if child(s, "locked") is not None or "locked" in [str(x) for x in s[1:] if not isinstance(x, list)]:
            t["locked"] = True
        tracks.append(t)
    for v in children(root, "via"):
        ls = [str(x) for x in child(v, "layers")[1:]]
        ls = [l for l in ls if l in copper] or [copper[0], copper[-1]]
        ls = sorted(ls, key=copper.index)
        vias.append({"id": str(uuid_of(v)), "net": net_of(v, codes), "position": list(xy(child(v, "at"))),
                     "diameter": nm(value(v, "size")), "drill": nm(value(v, "drill")),
                     "layers": [ls[0], ls[-1]]})
    for z in children(root, "zone"):
        ko = child(z, "keepout")
        if ko is None or value(ko, "tracks") != "not_allowed":
            continue
        zl = child(z, "layers") or child(z, "layer")
        layers = expand_layers(zl[1:], copper)
        poly = child(z, "polygon")
        if not layers or poly is None:
            continue
        pts = [list(xy(p)) for p in children(child(poly, "pts"), "xy")]
        if len(pts) >= 3:
            keepouts.append({"id": str(uuid_of(z)), "layers": layers, "polygon": pts})
    rules, _ = rules_from_project(pro_path)
    nets = {p["net"] for p in pads if p["net"]} | {t["net"] for t in tracks if t["net"]}
    apply_patterns(pro_path, rules, nets)
    return {"layers": copper, "outline": chain(edges), "rules": rules, "pads": pads,
            "tracks": tracks, "vias": vias, "keepouts": keepouts, "copper": copper_items}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pcb")
    ap.add_argument("-o", "--out")
    ap.add_argument("--pro", help="the .kicad_pro to read net classes from (default: beside the board)")
    a = ap.parse_args()
    board = convert(a.pcb, pathlib.Path(a.pro) if a.pro else None)
    text = json.dumps(board, separators=(",", ":"))
    if a.out:
        pathlib.Path(a.out).write_text(text + "\n")
    else:
        print(text)


if __name__ == "__main__":
    main()
