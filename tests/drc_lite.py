# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
"""An independent, deliberately simple rule check for the synthetic tests.

It knows nothing about PNS: copper is capsules (tracks; an arc is a chain of
short capsules), discs (vias, round pads) and polygons (other pads). It checks
the rules the board maps (review T-2):

- copper: two items of different nets on a shared layer are at least the larger
  of their classes' clearances apart;
- hole_to_hole: two drilled holes, ANY nets (a drill does not care), at least
  `hole_to_hole` apart edge to edge;
- hole_clearance: copper at least `hole_clearance` from a hole of another net
  (or of no net);
- edge_clearance: copper at least `edge_clearance` from every outline edge.

It is exact for the shapes the synthetic boards use (rotated rects included)."""
import math

TOL = 2  # nm: rounding slack


def _pt_seg(p, a, b):
    ax, ay = a
    bx, by = b
    px, py = p
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def _seg_seg(a, b, c, d):
    def orient(p, q, r):
        return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
    o1, o2, o3, o4 = orient(a, b, c), orient(a, b, d), orient(c, d, a), orient(c, d, b)
    if (o1 > 0) != (o2 > 0) and (o3 > 0) != (o4 > 0) and o1 and o2 and o3 and o4:
        return 0.0
    return min(_pt_seg(a, c, d), _pt_seg(b, c, d), _pt_seg(c, a, b), _pt_seg(d, a, b))


def _inside(p, poly):
    x, y = p
    inside = False
    for i in range(len(poly)):
        (x1, y1), (x2, y2) = poly[i], poly[(i + 1) % len(poly)]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            inside = not inside
    return inside


def _rot(x, y, deg):
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    return x * c + y * s, -x * s + y * c


def arc_points(a, m, b, n=48):
    """The arc a -> m -> b as n + 1 points (exact on the circle)."""
    (ax, ay), (mx, my), (bx, by) = a, m, b
    d = 2 * (ax * (my - by) + mx * (by - ay) + bx * (ay - my))
    if d == 0:
        return [tuple(a), tuple(b)]
    ux = ((ax * ax + ay * ay) * (my - by) + (mx * mx + my * my) * (by - ay) + (bx * bx + by * by) * (ay - my)) / d
    uy = ((ax * ax + ay * ay) * (bx - mx) + (mx * mx + my * my) * (ax - bx) + (bx * bx + by * by) * (mx - ax)) / d
    r = math.hypot(ax - ux, ay - uy)
    t0, tm, t1 = (math.atan2(p[1] - uy, p[0] - ux) for p in (a, m, b))
    sweep = (t1 - t0) % (2 * math.pi)
    if (tm - t0) % (2 * math.pi) > sweep:  # m is not on the ccw way from a to b
        sweep -= 2 * math.pi
    return [(ux + r * math.cos(t0 + sweep * k / n), uy + r * math.sin(t0 + sweep * k / n)) for k in range(n + 1)]


def holes(board):
    """[(id, net, centre, radius)] for every drilled hole: pads with a drill, vias."""
    out = [(p["id"], p.get("net"), tuple(p["position"]), p["drill"] / 2)
           for p in board.get("pads", []) if p.get("drill")]
    out += [(v["id"], v.get("net"), tuple(v["position"]), v["drill"] / 2) for v in board.get("vias", [])]
    return out


def shapes(board):
    """[(id, net, layers:set, kind, data)] where kind is 'capsule' (a, b, r),
    'disc' (c, r) or 'poly' (points)."""
    n = len(board["layers"])
    out = []
    for p in board.get("pads", []):
        layers = set(board["layers"]) if p.get("drill") else set(p["layers"])
        cx, cy = p["position"]
        w, h = p["size"]
        rot = p.get("rotation") or 0
        if p["shape"] == "circle" or (p["shape"] == "oval" and w == h):
            out.append((p["id"], p.get("net"), layers, "disc", ((cx, cy), max(w, h) / 2)))
        elif p["shape"] == "oval":
            half = abs(w - h) / 2
            dx, dy = _rot(half, 0, rot) if w > h else _rot(0, half, rot)
            out.append((p["id"], p.get("net"), layers, "capsule", ((cx - dx, cy - dy), (cx + dx, cy + dy), min(w, h) / 2)))
        else:
            if p["shape"] == "polygon":
                pts = [_rot(x, y, rot) for x, y in p["polygon"]]
            else:
                pts = [_rot(x, y, rot) for x, y in ((-w / 2, -h / 2), (w / 2, -h / 2), (w / 2, h / 2), (-w / 2, h / 2))]
            out.append((p["id"], p.get("net"), layers, "poly", [(cx + x, cy + y) for x, y in pts]))
    for t in board.get("tracks", []):
        if t.get("mid"):
            pts = arc_points(t["start"], t["mid"], t["end"])
            for k in range(len(pts) - 1):
                out.append((t["id"], t.get("net"), {t["layer"]}, "capsule", (pts[k], pts[k + 1], t["width"] / 2)))
            continue
        out.append((t["id"], t.get("net"), {t["layer"]}, "capsule", (tuple(t["start"]), tuple(t["end"]), t["width"] / 2)))
    for v in board.get("vias", []):
        i0, i1 = board["layers"].index(v["layers"][0]), board["layers"].index(v["layers"][1])
        out.append((v["id"], v.get("net"), set(board["layers"][min(i0, i1):max(i0, i1) + 1]), "disc",
                    (tuple(v["position"]), v["diameter"] / 2)))
    return out


def _dist(s1, s2):
    """Edge-to-edge distance (negative/zero = touching or overlapping)."""
    k1, d1 = s1
    k2, d2 = s2
    if k1 == "poly" and k2 == "poly":
        raise ValueError("pad-pad not checked")
    if k1 == "poly":
        k1, d1, k2, d2 = k2, d2, k1, d1
    if k1 == "disc":
        c, r = d1
        if k2 == "disc":
            return math.dist(c, d2[0]) - r - d2[1]
        if k2 == "capsule":
            return _pt_seg(c, d2[0], d2[1]) - r - d2[2]
        if _inside(c, d2):
            return -r
        return min(_pt_seg(c, d2[i], d2[(i + 1) % len(d2)]) for i in range(len(d2))) - r
    # capsule
    a, b, r = d1
    if k2 == "disc":
        return _pt_seg(d2[0], a, b) - r - d2[1]
    if k2 == "capsule":
        return _seg_seg(a, b, d2[0], d2[1]) - r - d2[2]
    if _inside(a, d2) or _inside(b, d2):
        return -r
    return min(_seg_seg(a, b, d2[i], d2[(i + 1) % len(d2)]) for i in range(len(d2))) - r


def clearance(rules, net):
    cls = rules.get("net_classes", {}).get(net, rules["default_class"]) if net else rules["default_class"]
    return rules["classes"][cls]["clearance"]


def _edges(board):
    for poly in board.get("outline", []):
        for k in range(len(poly)):
            yield tuple(poly[k]), tuple(poly[(k + 1) % len(poly)])


def violations(board, only_ids=None):
    """Rule violations (module docstring) with at least one item in only_ids
    (all when None), as (id_a, id_b, distance, needed)."""
    sh = shapes(board)
    rules = board["rules"]
    dflt = rules["classes"][rules["default_class"]]["clearance"]
    hole_clearance = rules.get("hole_clearance", dflt)
    hole_to_hole = rules.get("hole_to_hole", dflt)
    edge_clearance = rules.get("edge_clearance", dflt)
    mine = (lambda i: True) if only_ids is None else (lambda i: i in only_ids)
    bad = []
    hs = holes(board)
    for i in range(len(hs)):
        for j in range(i + 1, len(hs)):
            a, b = hs[i], hs[j]
            if mine(a[0]) or mine(b[0]):
                d = math.dist(a[2], b[2]) - a[3] - b[3]
                if d + TOL < hole_to_hole:
                    bad.append((a[0], b[0], round(d), hole_to_hole))
    for h in hs:
        for s in sh:
            if s[0] == h[0] or not (mine(h[0]) or mine(s[0])):
                continue
            if h[1] and h[1] == s[1]:
                continue  # same net: not a rule this checker claims
            d = _dist(("disc", (h[2], h[3])), (s[3], s[4]))
            if d + TOL < hole_clearance:
                bad.append((s[0], h[0] + "(hole)", round(d), hole_clearance))
    for s in sh:
        if not mine(s[0]):
            continue
        for e in _edges(board):
            d = _dist((s[3], s[4]), ("capsule", (e[0], e[1], 0)))
            if d + TOL < edge_clearance:
                bad.append((s[0], "edge", round(d), edge_clearance))
    for i in range(len(sh)):
        for j in range(i + 1, len(sh)):
            a, b = sh[i], sh[j]
            if only_ids is not None and a[0] not in only_ids and b[0] not in only_ids:
                continue
            if a[1] and a[1] == b[1]:
                continue
            if not (a[2] & b[2]):
                continue
            if a[3] == "poly" and b[3] == "poly":
                continue
            need = max(clearance(rules, a[1]), clearance(rules, b[1]))
            d = _dist((a[3], a[4]), (b[3], b[4]))
            if d + TOL < need:
                bad.append((a[0], b[0], round(d), need))
    return bad


def apply(board, proposal):
    """The proposal applied to a copy of the board (what `apply` does server-side)."""
    import copy
    b = copy.deepcopy(board)
    rm = set(proposal["remove"])
    b["tracks"] = [t for t in b.get("tracks", []) if t["id"] not in rm]
    b["vias"] = [v for v in b.get("vias", []) if v["id"] not in rm]
    for it in proposal["add"]:
        item = {k: v for k, v in it.items() if k != "kind"}
        (b.setdefault("tracks", []) if it["kind"] == "track" else b.setdefault("vias", [])).append(item)
    return b
