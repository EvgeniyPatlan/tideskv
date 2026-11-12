#!/usr/bin/env python3
"""F1 command matrix: the same commands against Redis 7.2 (the reference) and each target, replies compared.

  f1_matrix.py run --target NAME=host:port ... --ref host:port --cases f1_cases.txt [--documented f1_documented.txt] --out m.json
  f1_matrix.py merge FILE... --out m.json     (combine runs of different targets against the same reference)

Case file: blocks `## <family> | <case name>`, then one command per line (shell-like quoting), run on a FLUSHALLed db.
A line may end in a directive: `#unordered` (arrays compared as sorted multisets), `#length` (only the array's length, for random
picks: membership is then checked by later commands), `#shape` (only reply types and array lengths), `#fields=a,b` (a bulk INFO-style reply: only those `key:value` lines). `@sleep S` waits. Lines starting `# ` are comments.

Per case per target: `same`, `differs` (first differing command, both replies), `unsupported` (the target answers
"unknown command/subcommand" where Redis does not), or `extra` (Redis 7.2 answers unknown and the target does not: a
command the reference lacks, not a difference). Error replies compare by their code word (ERR, WRONGTYPE, ...); a same-code
difference in text is counted in `error_text_differs`. Stdlib only.
"""
import argparse
import json
import re
import shlex
import socket
import sys
import time

TYPE_FAMILIES = ("streams", "bitmaps", "hyperloglog", "geo", "pubsub", "json", "search", "bloom")
FAMILIES = ("strings", "keyspace", "expire", "hash", "list", "set", "zset", "transactions", "scripting", "server/info",
            "notifications", "tracking", "object") + TYPE_FAMILIES
_DIRECTIVE = re.compile(r"\s#(unordered|length|shape|fields=\S+)\s*$")


# ---- case and documented files ----

def parse_cases(path):
    cases, cur, seen = [], None, set()
    with open(path) as f:
        lines = f.read().splitlines()
    for n, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or (line.startswith("#") and not line.startswith("##")):
            continue
        if line.startswith("##"):
            fam, _, name = (x.strip() for x in line[2:].partition("|"))
            if fam not in FAMILIES or not name:
                raise ValueError("%s:%d: bad block header %r (family must be one of %s)" % (path, n, line, FAMILIES))
            if (fam, name) in seen:
                raise ValueError("%s:%d: duplicate case %r in family %s" % (path, n, name, fam))
            seen.add((fam, name))
            cur = {"family": fam, "case": name, "commands": []}
            cases.append(cur)
            continue
        if cur is None:
            raise ValueError("%s:%d: command before the first block" % (path, n))
        cur["commands"].append(_parse_command(line))
    for c in cases:
        if not c["commands"]:
            raise ValueError("%s: case %r has no commands" % (path, c["case"]))
    return cases


def _parse_command(line):
    if line.startswith("@sleep "):
        return {"argv": [], "mode": None, "sleep": float(line.split()[1])}
    mode = None
    m = _DIRECTIVE.search(line)
    if m:
        mode, line = m.group(1), line[:m.start()]
    return {"argv": [a.encode() for a in shlex.split(line)], "mode": mode, "sleep": 0}


def parse_documented(path):
    """{(target section, family, case): {url, note, command}} from `## kvrocks` / `## tideskv` sections of
    `family | case | url | note`. `case @CMD` limits the label to a first differing command CMD."""
    out, section = {}, None
    with open(path) as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("# "):
                continue
            if line.startswith("##"):
                section = line[2:].strip()
                continue
            parts = [p.strip() for p in line.split("|", 3)]
            if section is None or len(parts) != 4:
                raise ValueError("%s: bad line %r" % (path, line))
            name, _, only = parts[1].partition(" @")
            out[(section, parts[0], name)] = {"url": parts[2], "note": parts[3], "command": only.upper() or None}
    return out


# ---- RESP ----

class Conn:
    def __init__(self, addr, timeout=5.0):
        host, _, port = addr.rpartition(":")
        self.sock = socket.create_connection((host, int(port)), timeout=timeout)
        self.f = self.sock.makefile("rb")

    def close(self):
        try:
            self.f.close()
            self.sock.close()
        except OSError:
            pass

    def call(self, argv):
        try:
            self.sock.sendall(b"*%d\r\n" % len(argv) + b"".join(b"$%d\r\n%s\r\n" % (len(a), a) for a in argv))
            return self._read()
        except (OSError, ValueError) as e:
            return ("e", "CONN %s: %s" % (type(e).__name__, e))

    def _read(self):
        line = self.f.readline()
        if not line:
            raise ConnectionError("closed")
        kind, rest = line[:1], line[1:-2]
        if kind == b"+":
            return ("s", rest)
        if kind == b"-":
            return ("e", rest.decode("utf-8", "replace"))
        if kind == b":":
            return ("i", int(rest))
        if kind == b"$":
            n = int(rest)
            return ("b", None) if n < 0 else ("b", self.f.read(n + 2)[:-2])
        if kind == b"*":
            n = int(rest)
            return ("a", None) if n < 0 else ("a", [self._read() for _ in range(n)])
        raise ValueError("unexpected reply type %r" % kind)


def show(r, limit=160):
    k, v = r
    if k == "s":
        s = "+" + v.decode("utf-8", "replace")
    elif k == "e":
        s = "-" + v
    elif k == "i":
        s = ":%d" % v
    elif k == "b":
        s = "(nil)" if v is None else '"%s"' % v.decode("utf-8", "replace")
    else:
        s = "(nil array)" if v is None else "[" + ", ".join(show(x, limit) for x in v) + "]"
    return s if len(s) <= limit else s[:limit] + "..."


def is_unknown(r):
    return r[0] == "e" and ("unknown command" in r[1].lower() or "unknown subcommand" in r[1].lower())


def _code(msg):
    return msg.split(" ", 1)[0]


def _norm(r, mode):
    k, v = r
    if k == "e":
        return ("e", _code(v))
    if mode == "fields":
        return r
    if mode == "length":
        return ("a", len(v)) if k == "a" and v is not None else r
    if mode == "shape":
        return ("a", [_norm(x, mode) for x in v]) if k == "a" and v is not None else (k, None)
    if k == "a" and v is not None:
        items = [_norm(x, mode) for x in v]
        return ("a", sorted(items, key=repr) if mode == "unordered" else items)
    return r


def _fields(r, wanted):
    if r[0] != "b" or r[1] is None:
        return r
    got = {}
    for line in r[1].decode("utf-8", "replace").splitlines():
        key, _, val = line.partition(":")
        if key in wanted:
            got[key] = val.strip()
    return ("f", sorted(got.items()))


def _prepare(r, mode):
    if mode and mode.startswith("fields="):
        r, mode = _fields(r, mode[7:].split(",")), "fields"
    return _norm(r, mode)


def judge(cmd, ref, got):
    """('same'|'differs'|'unsupported'|'extra', error_text_differs) for one command's replies. `unsupported`: the target
    does not know the command or subcommand where Redis succeeds. `extra`: Redis does not know the command and the target
    does. Two errors compare by code word (an unknown command on both is `same`)."""
    if is_unknown(got) and ref[0] != "e":
        return "unsupported", False
    if ref[0] == "e" and "unknown command" in ref[1].lower() and not is_unknown(got):
        return "extra", False
    same = _prepare(ref, cmd["mode"]) == _prepare(got, cmd["mode"])
    return ("same" if same else "differs"), same and ref[0] == "e" and got[0] == "e" and ref[1] != got[1]


# ---- running ----

def _execute(conn, commands):
    out = []
    for c in commands:
        if c["sleep"]:
            time.sleep(c["sleep"])
            out.append(None)
        else:
            out.append(conn.call(c["argv"]))
    return out


def _fresh(addr):
    conn = Conn(addr)
    conn.call([b"FLUSHALL"])
    return conn


def _compare(commands, ref_replies, conn):
    errs = False
    for c, ref in zip(commands, ref_replies):
        if c["sleep"]:
            time.sleep(c["sleep"])
            continue
        got = conn.call(c["argv"])
        status, text = judge(c, ref, got)
        errs = errs or text
        if status != "same":
            return {"status": status, "command": " ".join(a.decode("utf-8", "replace") for a in c["argv"]),
                    "ref": show(ref), "got": show(got), "error_text_differs": errs}
    return {"status": "same", "error_text_differs": errs}


def _ref_version(addr):
    conn = Conn(addr)
    try:
        r = conn.call([b"INFO", b"server"])
    finally:
        conn.close()
    m = re.search(rb"redis_version:(\S+)", r[1] or b"") if r[0] == "b" else None
    return "redis " + m.group(1).decode() if m else "unknown"


def run(targets, ref_addr, cases_path, documented_path, out_path):
    cases = parse_cases(cases_path)
    documented = parse_documented(documented_path) if documented_path else {}
    names = [t.split("=", 1)[0] for t in targets]
    addrs = dict(t.split("=", 1) for t in targets)
    matrix = {"ref": _ref_version(ref_addr) + " at " + ref_addr, "targets": names, "cases": []}
    for case in cases:
        ref = _fresh(ref_addr)
        replies = _execute(ref, case["commands"])
        ref.close()
        first = next((r for c, r in zip(case["commands"], replies) if not c["sleep"]), ("e", ""))
        entry = {"family": case["family"], "case": case["case"], "ref_accepts": not is_unknown(first), "results": {}}
        for name in names:
            conn = _fresh(addrs[name])
            res = _compare(case["commands"], replies, conn)
            conn.close()
            doc = documented.get((name, case["family"], case["case"])) if res["status"] in ("differs", "unsupported") else None
            if doc and doc["command"] and res["command"].split()[0].upper() != doc["command"]:
                doc = None
            res["documented"] = doc and {"url": doc["url"], "note": doc["note"]}
            entry["results"][name] = res
        matrix["cases"].append(entry)
    with open(out_path, "w") as f:
        json.dump(matrix, f, indent=1)
    return matrix


def merge(matrices):
    first = matrices[0]
    out = {"ref": first["ref"], "targets": [], "cases": [dict(c, results={}) for c in first["cases"]]}
    index = {(c["family"], c["case"]): c for c in out["cases"]}
    for m in matrices:
        out["targets"] += [t for t in m["targets"] if t not in out["targets"]]
        for c in m["cases"]:
            index[(c["family"], c["case"])]["results"].update(c["results"])
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--target", action="append", required=True, metavar="NAME=host:port")
    r.add_argument("--ref", required=True)
    r.add_argument("--cases", required=True)
    r.add_argument("--documented")
    r.add_argument("--out", required=True)
    m = sub.add_parser("merge")
    m.add_argument("files", nargs="+")
    m.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    if a.cmd == "run":
        run(a.target, a.ref, a.cases, a.documented, a.out)
    else:
        docs = []
        for p in a.files:
            with open(p) as f:
                docs.append(json.load(f))
        with open(a.out, "w") as f:
            json.dump(merge(docs), f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
