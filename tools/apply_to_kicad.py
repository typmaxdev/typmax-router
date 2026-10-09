#!/usr/bin/env python3.11
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
"""Write a typmax-router proposal into a COPY of a .kicad_pcb (stdlib only).

  apply_to_kicad.py in.kicad_pcb proposal.json out.kicad_pcb

The proposal is the `proposal` object of a route/drag response ({remove, add}),
or a list of them applied in order. Removed ids are matched against the uuid
of top-level segments, arcs and vias and cut out verbatim; added tracks and
vias are appended as new items with fresh uuids. Everything else in the file is
copied byte for byte. The input file is never written."""
import argparse
import json
import pathlib
import sys
import uuid

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from kicad_sexpr import child, head, parse, quote, value  # noqa: E402

COPPER_ITEMS = ("segment", "arc", "via")


def mm(v):
    s = f"{v / 1_000_000:.6f}".rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def compose(proposals):
    """Fold a sequence of proposals into one net change against the first board."""
    removed, added = [], {}
    for p in proposals:
        for rid in p["remove"]:
            if rid in added:
                del added[rid]
            else:
                removed.append(rid)
        for it in p["add"]:
            added[it["id"]] = it
    return {"remove": sorted(set(removed)), "add": list(added.values())}


def _net_ref(net, codes_by_name):
    if net is None:
        return "(net 0)" if codes_by_name else ""
    if codes_by_name:  # KiCad <= 9: nets by code
        return f"(net {codes_by_name[net]})"
    return f"(net {quote(net)})"  # KiCad 10: nets by name


def render(item, codes_by_name, tag):
    u = str(uuid.uuid5(uuid.NAMESPACE_URL, f"typmax-router:{tag}:{item['id']}"))
    net = _net_ref(item.get("net"), codes_by_name)
    if item["kind"] == "track":
        sx, sy = item["start"]
        ex, ey = item["end"]
        kind = "arc" if "mid" in item else "segment"
        mid = f" (mid {mm(item['mid'][0])} {mm(item['mid'][1])})" if "mid" in item else ""
        return (f"\t({kind} (start {mm(sx)} {mm(sy)}){mid} (end {mm(ex)} {mm(ey)}) (width {mm(item['width'])}) "
                f"(layer {quote(item['layer'])}) {net} (uuid {quote(u)}))\n")
    x, y = item["position"]
    layers = " ".join(quote(l) for l in item["layers"])
    return (f"\t(via (at {mm(x)} {mm(y)}) (size {mm(item['diameter'])}) (drill {mm(item['drill'])}) "
            f"(layers {layers}) {net} (uuid {quote(u)}))\n")


def rewrite(text, remove_ids=(), add_items=(), drop=None, tag="p"):
    """Return new file text: top-level copper items whose uuid is in remove_ids
    (or for which drop(node) is true) cut out, add_items appended."""
    root, spans = parse(text)
    remove_ids = set(remove_ids)
    found = set()
    cuts = []
    kids = [c for c in root[1:] if isinstance(c, list)]
    assert len(kids) == len(spans), "span bookkeeping"
    for node, span in zip(kids, spans):
        u = value(node, "uuid") or value(node, "tstamp")
        if head(node) in COPPER_ITEMS and u is not None and str(u) in remove_ids:
            found.add(str(u))
            cuts.append(span)
        elif drop is not None and drop(node):
            cuts.append(span)
    missing = remove_ids - found
    if missing:
        raise SystemExit(f"apply_to_kicad: remove names items not in the board: {sorted(missing)[:5]}")
    out, pos = [], 0
    for s, e in sorted(cuts):
        out.append(text[pos:s])
        pos = e
    out.append(text[pos:])
    body = "".join(out).rstrip()
    assert body.endswith(")"), "a .kicad_pcb ends with ')'"
    codes = {str(n[2]): str(n[1]) for n in root[1:] if isinstance(n, list) and head(n) == "net" and len(n) >= 3}
    new = "".join(render(it, codes, tag) for it in add_items)
    return body[:-1].rstrip() + "\n" + new + ")\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pcb")
    ap.add_argument("proposal")
    ap.add_argument("out")
    a = ap.parse_args()
    src, dst = pathlib.Path(a.pcb), pathlib.Path(a.out)
    if src.resolve() == dst.resolve():
        raise SystemExit("apply_to_kicad: refusing to overwrite the input board")
    p = json.loads(pathlib.Path(a.proposal).read_text())
    if isinstance(p, list):
        p = compose(p)
    elif "proposal" in p:
        p = p["proposal"]
    dst.write_text(rewrite(src.read_text(), p["remove"], p["add"]))
    print(f"wrote {dst}: removed {len(p['remove'])}, added {len(p['add'])}")


if __name__ == "__main__":
    main()
