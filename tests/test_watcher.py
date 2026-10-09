#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
"""The release watcher's logic, offline: tools/check_kicad_release.sh against
mocked tag lists, and tools/kicad_watch.sh in a scratch git repository with a
fake update command (no network, no build).

    python3 tests/test_watcher.py      (also `ctest -R watcher`)
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOLS = ["check_kicad_release.sh", "kicad_watch.sh", "watch_report.py", "golden_diff.py"]
FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)
        print(f"FAIL {msg}")


def tags_file(d, names):
    p = pathlib.Path(d) / "tags.json"
    p.write_text(json.dumps([{"name": n} for n in names]))
    return str(p)


def manifest(d, tag="10.0.7"):
    p = pathlib.Path(d) / "kicad.manifest"
    p.write_text(f"# test\ntag {tag}\ncommit {'a' * 40}\n")
    return str(p)


def run_check(d, names=None, tag="10.0.7", url=None):
    env = dict(os.environ, TYPMAX_KICAD_MANIFEST=manifest(d, tag))
    env.pop("TYPMAX_KICAD_TAGS_FILE", None)
    if names is not None:
        env["TYPMAX_KICAD_TAGS_FILE"] = tags_file(d, names)
    if url:
        env["TYPMAX_KICAD_TAGS_URL"] = url
    r = subprocess.run([str(ROOT / "tools" / "check_kicad_release.sh")], env=env, capture_output=True, text=True,
                       timeout=60)
    return r.returncode, r.stdout.strip()


def t_check():
    with tempfile.TemporaryDirectory() as d:
        cases = [
            ("current", ["10.0.7", "10.0.7-rc2", "10.0.6", "9.0.9"], (0, "current")),
            ("patch", ["10.0.8", "10.0.8-rc1", "10.0.7"], (10, "patch-release 10.0.8")),
            ("newest patch", ["10.0.9", "10.0.8", "10.0.7"], (10, "patch-release 10.0.9")),
            ("major", ["11.0.0", "11.0.0-rc1", "10.0.7"], (11, "major-release 11.0.0")),
            ("minor is major", ["10.1.0", "10.0.7"], (11, "major-release 10.1.0")),
            ("patch before major", ["11.0.0", "10.0.8", "10.0.7"], (10, "patch-release 10.0.8")),
            ("rc only", ["10.0.8-rc1", "11.0.0-rc2", "10.0.7"], (0, "current")),
            ("dev series", ["10.99.0", "10.0.7"], (0, "current")),
            ("hotfix tag", ["10.0.7.1", "10.0.7"], (0, "current")),
            ("older branch", ["9.0.10", "10.0.7"], (0, "current")),
            ("non-version tags", ["review", "pre-kiway", "10.0.7"], (0, "current")),
        ]
        for name, names, want in cases:
            got = run_check(d, names)
            check(got == want, f"check {name}: {got} != {want}")
        # a list that lacks the vendored tag is not judged (review r3 W-3): never "current"
        for name, names in (("empty list", []), ("another series only", ["9.0.9"]),
                            ("newer only, vendored tag missing", ["10.0.8"])):
            got = run_check(d, names)
            check(got[0] == 6 and got[1].startswith("error:") and "10.0.7" in got[1], f"check {name}: {got}")
        got = run_check(d, url="http://127.0.0.1:9/no-such-server")
        check(got[0] == 3 and got[1].startswith("error:"), f"check network failure: {got}")
        bad = pathlib.Path(d) / "bad.json"
        bad.write_text("<html>rate limited</html>")
        env = dict(os.environ, TYPMAX_KICAD_MANIFEST=manifest(d), TYPMAX_KICAD_TAGS_FILE=str(bad))
        r = subprocess.run([str(ROOT / "tools" / "check_kicad_release.sh")], env=env, capture_output=True, text=True)
        check(r.returncode == 3, f"check malformed list: {r.returncode} {r.stdout}")
        env = dict(os.environ, TYPMAX_KICAD_MANIFEST=str(pathlib.Path(d) / "missing"),
                   TYPMAX_KICAD_TAGS_FILE=tags_file(d, ["10.0.7"]))
        r = subprocess.run([str(ROOT / "tools" / "check_kicad_release.sh")], env=env, capture_output=True, text=True)
        check(r.returncode == 2, f"check without a manifest: {r.returncode} {r.stdout}")


FAKE_UPDATE = """#!/usr/bin/env bash
# a stand-in for tools/update_kicad.sh: vendors nothing, says what it did
set -e
[ "$1" = --dry-run ] && { echo "update_kicad: patch 1 applies: 0001-x.patch"; echo "update_kicad: dry run: the repository was not touched"; exit 0; }
sed -i.bak "s/^tag .*/tag $1/" vendor/kicad.manifest && rm vendor/kicad.manifest.bak
mkdir -p build/kicad-update
for g in synthetic determinism; do echo PASS > build/kicad-update/$g.status; done
echo SKIP > build/kicad-update/fixtures.status; echo SAME > build/kicad-update/golden.status
cp tests/golden/responses.jsonl build/kicad-update/golden.jsonl
echo "update_kicad: vendor/kicad: 10.0.7 -> $1: 3 changed, 0 added, 0 removed"
"""


def scratch_repo(d):
    repo = pathlib.Path(d) / "repo"
    (repo / "tools").mkdir(parents=True)
    for t in TOOLS:
        shutil.copy2(ROOT / "tools" / t, repo / "tools" / t)
    (repo / "vendor").mkdir()
    (repo / "vendor" / "kicad.manifest").write_text("tag 10.0.7\ncommit " + "a" * 40 + "\n")
    (repo / "tests" / "golden").mkdir(parents=True)
    (repo / "tests" / "golden" / "responses.jsonl").write_text('{"ok":false,"error":{"code":"unroutable"}}\n')
    (repo / ".gitignore").write_text("/build/\nreports/\n")
    fake = repo / "fake_update.sh"
    fake.write_text(FAKE_UPDATE)
    fake.chmod(0o755)
    git = lambda *a: subprocess.run(["git", "-C", str(repo), *a], check=True, capture_output=True, text=True)
    git("init", "-q", "-b", "main")
    git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "add", "-A")
    git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q", "-m", "init")
    return repo, git


def run_watch(repo, d, names=None, url=None, args=()):
    env = dict(os.environ, TYPMAX_WATCH_UPDATE=str(repo / "fake_update.sh"),
               TYPMAX_KICAD_CACHE=str(pathlib.Path(d) / "cache"), GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
    env.pop("TYPMAX_KICAD_MANIFEST", None)
    env.pop("TYPMAX_KICAD_TAGS_FILE", None)
    if names is not None:
        env["TYPMAX_KICAD_TAGS_FILE"] = tags_file(d, names)
    if url:
        env["TYPMAX_KICAD_TAGS_URL"] = url
    r = subprocess.run([str(repo / "tools" / "kicad_watch.sh"), *args], env=env, capture_output=True, text=True,
                       timeout=120)
    return r.returncode, (repo / "reports" / "watch.log").read_text().splitlines() if (repo / "reports" / "watch.log").exists() else []


def t_watch():
    with tempfile.TemporaryDirectory() as d:
        repo, git = scratch_repo(d)
        head = git("rev-parse", "main").stdout

        rc, log = run_watch(repo, d, ["10.0.7"])
        check(rc == 0 and log and " current:" in log[-1], f"watch current: {rc} {log[-1:]}")

        rc, log = run_watch(repo, d, url="http://127.0.0.1:9/x")
        check(rc == 3 and "check failed: error:" in log[-1], f"watch network failure: {rc} {log[-1:]}")

        rc, log = run_watch(repo, d, [])
        check(rc == 6 and "check failed: error:" in log[-1], f"watch on an empty tag list: {rc} {log[-1:]}")

        rc, _ = run_watch(repo, d, ["10.0.8", "10.0.7"], args=["--dry-run"])
        check(rc == 0 and not git("branch", "--list", "update/kicad-10.0.8").stdout.strip(),
              "watch --dry-run makes no branch")

        rc, log = run_watch(repo, d, ["10.0.8", "10.0.7"])
        check(rc == 0 and "patch-release 10.0.8: branch update/kicad-10.0.8 ready" in log[-1], f"watch patch: {rc} {log[-1:]}")
        check(git("rev-parse", "main").stdout == head, "watch patch leaves main where it was")
        branch_files = git("ls-tree", "-r", "--name-only", "update/kicad-10.0.8").stdout.split()
        check("reports/kicad-10.0.8.md" in branch_files, "the report is committed on the branch")
        check("tag 10.0.8" in git("show", "update/kicad-10.0.8:vendor/kicad.manifest").stdout, "the branch has the update")
        rp = repo / "reports" / "kicad-10.0.8.md"
        report = rp.read_text() if rp.exists() else "(no report)"
        check("patch release, prepared on `update/kicad-10.0.8`" in report and "| synthetic | PASS |" in report,
              f"the report says what ran:\n{report}")
        check(not git("worktree", "list", "--porcelain").stdout.count("watch-worktree"), "the scratch worktree is gone")
        check((repo / "reports" / "kicad-10.0.8.golden.jsonl").exists(), "the new golden lines are kept for the macOS gate")

        rc, log = run_watch(repo, d, ["10.0.8", "10.0.7"])
        check(rc == 0 and "already exists (refs/heads/update/kicad-10.0.8); nothing done" in log[-1], f"watch patch again is a no-op: {rc} {log[-1:]}")

        # a branch only on the remote (a fresh CI checkout: refs/remotes/origin/..., no local
        # branch), e.g. one a person edited or whose pull request was closed: not touched again
        # (review r3 W-1)
        git("update-ref", "refs/remotes/origin/update/kicad-10.0.9", "main")
        rc, log = run_watch(repo, d, ["10.0.9", "10.0.7"])
        check(rc == 0 and "already exists (refs/remotes/origin/update/kicad-10.0.9); nothing done" in log[-1],
              f"watch patch with a remote branch is a no-op: {rc} {log[-1:]}")
        check(not git("branch", "--list", "update/kicad-10.0.9").stdout.strip(), "no local branch made over a remote one")
        check(git("rev-parse", "refs/remotes/origin/update/kicad-10.0.9").stdout == head, "the remote branch is untouched")

        rc, log = run_watch(repo, d, ["11.0.0", "10.0.7"])
        check(rc == 0 and "major-release 11.0.0: a port is needed" in log[-1], f"watch major: {rc} {log[-1:]}")
        check((repo / "reports" / "kicad-11.0.0.md").exists(), "watch major writes its report")
        check(not git("branch", "--list", "update/kicad-11.0.0").stdout.strip(), "watch major makes no branch")

        lock = repo / "reports" / ".watch.lock"
        lock.mkdir()
        (lock / "pid").write_text(str(os.getpid()))  # a live holder: this test
        rc, log = run_watch(repo, d, ["10.0.7"])
        check(rc == 4 and "locked" in log[-1], f"watch with the lock held: {rc} {log[-1:]}")
        (lock / "pid").write_text("999999")  # a dead holder: taken over
        rc, log = run_watch(repo, d, ["10.0.7"])
        check(rc == 0 and " current:" in log[-1] and not lock.exists(), f"watch takes a stale lock: {rc} {log[-1:]}")
        check(all(len(l.split(" ", 1)) == 2 for l in log), "one line per run")


def workflow_jobs(wf):
    """The workflow's text split per job (top-level key -> its lines), and the text
    before `jobs:`. A line reader, not a YAML parser: the file is ours and plain."""
    head, jobs, cur = [], {}, None
    in_jobs = False
    for line in wf.splitlines():
        if line.startswith("jobs:"):
            in_jobs = True
            continue
        if not in_jobs:
            head.append(line)
        elif line.startswith("  ") and not line.startswith("   ") and line.rstrip().endswith(":"):
            cur = line.strip()[:-1]
            jobs[cur] = []
        elif cur:
            jobs[cur].append(line)
    return "\n".join(head), {k: "\n".join(v) for k, v in jobs.items()}


def run_blocks(wf):
    """The text of every `run:` script in the workflow."""
    blocks, lines, i = [], wf.splitlines(), 0
    while i < len(lines):
        line = lines[i]
        stripped = line.lstrip(" -")
        if stripped.startswith("run:"):
            rest = stripped[4:].strip()
            if rest not in ("|", ">"):
                blocks.append(rest)
            else:
                indent = len(line) - len(line.lstrip(" -"))
                body = []
                i += 1
                while i < len(lines) and (not lines[i].strip() or len(lines[i]) - len(lines[i].lstrip()) > indent):
                    body.append(lines[i])
                    i += 1
                blocks.append("\n".join(body))
                continue
        i += 1
    return blocks


def t_workflow():
    """The GitHub workflow (the watcher's primary path): every action pinned by
    commit SHA, the four jobs, least-privilege permissions per job (read-only by
    default), no secret beyond GITHUB_TOKEN, no ${{ }} inside a script, clang on
    Linux, and an update branch never re-created or force-pushed (review r3 W-1, W-2,
    W-5, W-6)."""
    import re
    wf = (ROOT / ".github" / "workflows" / "kicad-watch.yml").read_text()
    uses = re.findall(r"uses:\s*(\S+)", wf)
    check(uses, "the workflow uses actions")
    for u in uses:
        check(re.fullmatch(r"[\w.-]+/[\w.-]+@[0-9a-f]{40}", u) is not None, f"action not pinned by SHA: {u}")
    for job in ("check:", "update:", "macos-gate:", "major:"):
        check(f"\n  {job}" in wf, f"job {job[:-1]} missing")
    head, jobs = workflow_jobs(wf)
    check(re.search(r"^permissions:\n  contents: read\n(?!  )", head + "\n", re.M) is not None,
          "the workflow-level permissions are contents: read only")
    want = {"check": {"contents: read"},
            "update": {"contents: write", "pull-requests: write"},
            "macos-gate": {"contents: read", "pull-requests: write"},
            "major": {"contents: read", "issues: write"}}
    for job, perms in want.items():
        m = re.search(r"\n    permissions:\n((?:      .*\n)+)", jobs.get(job, "") + "\n")
        got = {re.sub(r"\s*#.*", "", x).strip() for x in m.group(1).splitlines()} if m else set()
        check(got == perms, f"job {job}: permissions {sorted(got)} != {sorted(perms)}")
    for i, block in enumerate(run_blocks(wf)):
        check("${{" not in block, f"run block {i}: a ${{{{ }}}} expression inside the script (use env:)")
    check(run_blocks(wf), "the workflow has run blocks")
    upd = jobs.get("update", "")
    check("CC: clang" in upd and "CXX: clang++" in upd, "the update job builds with clang")
    check("--state all" in upd and "git ls-remote --exit-code --heads origin" in upd,
          "the update job dedupes on the remote branch and on pull requests in any state")
    pushes = re.findall(r"git push[^\n]*", wf)
    check(pushes and not any(re.search(r"(--force|\s-f\b|\s\+)", x) for x in pushes), f"no force-push: {pushes}")
    check("--build-arg GOLDEN=0" in upd and "/build/golden.jsonl" in upd, "the docker image's golden lines are taken out")
    check("docker-golden.jsonl" in jobs.get("macos-gate", ""), "the macOS gate compares with the docker lines")
    check("cancel-in-progress: false" in wf, "concurrency without cancel")
    check(re.findall(r"secrets\.(\w+)", wf) == [], "no secrets beyond github.token")
    check("runs-on: macos-14" in wf and "gh pr comment" in wf, "the macOS gate comments on the PR")
    check("gh issue create" in wf and "port needed" in wf, "a major release opens an issue")
    for f in ("tools/kicad_watch.sh", "README.md", "MAINTAINING.md"):
        check("osascript" not in (ROOT / f).read_text(), f"{f}: no desktop notification")
    check(not (ROOT / "ops" / "launchd").exists(), "no launchd template")


def t_every_workflow():
    """Every workflow, not only the watch: actions pinned by commit SHA, a read-only
    workflow default, no pull_request_target (a fork's pull request must never run with
    a write token or secrets), no secret beyond github.token, and only GitHub-hosted
    runners."""
    import re
    wfs = sorted((ROOT / ".github" / "workflows").glob("*.y*ml"))
    check(any(w.name == "ci.yml" for w in wfs), "the pull-request workflow ci.yml exists")
    for w in wfs:
        wf = w.read_text()
        for u in re.findall(r"uses:\s*(\S+)", wf):
            check(re.fullmatch(r"[\w.-]+/[\w.-]+@[0-9a-f]{40}", u) is not None, f"{w.name}: action not pinned by SHA: {u}")
        head, _ = workflow_jobs(wf)
        check(re.search(r"^permissions:\n  contents: read\n(?!  )", head + "\n", re.M) is not None,
              f"{w.name}: the workflow-level permissions are contents: read only")
        code = "\n".join(re.sub(r"(^|\s)#.*", "", x) for x in wf.splitlines())
        check("pull_request_target" not in code, f"{w.name}: no pull_request_target")
        check(re.findall(r"secrets\.(\w+)", wf) == [], f"{w.name}: no secrets beyond github.token")
        for r in re.findall(r"runs-on:\s*(\S+)", wf):
            check(re.fullmatch(r"(ubuntu|macos)-[\w.]+", r) is not None, f"{w.name}: not a GitHub-hosted runner: {r}")


def main():
    for t in (t_check, t_watch, t_workflow, t_every_workflow):
        before = len(FAILS)
        t()
        print(("PASS " if len(FAILS) == before else "FAILED ") + t.__name__)
    print("FAILED" if FAILS else "OK")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
