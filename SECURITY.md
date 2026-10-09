<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 The typmax-router authors -->
# Security policy

## Reporting a vulnerability

Please report vulnerabilities **privately**, through GitHub's private vulnerability
reporting: the **Security** tab of this repository, then **Report a vulnerability**
([direct link](https://github.com/typmaxdev/typmax-router/security/advisories/new)).
Do not open a public issue, pull request or discussion for a suspected vulnerability.

Include what you can of: the commit or the `engine` object of a `ping` answer, the
platform, a request stream that triggers the problem, and what an attacker gains.
You should get an acknowledgement within 7 days. A fix is developed in a private
advisory fork and published with the advisory; you are credited unless you ask not
to be.

## Supported versions

There are no releases yet: only the latest commit on `main` is supported. Once
releases are tagged, the newest release and `main` will be.

## Scope

The service reads JSON Lines from whoever starts it and treats every request as
untrusted input. In scope:

- memory-safety bugs, crashes, hangs or unbounded memory/CPU use reachable from a
  request stream (the JSON reader, the board loader, the router through `route`,
  `drag` and `apply`), including ones that escape the documented time and size limits
  (PROTOCOL.md, "Time limits and crashes");
- a proposal that the protocol's guarantees table says cannot happen, where that has
  a security consequence for a client relying on it;
- the build and CI: the workflows in `.github/workflows/`, the Dockerfile, and the
  update tools in `tools/`.

Out of scope: bugs in KiCad's router that KiCad itself also has (report those to
[KiCad](https://gitlab.com/kicad/code/kicad/-/issues) too; we will carry a patch in
`patches/` where it affects the service), and deployments that expose the service's
stdin to untrusted parties without the limits PROTOCOL.md describes.
