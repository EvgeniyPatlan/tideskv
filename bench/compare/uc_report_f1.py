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
    """{family: {'cases': n, server: {'same', 'differs', 'unsupported', 'extra', 'documented', 'undocumented'}}}."""
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
    r = c["results"].get(srv)
    if r is None:
        return None
    return r["status"] != "unsupported" and not (r["status"] == "same" and not c["ref_accepts"])  # same + Redis unknown = unknown


def _type_rows(matrix, servers):
    rows = []
    for c in matrix["cases"]:
        if c["family"] in TYPE_FAMILIES:
            rows.append((c, ["yes" if accepts(c, k) else "no" for k, _ in servers]))
    return rows


def _non_same(matrix, servers):
    rows = []
    for c in matrix["cases"]:
        for k, name in servers:
            r = c["results"].get(k)
            if r and r["status"] != "same":
                label = "documented: [%s](%s)" % (_short(r["documented"]["note"], 60), r["documented"]["url"]) \
                    if r.get("documented") else ("not a difference (Redis 7.2 lacks it)" if r["status"] == "extra" else "undocumented")
                rows.append("| %s | %s | %s | %s | `%s` | %s | %s | %s |" % (
                    c["family"], _short(c["case"], 40), name, r["status"], _short(r.get("command"), 50),
                    _short(r.get("ref")), _short(r.get("got")), label))
    return rows


def render_f1(matrix):
    servers = _servers(matrix)
    out = ["Reference: %s (the neutral oracle: a reply is `same` when it equals Redis' on every command of the case). "
           "%d cases, each on a flushed database, one repetition (the run is deterministic)." % (matrix["ref"], len(matrix["cases"])),
           "", "`differs`: a reply differs from Redis'. `unsupported`: the server answers unknown command or subcommand where "
           "Redis succeeds. `extra`: Redis 7.2 itself answers unknown and the server does not (a command the reference lacks; not "
           "a difference). `documented`: how many of the differs and unsupported results match a deviation the server's own "
           "documentation describes (the link is in the list below). The label applies to a server only through that server's "
           "own documentation. On the type probes (streams to bloom) `same` with a `no` in the types table means Redis 7.2 and the "
           "server both lack the command.", "",
           "Error replies compare by their code word (`ERR`, `WRONGTYPE`, ...), not by their text; "
           "`#unordered` cases compare sets of replies, not their order.", ""]
    n_text = {k: sum(1 for c in matrix["cases"] if (c["results"].get(k) or {}).get("error_text_differs")) for k, _ in servers}
    out += ["Cases whose replies are `same` but whose error text differs: " + ", ".join(
        "%s %d" % (n, n_text[k]) for k, n in servers) + ".", ""]
    out += ["### Per family", "", "| Family | Cases | " + " | ".join(n for _, n in servers) + " |",
            "|---|---|" + "---|" * len(servers)]
    for fam, d in family_counts(matrix).items():
        out.append("| %s | %d | %s |" % (fam, d["cases"], " | ".join(_cell(d.get(k)) for k, _ in servers)))
    out += ["", "### Types only one side has", "", "yes = the probe command is accepted; no = the server answers unknown command.", "",
            "| Type | Probe | Redis 7.2 | " + " | ".join(n for _, n in servers) + " |", "|---|---|---|" + "---|" * len(servers)]
    for c, ans in _type_rows(matrix, servers):
        out.append("| %s | `%s` | %s | %s |" % (TYPE_TITLES[c["family"]], c["case"], "yes" if c["ref_accepts"] else "no", " | ".join(ans)))
    rows = _non_same(matrix, servers)
    out += ["", "### Non-same results, per case", ""]
    out += (["| Family | Case | Server | Result | First differing command | Redis 7.2 | Server | Source |",
             "|---|---|---|---|---|---|---|---|"] + rows) if rows else ["Every case is `same` on every server."]
    return out


def f1_items(matrix):
    """Pros and cons from F1: a family where one server is all same and the other has undocumented non-same results, and a
    type only one side accepts."""
    items = {"tideskv_pros": [], "tideskv_cons": [], "kvrocks_pros": [], "kvrocks_cons": []}
    servers = _servers(matrix)
    if len(servers) < 2:
        return items
    (ka, na), (kb, nb) = servers
    pro = {ka: items[ka + "_pros"], kb: items[kb + "_pros"]}
    con = {ka: items[ka + "_cons"], kb: items[kb + "_cons"]}
    for fam, d in family_counts(matrix).items():
        if fam in TYPE_FAMILIES or ka not in d or kb not in d:
            continue
        for good, bad, gn, bn in ((ka, kb, na, nb), (kb, ka, nb, na)):
            g, b = d[good], d[bad]
            if g["differs"] + g["unsupported"] == 0 and b["undocumented"] > 0:
                pro[good].append("F1 %s: %s matches Redis 7.2 on all %d cases" % (fam, gn, d["cases"]))
                con[bad].append("F1 %s: %s has %d undocumented difference(s) from Redis 7.2 in %d cases" % (fam, bn, b["undocumented"], d["cases"]))
    for c in matrix["cases"]:
        if c["family"] not in TYPE_FAMILIES:
            continue
        a, b = accepts(c, ka), accepts(c, kb)
        if a is None or b is None or a == b:
            continue
        good, bad, gn, bn = (ka, kb, na, nb) if a else (kb, ka, nb, na)
        pro[good].append("F1 type: %s has %s, %s does not" % (gn, TYPE_TITLES[c["family"]], bn))
        con[bad].append("F1 type: %s has no %s, %s has" % (bn, TYPE_TITLES[c["family"]], gn))
    return items
