# KiCad 10.0.8: patch release, prepared on `update/kicad-10.0.8`

tools/kicad_watch.sh created `update/kicad-10.0.8` and ran `tools/update_kicad.sh 10.0.8` there. Nothing was merged. Review the branch, then follow MAINTAINING.md, "Updating to a new KiCad release".

## Update

`update_kicad.sh` exit **0**: done, gates green.

```
tag 10.0.8 = commit e432beac2668303194ba2040274f9e5efe773fc0 (2026-10-29)
patch 1 applies: 0001-shove-search-arc-obstacles.patch
patch 2 applies: 0002-archull-margin-covers-polyline-error.patch
patch 3 applies: 0003-obstacle-order-by-item-serial.patch
patch 4 applies: 0004-virtual-vias-in-a-stable-order.patch
vendor/kicad: 10.0.7 (69cafb907ebc) -> 10.0.8 (e432beac2668): 8 changed, 0 added, 0 removed
vendor/kicad is now 10.0.8 (e432beac2668303194ba2040274f9e5efe773fc0); review with: git status vendor/ && git diff --stat
gate synthetic: PASS
gate determinism: PASS
gate fixtures: PASS
golden lines: 14 of 18 differ (/home/runner/work/typmax-router/typmax-kicad-cache/watch-worktree-10.0.8/build/kicad-update/golden.diff): classify them, then re-record (MAINTAINING.md)
```

## Gates

| gate | result |
|---|---|
| synthetic | PASS |
| determinism | PASS |
| fixtures | PASS |
| golden | DIFF |

`golden` is SAME, DIFF (expected after a release; classify below) or FAIL.

## Golden lines

18 lines: 18 identical, 0 better, 0 worse, 0 changed.

| # | request | class | before | after |
|---|---|---|---|---|

Classify every non-identical line (better / equivalent / worse) by reading its geometry, then re-record and bump `SERVICE_VERSION` (MAINTAINING.md).

## Fixture boards (before = the base, after = this release; same machine, same run)

| board | | routed | new copper violations | latency median / p90 (ms) | load_board (ms) |
|---|---|---|---|---|---|
| pic_programmer | before | 23/23 | 0 | 1.35 / 10.27 | 3.4 |
| pic_programmer | after | 23/23 | 0 | 1.4 / 10.27 | 3.4 |
| video | before | 25/45 | 0 | 69.5 / 76.99 | 51.7 |
| video | after | 25/45 | 0 | 70.81 / 82.29 | 53.7 |

