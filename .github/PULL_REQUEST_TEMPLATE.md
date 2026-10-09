<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 The typmax-router authors -->
## Summary

-

## Test plan

- [ ] `ctest --test-dir build --output-on-failure` (synthetic, determinism, watcher; fixtures if you have them)
- [ ] golden lines unchanged, or re-recorded with the classification in a commit body (MAINTAINING.md "Updating", step 4)

## Checklist

- [ ] commits are signed off (`git commit -s`, CONTRIBUTING.md)
- [ ] no edit under `vendor/kicad/`: a change to KiCad's code is a patch in `patches/` with its GPL section 5(a) notice
- [ ] every new file has an SPDX line (or a REUSE.toml entry)
- [ ] PROTOCOL.md updated (and its changelog) if the protocol changed
