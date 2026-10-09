# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The typmax-router authors
"""A small S-expression reader for .kicad_pcb files (stdlib only).

parse(text) -> (tree, spans): tree is nested lists of atoms (str) where quoted
strings are kept as Str (a str subclass, so `isinstance(x, Str)` tells them
apart); spans[i] is the (start, end) byte range of the i-th top-level child of
the root list, so a writer can cut items out of the original text verbatim."""


class Str(str):
    """A quoted string atom."""


def _tokens(text):
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
        elif c == "(" or c == ")":
            yield c, i, i + 1
            i += 1
        elif c == '"':
            j = i + 1
            buf = []
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    nxt = text[j + 1]
                    buf.append({"n": "\n", "t": "\t", "r": "\r"}.get(nxt, nxt))
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            yield Str("".join(buf)), i, j + 1
            i = j + 1
        else:
            j = i
            while j < n and text[j] not in " \t\r\n()":
                j += 1
            yield text[i:j], i, j
            i = j


def parse(text):
    stack = [[]]
    starts = []
    spans = []
    for tok, s, e in _tokens(text):
        if tok == "(" and not isinstance(tok, Str):
            stack.append([])
            starts.append(s)
        elif tok == ")" and not isinstance(tok, Str):
            done = stack.pop()
            start = starts.pop()
            stack[-1].append(done)
            if len(stack) == 2:  # a direct child of the root list
                spans.append((start, e))
        else:
            stack[-1].append(tok)
    if len(stack) != 1 or len(stack[0]) != 1:
        raise ValueError("unbalanced S-expression")
    return stack[0][0], spans


def head(node):
    return node[0] if isinstance(node, list) and node and not isinstance(node[0], list) else None


def children(node, name):
    return [c for c in node if isinstance(c, list) and head(c) == name]


def child(node, name):
    for c in node:
        if isinstance(c, list) and head(c) == name:
            return c
    return None


def value(node, name, default=None):
    c = child(node, name)
    return c[1] if c is not None and len(c) > 1 else default


def quote(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'
