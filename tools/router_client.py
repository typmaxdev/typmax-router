# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
"""A tiny stdlib client for the typmax-router JSON Lines protocol (PROTOCOL.md)."""
import json
import os
import subprocess

PROTOCOL_VERSION = int(os.environ.get("TYPMAX_PROTOCOL_VERSION", "2"))


class RouterError(Exception):
    def __init__(self, error):
        super().__init__(f"{error.get('code')}: {error.get('message')}")
        self.code = error.get("code")
        self.error = error


class RouterClient:
    def __init__(self, binary, args=(), env=None):
        # UTF-8 explicitly, never the locale's encoding: protocol lines are UTF-8.
        self.proc = subprocess.Popen([binary, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, encoding="utf-8", bufsize=1,
                                     env=env)
        self.next_id = 1

    def raw(self, line):
        """Send one raw line, return the raw response line (no trailing newline)."""
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        out = self.proc.stdout.readline()
        if not out:
            raise RuntimeError(f"router exited (code {self.proc.poll()})")
        return out.rstrip("\n")

    def request(self, method, params=None, raw_response=False):
        rid = self.next_id
        self.next_id += 1
        req = {"id": rid, "protocol_version": PROTOCOL_VERSION, "method": method}
        if params is not None:
            req["params"] = params
        line = self.raw(json.dumps(req, separators=(",", ":")))
        if raw_response:
            return line
        resp = json.loads(line)
        if resp.get("id") != rid:
            raise RuntimeError(f"response id {resp.get('id')} != request id {rid}")
        return resp

    def call(self, method, params=None):
        resp = self.request(method, params)
        if not resp["ok"]:
            raise RouterError(resp["error"])
        return resp["result"]

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            self.proc.wait(timeout=30)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
