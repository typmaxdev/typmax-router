<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 The typmax-router authors -->
# reports/

Written by `tools/kicad_watch.sh` (README.md, "Release watch"; on the GitHub runner,
where the workflow uploads them): `watch.log`
(one line per run), `kicad-<tag>.md` (what a new KiCad release changed: the
patches, the gates, the golden-line diff, the fixture boards before and
after), `kicad-<tag>.golden.jsonl` (the new golden lines, for the macOS gate), the lock
while a run is going. None of it is committed on the main
branch except through an update branch, which commits its own report.
