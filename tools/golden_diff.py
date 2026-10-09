#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
"""Compare two golden-line files (tests/golden/responses.jsonl) line by line.

    tools/golden_diff.py OLD.jsonl NEW.jsonl [--markdown]

Each line is a response to the same request (tests/run_tests.py,
determinism_requests). The engine identity is set aside; what remains is
sorted into a first-pass class a reviewer then confirms:

  identical   the same answer
  better      an error before, a proposal now
  worse       a proposal before, an error now
  changed     a different proposal (or error): read it — the summary gives
              the copper added and removed, the vias and the track length
              on both sides

A summary is a starting point, not a verdict: a "changed" route may be
better, equivalent or worse, and only a reading of the geometry says which.
"""
import json
import math
import sys


def _case_names():
    """The request behind each golden line, from the test suite's own list."""
    import pathlib

    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tests"))
    try:
        import run_tests  # noqa: E402
    except Exception:  # the names are a convenience; the diff works without them
        return []
    names = []
    for name, steps in run_tests.determinism_requests():
        for method, params in steps:
            mode = params.get("mode") if isinstance(params, dict) else None
            names.append(f"{name}:{method}" + (f"({mode})" if mode else ""))
    return names


def summary(line):
    """One line's answer, without the engine identity, and a short description."""
    try:
        r = json.loads(line)
    except ValueError:
        return line, line
    if not r.get("ok"):
        e = r.get("error", {})
        return ("error", e.get("code")), f"error {e.get('code')}"
    res = r.get("result", {})
    if "proposal" not in res:
        return ("ok", {k: v for k, v in res.items() if k != "engine"}), f"ok (revision {res.get('revision')})"
    p = res["proposal"]
    tracks = [a for a in p["add"] if a["kind"] == "track"]
    vias = [a for a in p["add"] if a["kind"] == "via"]
    length = sum(math.dist(t["start"], t["end"]) for t in tracks) / 1e6
    desc = f"remove {len(p['remove'])}, add {len(tracks)} tracks + {len(vias)} vias, {length:.3f} mm"
    return ("proposal", p), desc


def compare(old_lines, new_lines):
    names = _case_names()
    rows = []
    for k in range(max(len(old_lines), len(new_lines))):
        o = old_lines[k] if k < len(old_lines) else None
        n = new_lines[k] if k < len(new_lines) else None
        name = names[k] if k < len(names) else f"line {k + 1}"
        if o is None or n is None:
            rows.append((k + 1, name, "changed", "(missing)" if o is None else summary(o)[1],
                         "(missing)" if n is None else summary(n)[1]))
            continue
        (ok_kind, ov), od = summary(o)
        (nk_kind, nv), nd = summary(n)
        if (ok_kind, ov) == (nk_kind, nv):
            cls = "identical"
        elif ok_kind == "error" and nk_kind != "error":
            cls = "better"
        elif ok_kind != "error" and nk_kind == "error":
            cls = "worse"
        else:
            cls = "changed"
        rows.append((k + 1, name, cls, od, nd))
    return rows


def render(rows, markdown=False):
    counts = {}
    for r in rows:
        counts[r[2]] = counts.get(r[2], 0) + 1
    head = ", ".join(f"{counts.get(c, 0)} {c}" for c in ("identical", "better", "worse", "changed"))
    out = []
    if markdown:
        out.append(f"{len(rows)} lines: {head}.\n")
        out.append("| # | request | class | before | after |")
        out.append("|---|---|---|---|---|")
        for k, name, cls, od, nd in rows:
            if cls != "identical":
                out.append(f"| {k} | `{name}` | **{cls}** | {od} | {nd} |")
    else:
        out.append(f"{len(rows)} lines: {head}")
        for k, name, cls, od, nd in rows:
            out.append(f"{k:3d} {cls:9s} {name}\n      before: {od}\n      after:  {nd}")
    return "\n".join(out)


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    if len(args) != 2:
        print(__doc__)
        return 2
    old = open(args[0], encoding="utf-8").read().splitlines()
    new = open(args[1], encoding="utf-8").read().splitlines()
    print(render(compare(old, new), markdown="--markdown" in argv))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
