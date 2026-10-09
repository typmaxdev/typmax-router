<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 The typmax-router authors -->
# patches/

Our changes to the vendored KiCad files, as an ordered series (`series` lists
them; SOURCE.md, "Patches", says what each one changes and why). At KiCad
10.0.7 there are four, all fixes of upstream router behaviour; everything else
that differs from KiCad's own build is a stand-in header or source under
`shim/` (SOURCE.md lists each one and why). A patch is the last resort, for a
change no stand-in can make.

## Adding a patch

1. Make the change in a scratch copy of `vendor/kicad/` (never in
   `vendor/kicad/` itself) and write it as a unified diff relative to the
   KiCad source root, `a/` and `b/` prefixes (`diff -ruN a b` or
   `git diff --no-index`), named `NNNN-short-name.patch`.
2. Start the file with a header (patch ignores text before the first `---`):

   ```
   SPDX-License-Identifier: GPL-3.0-or-later
   SPDX-FileCopyrightText: <year> <author>
   Subject: <one line: what it changes>
   Upstream-file: pcbnew/router/<file>
   Why: <the behaviour it changes and why the service needs it>
   Upstream: <could it go upstream? an MR link once proposed, or "no: <why>">
   ```
3. In the same diff, add a notice to the top of every file it modifies, inside
   the file's copyright comment (GPL-3.0 §5(a): a modified file carries a
   prominent notice that it was modified, and when):

   ```
    * Modified by the typmax-router authors, <YYYY-MM-DD>: <what, in a line>
    *   (patches/NNNN-short-name.patch in the typmax-router repository).
   ```

   A second patch to an already modified file adds its own notice line.
4. Add its name to `series`, re-run CMake (it re-applies the series to its
   copy in the build directory) and the tests; record it in SOURCE.md.

`tools/update_kicad.sh` checks every patch against each new release before it
touches `vendor/kicad/`, and stops on the first one that no longer applies.
