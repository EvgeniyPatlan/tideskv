"""F1 section of the use-case report (stdlib only): the command matrix per family, the per-case non-same results, the
types only one side has, and the F1 pros and cons. Input: the parsed F1/matrix.json written by f1_matrix.py."""
from collections import OrderedDict

from f1_matrix import FAMILIES, TYPE_FAMILIES

SERVERS = (("tideskv", "TidesKV"), ("kvrocks", "Kvrocks"))
STATUS_ORDER = ("same", "differs", "unsupported", "extra")
TYPE_TITLES = {"streams": "Streams", "bitmaps": "Bitmaps", "hyperloglog": "HyperLogLog", "geo": "Geo", "pubsub": "Pub/Sub",
               "json": "JSON", "search": "Search", "bloom": "Bloom"}


def _servers(matrix):
    return [(k, n) for k, n in SERVERS if k in matrix.get("targets", [])]


def family_counts(matrix):
    """{family: {'cases': n, server: {'same', 'differs', 'unsupported', 'extra', 'documented', 'undocumented'}}}.
    A case is documented only when every one of its differences is."""
    out = OrderedDict((f, {"cases": 0}) for f in FAMILIES)
    for c in matrix["cases"]:
        fam = out.setdefault(c["family"], {"cases": 0})
        fam["cases"] += 1
        for srv, r in c["results"].items():
            d = fam.setdefault(srv, dict.fromkeys(STATUS_ORDER + ("documented", "undocumented"), 0))
            d[r["status"]] += 1
            if r["status"] in ("differs", "unsupported"):
                d["documented" if r.get("documented") else "undocumented"] += 1
    return OrderedDict((f, d) for f, d in out.items() if d["cases"])


def _cell(d):
    if d is None:
        return "no data"
    text = "same %d / differs %d / unsupported %d / documented %d" % (d["same"], d["differs"], d["unsupported"], d["documented"])
    return text + (" / extra %d" % d["extra"] if d["extra"] else "")


def _short(s, n=70):
    s = (s or "").replace("|", "\\|").replace("\n", " ")
    return s if len(s) <= n else s[:n] + "..."


def accepts(c, srv):
    """Does the server accept the case's first command (anything but an unknown-command style refusal)?"""
    r = c["results"].get(srv)
    return None if r is None else r["accepts"]


def _type_rows(matrix, servers):
    return [(c, ["yes" if accepts(c, k) else "no" for k, _ in servers]) for c in matrix["cases"] if c["family"] in TYPE_FAMILIES]


def _source(d):
    if d["status"] == "extra":
        return "not a difference (Redis 7.2 lacks it)"
    doc = d.get("documented")
    return "documented: [%s](%s)" % (_short(doc["note"], 60), doc["url"]) if doc else "undocumented"


def _non_same(matrix, servers):
    rows = []
    for c in matrix["cases"]:
        for k, name in servers:
            for d in (c["results"].get(k) or {}).get("diffs", []):
                rows.append("| %s | %s | %s | %s | `%s` | %s | %s | %s |" % (
                    c["family"], _short(c["case"], 40), name, d["status"], _short(d["command"], 50),
                    _short(d["ref"]), _short(d["got"]), _source(d)))
    return rows


def _error_text_rows(matrix, servers):
    rows = []
    for c in matrix["cases"]:
        for k, name in servers:
            for t in (c["results"].get(k) or {}).get("error_text", []):
                rows.append("| %s | %s | %s | `%s` | %s | %s |" % (c["family"], _short(c["case"], 40), name,
                                                              _short(t["command"], 50), _short(t["ref"], 100), _short(t["got"], 100)))
    return rows


LEGEND = (
    "Reference: %s (the neutral oracle: a command is `same` when its reply equals Redis'). %d cases, each on a flushed "
    "database, every command of a case compared, one repetition (expiry-timing cases, such as EXPIRE with 0 or a negative time, can differ between runs).",
    "Per case: `differs`: a reply differs from Redis'. `unsupported`: every difference is a refusal, the server answers unknown "
    "command or subcommand (or Kvrocks' equivalents, see `is_unknown` in `f1_matrix.py`) where Redis succeeds. `extra`: Redis 7.2 "
    "itself answers unknown command and the server answers anything else, so any non-refusal reply counts as having the command "
    "(a probe such as `FT.INFO` may reply with an error and still be counted as present). `documented`: how many of the differs "
    "and unsupported cases have every difference described in the server's own documentation (the link is in the list below); a "
    "case with only some documented differences is `differs` and counts as undocumented. The label applies to a server only "
    "through that server's own documentation.",
    "Later commands of a case can differ because an earlier one did (for example after a refused SELECT, OBJECT or SCRIPT KILL); "
    "the list gives every differing command, so read the rows of one case together.",
    "Kvrocks ran with its default `redis-databases 0`, where `SELECT` does not switch databases; the setting is configurable "
    "([supported commands](https://kvrocks.apache.org/docs/supported-commands)), so the SELECT-related differences below depend on "
    "that choice and a Kvrocks configured with `redis-databases` above 0 would not show them.",
    "Error replies compare by their code word (`ERR`, `WRONGTYPE`, ...), not by their text (see the appendix); `#unordered` and "
    "`#length` cases compare sets and lengths of replies, not their order.",
)


def render_f1(matrix):
    servers = _servers(matrix)
    out = [LEGEND[0] % (matrix["ref"], len(matrix["cases"])), ""] + [p for t in LEGEND[1:] for p in (t, "")]
    out += ["On the type probes (streams to bloom) `same` with a `no` in the types table means Redis 7.2 and the server both "
            "lack the command.", ""]
    out += ["### Per family", "", "| Family | Cases | " + " | ".join(n for _, n in servers) + " |", "|---|---|" + "---|" * len(servers)]
    for fam, d in family_counts(matrix).items():
        out.append("| %s | %d | %s |" % (fam, d["cases"], " | ".join(_cell(d.get(k)) for k, _ in servers)))
    out += ["", "### Types only one side has", "", "yes = the first probe command is accepted; no = the server refuses it.", "",
            "| Type | Probe | Redis 7.2 | " + " | ".join(n for _, n in servers) + " |", "|---|---|---|" + "---|" * len(servers)]
    for c, ans in _type_rows(matrix, servers):
        out.append("| %s | `%s` | %s | %s |" % (TYPE_TITLES[c["family"]], c["case"], "yes" if c["ref_accepts"] else "no", " | ".join(ans)))
    rows = _non_same(matrix, servers)
    out += ["", "### Differing commands, per case", ""]
    out += (["| Family | Case | Server | Result | Command | Redis 7.2 | Server | Source |", "|---|---|---|---|---|---|---|---|"] + rows
            ) if rows else ["Every command is `same` on every server."]
    text_rows = _error_text_rows(matrix, servers)
    if text_rows:
        out += ["", "<details><summary>Same error code, different text (%d commands; not counted above)</summary>" % len(text_rows), "",
                "| Family | Case | Server | Command | Redis 7.2 | Server |", "|---|---|---|---|---|---|"] + text_rows + ["", "</details>"]
    return out


def _cause(d):
    if d["status"] == "unsupported":
        return "refuses commands Redis 7.2 accepts"
    if d["ref_kind"] == d["got_kind"]:
        return "different %s content" % d["ref_kind"]
    return "%s replaced by %s" % (d["ref_kind"], d["got_kind"])


CONTAINERS = {"CONFIG", "CLIENT", "COMMAND", "SCRIPT", "OBJECT", "CLUSTER", "CLUSTERX", "FUNCTION", "MEMORY", "LATENCY",
              "SLOWLOG", "PUBSUB", "DEBUG"}


def _cmd_name(command):
    """The command word, with the subcommand for container commands (CONFIG SET, CLIENT TRACKING)."""
    words = command.upper().split()
    return " ".join(words[:2]) if words[0] in CONTAINERS and len(words) > 1 else words[0]


def _undocumented_causes(matrix, srv):
    """{cause: {'families': set, 'commands': [..]}} over the server's undocumented differs/unsupported commands."""
    causes = OrderedDict()
    for c in matrix["cases"]:
        r = c["results"].get(srv) or {}
        if r.get("documented"):
            continue
        for d in r.get("diffs", []):
            if d["status"] in ("differs", "unsupported") and not d.get("documented"):
                e = causes.setdefault(_cause(d), {"families": [], "commands": []})
                if c["family"] not in e["families"]:
                    e["families"].append(c["family"])
                name = _cmd_name(d["command"])
                if name not in e["commands"]:
                    e["commands"].append(name)
    return causes


def _listing(names, cap=12):
    return ", ".join(names[:cap]) + (" and %d more" % (len(names) - cap) if len(names) > cap else "")


def f1_items(matrix):
    """F1 pros and cons, the same rules for both servers. Cons: every server's undocumented differences, one item per
    root cause (the kind of Redis' reply and of the server's), naming families and commands. Pros: a family (other than the
    type probes) where one server is `same` in every case and the other has undocumented differences (a documented
    absence is no pro). A type only one side accepts: a
    pro for that side, a con for the other."""
    items = {"tideskv_pros": [], "tideskv_cons": [], "kvrocks_pros": [], "kvrocks_cons": []}
    servers = _servers(matrix)
    for srv, name in servers:
        for cause, e in _undocumented_causes(matrix, srv).items():
            items[srv + "_cons"].append("F1: %s %s, undocumented: %s (%d command%s: %s)" % (
                name, cause, ", ".join(e["families"]), len(e["commands"]), "" if len(e["commands"]) == 1 else "s",
                _listing(e["commands"])))
    if len(servers) < 2:
        return items
    for fam, d in family_counts(matrix).items():
        if fam in TYPE_FAMILIES:  # a type a server documents as missing is no pro; the types rule below covers it
            continue
        for (good, gn), (bad, bn) in (servers, servers[::-1]):
            if good in d and bad in d and d[good]["same"] == d["cases"] and d[bad]["undocumented"] > 0:
                items[good + "_pros"].append("F1 %s: %s matches Redis 7.2 on all %d cases; %s has %d undocumented case(s)" % (
                    fam, gn, d["cases"], bn, d[bad]["undocumented"]))
    (ka, na), (kb, nb) = servers
    for c in matrix["cases"]:
        if c["family"] not in TYPE_FAMILIES:
            continue
        a, b = accepts(c, ka), accepts(c, kb)
        if a is None or b is None or a == b:
            continue
        good, bad, gn, bn = (ka, kb, na, nb) if a else (kb, ka, nb, na)
        items[good + "_pros"].append("F1 type: %s has %s, %s does not" % (gn, TYPE_TITLES[c["family"]], bn))
        items[bad + "_cons"].append("F1 type: %s has no %s, %s has" % (bn, TYPE_TITLES[c["family"]], gn))
    return items
