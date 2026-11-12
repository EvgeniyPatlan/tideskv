#!/usr/bin/env python3
"""F1 command matrix: the same commands against Redis 7.2 (the reference) and each target, replies compared.

  f1_matrix.py run --target NAME=host:port ... --ref host:port --cases f1_cases.txt [--documented f1_documented.txt] --out m.json
  f1_matrix.py merge FILE... --out m.json     (combine runs of different targets against the same reference)

Case file: blocks `## <family> | <case name>`, then one command per line (shell-like quoting), run on a FLUSHALLed db.
A line may end in a directive: `#unordered` (arrays compared as sorted multisets), `#length` (only the array's length, for random
picks: membership is then checked by later commands), `#shape` (only reply types and array lengths), `#fields=a,b` (a bulk INFO-style reply: only those `key:value` lines). `@sleep S` waits. Lines starting `# ` are comments.

Every command of a case is compared (a later command may differ because an earlier one did: the report says so). Per
command: `same`, `differs`, `unsupported` (the target refuses the command or subcommand where Redis succeeds: see
is_unknown) or `extra` (Redis 7.2 answers unknown command and the target answers anything else: a command the reference
lacks, not a difference). Per case per target: `same` when every command is; else `differs` when any command differs or the
differences are only partly documented, `unsupported` when all are refusals, `extra` when all are extras. Error replies
compare by their code word (ERR, WRONGTYPE, ...); same-code differences in text are listed in `error_text`, not counted.
Stdlib only.
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
    """{(target section, family, case): [{url, note, command, index}]} from `## kvrocks` / `## tideskv` sections of lines
    `family | case @CMD | url | note`. @CMD is mandatory. A bare command word covers every differing CMD of the case. A line
    that pins the full argv (`@GET a`) or the 1-based command index (`@DBSIZE #3`) covers only that command; a note
    starting "follows from" (a difference that only follows from an earlier documented one) must pin."""
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
            name, sep, cmd = parts[1].partition(" @")
            m = re.match(r"^(.*?)(?: #(\d+))?$", cmd.strip())
            if not sep or not m.group(1).strip():
                raise ValueError("%s: line without @COMMAND: %r" % (path, line))
            argv = m.group(1).upper().split()
            entry = {"url": parts[2], "note": parts[3], "command": " ".join(argv), "index": int(m.group(2) or 0) or None}
            if parts[3].lower().startswith("follows from") and len(argv) == 1 and not entry["index"]:
                raise ValueError("%s: a follows-from line must pin the full command or its #index: %r" % (path, line))
            out.setdefault((section, parts[0], name), []).append(entry)
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


_REFUSAL = re.compile(r"unknown command|unknown subcommand|unknown \w+ subcommand|subcommand must be")
_LISTING = re.compile(r"syntax error, try (\w+) ([\w |:]+)", re.I)


def is_unknown(r, argv=None):
    """The server refuses a command or subcommand it does not implement. Redis: `unknown command`, `unknown subcommand`.
    Kvrocks: `Unknown SCRIPT subcommand or wrong number of arguments`, `object subcommand must be dump`, `Command
    subcommand must be one of COUNT, GETKEYS, INFO`. Kvrocks' CLIENT answers every bad call, an unknown subcommand and
    the wrong arguments of a supported one alike (checked by hand: `CLIENT TRACKING ON`, `CLIENT NOSUCH` and `CLIENT
    SETNAME` all get `Syntax error, try CLIENT LIST|INFO|KILL ...`), so that text is a refusal only when argv's
    subcommand is not among the ones the message lists itself. Arity errors are not refusals."""
    if r[0] != "e":
        return False
    msg = r[1].lower()
    if _REFUSAL.search(msg):
        return True
    m = _LISTING.search(r[1])
    if m and argv and len(argv) > 1 and argv[0].upper() == m.group(1).upper().encode():
        listed = {w.split(" ")[0].upper() for w in m.group(2).strip().split("|")}
        return argv[1].decode("utf-8", "replace").upper() not in listed
    return False


def kind(r):
    k, v = r
    if k == "e":
        return "error " + _code(v)
    return {"s": "status", "i": "integer", "b": "nil-bulk" if v is None else "bulk",
            "a": "nil-array" if v is None else "array"}[k]


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
    if is_unknown(got, cmd["argv"]) and ref[0] != "e":
        return "unsupported", False
    if ref[0] == "e" and "unknown command" in ref[1].lower() and not is_unknown(got, cmd["argv"]):
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


def _name(argv):
    return " ".join(a.decode("utf-8", "replace") for a in argv)


def _compare(commands, ref_replies, conn):
    """Every command of the case against the reference's recorded replies: {status, diffs, error_text, accepts}."""
    diffs, texts, accepts, n = [], [], None, 0
    for c, ref in zip(commands, ref_replies):
        if c["sleep"]:
            time.sleep(c["sleep"])
            continue
        n += 1
        got = conn.call(c["argv"])
        if accepts is None:
            accepts = not is_unknown(got, c["argv"])
        status, text = judge(c, ref, got)
        if text:
            texts.append({"command": _name(c["argv"]), "ref": show(ref), "got": show(got)})
        if status != "same":
            diffs.append({"command": _name(c["argv"]), "n": n, "status": status, "ref": show(ref), "got": show(got),
                          "ref_kind": kind(ref), "got_kind": kind(got), "documented": None})
    return {"status": _overall(diffs), "diffs": diffs, "error_text": texts, "accepts": bool(accepts),
            "documented": None, "undocumented": [d["command"] for d in diffs]}


def _overall(diffs):
    if not diffs:
        return "same"
    states = {d["status"] for d in diffs}
    return "differs" if "differs" in states else sorted(states)[-1]  # unsupported, else extra


def _covers(entry, d):
    words = d["command"].upper().split()
    if entry["index"] and d["n"] != entry["index"]:
        return False
    want = entry["command"].split()
    return words == want if len(want) > 1 else words[0] == want[0]


def label(res, entries):
    """Documented labels per differing command (`differs`/`unsupported` ones only; entries are the server's lines for the
    case, see parse_documented). The case is documented when every one of those differences is (`extra` commands are no
    differences); a partly documented one is `differs` and names the undocumented commands."""
    flagged = [d for d in res["diffs"] if d["status"] in ("differs", "unsupported")]
    for d in flagged:
        d["documented"] = next(({"url": e["url"], "note": e["note"]} for e in entries if _covers(e, d)), None)
    open_cmds = [d["command"] for d in flagged if not d["documented"]]
    res["undocumented"] = open_cmds
    if flagged and not open_cmds:
        res["documented"] = [d["documented"] for d in flagged]
    elif len(open_cmds) < len(flagged):
        res["status"] = "differs"  # partly documented
    return res


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
            label(res, documented.get((name, case["family"], case["case"]), []))
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
