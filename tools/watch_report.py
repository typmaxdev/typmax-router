#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
"""Write the markdown report tools/kicad_watch.sh leaves for a KiCad release.

    tools/watch_report.py --tag T --kind patch|major --update-log LOG --update-rc N
                          [--branch B] [--gates DIR] [--before DIR] [--golden-old FILE]

--gates is update_kicad.sh's build/kicad-update/ (status files, golden.jsonl,
fixtures/); --before is the base measured just before the update (golden.jsonl,
fixtures/). Everything missing is reported as missing, never guessed.
"""
import argparse
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import golden_diff  # noqa: E402

UPDATE_RC = {0: "done, gates green", 1: "usage or environment error", 2: "fetch or stage failed",
             3: "a patch does not apply", 4: "build failed", 5: "a gate failed"}


def read(p):
    try:
        return pathlib.Path(p).read_text(encoding="utf-8")
    except (OSError, TypeError):
        return None


def fixtures(d):
    out = {}
    if not d:
        return out
    for f in sorted(pathlib.Path(d).glob("*-result.json")):
        try:
            out[f.name[: -len("-result.json")]] = json.loads(f.read_text())
        except ValueError:
            pass
    return out


def fixture_rows(before, after):
    rows = ["| board | | routed | new copper violations | latency median / p90 (ms) | load_board (ms) |",
            "|---|---|---|---|---|---|"]
    for name in sorted(set(before) | set(after)):
        for label, r in (("before", before.get(name)), ("after", after.get(name))):
            if not r:
                rows.append(f"| {name} | {label} | (not run) | | | |")
                continue
            n = r["routes_ok"] + r["routes_failed"]
            lat = r.get("latency_ms", {})
            rows.append(f"| {name} | {label} | {r['routes_ok']}/{n} | {sum(r.get('new_copper_violations', {}).values())} "
                        f"| {lat.get('median')} / {lat.get('p90')} | {r.get('load_board_ms')} |")
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--kind", choices=["patch", "major"], required=True)
    ap.add_argument("--branch")
    ap.add_argument("--update-log", required=True)
    ap.add_argument("--update-rc", type=int, required=True)
    ap.add_argument("--gates")
    ap.add_argument("--before")
    ap.add_argument("--golden-old")
    a = ap.parse_args()
    log = read(a.update_log) or ""
    lines = [l[len("update_kicad: "):] if l.startswith("update_kicad: ") else l for l in log.splitlines()]
    out = []
    if a.kind == "major":
        out += [f"# KiCad {a.tag}: a new major/minor release — a port is needed", "",
                "tools/kicad_watch.sh does not update across a major or minor release on its own. "
                "Below is a dry run of `tools/update_kicad.sh --dry-run --try-build "
                f"{a.tag}`: which vendored files changed, whether every patch still applies, "
                "and whether the tree builds with this repository's shims.", ""]
    else:
        out += [f"# KiCad {a.tag}: patch release, prepared on `{a.branch}`", "",
                f"tools/kicad_watch.sh created `{a.branch}` and ran `tools/update_kicad.sh {a.tag}` there. "
                "Nothing was merged. Review the branch, then follow MAINTAINING.md, "
                "\"Updating to a new KiCad release\".", ""]
    out += ["## Update", "", f"`update_kicad.sh` exit **{a.update_rc}**: {UPDATE_RC.get(a.update_rc, 'unexpected')}.", ""]
    keep = [l for l in lines if re.search(r"^(tag |patch |vendor/kicad|.*has no |.*does not apply|try-build|gate |golden|build failed|dry run|patches/series)", l)]
    out += ["```"] + (keep or ["(no output)"]) + ["```", ""]
    if a.kind == "patch":
        g = pathlib.Path(a.gates) if a.gates else None
        out += ["## Gates", "", "| gate | result |", "|---|---|"]
        for gate in ("synthetic", "determinism", "fixtures", "golden"):
            st = read(g / f"{gate}.status") if g else None
            out.append(f"| {gate} | {st.strip() if st else 'not run'} |")
        out += ["", "`golden` is SAME, DIFF (expected after a release; classify below) or FAIL.", ""]
        out += ["## Golden lines", ""]
        old, new = read(a.golden_old), read(g / "golden.jsonl") if g else None
        if old and new:
            out += [golden_diff.render(golden_diff.compare(old.splitlines(), new.splitlines()), markdown=True), "",
                    "Classify every non-identical line (better / equivalent / worse) by reading its geometry, "
                    "then re-record and bump `SERVICE_VERSION` (MAINTAINING.md).", ""]
        else:
            out += ["(not recorded: the build or the recording failed)", ""]
        out += ["## Fixture boards (before = the base, after = this release; same machine, same run)", ""]
        before = fixtures(pathlib.Path(a.before) / "fixtures" if a.before else None)
        after = fixtures(g / "fixtures" if g else None)
        if before or after:
            out += fixture_rows(before, after) + [""]
        else:
            out += ["(not run: TYPMAX_FIXTURES_DIR was unset)", ""]
    print("\n".join(out).rstrip() + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
