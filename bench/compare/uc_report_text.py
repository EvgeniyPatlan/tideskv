"""Markdown rendering of the use-case suite report (stdlib only); render(summary) -> str, in spec section 6's order."""
import json
import math

import uc_cells
import uc_report_method

CASES = ("U1", "U2", "U3", "U4", "U5", "U6", "U7", "U8", "F1")
NAMES = {"tideskv": "TidesKV", "kvrocks": "Kvrocks", "kvrocks-blob": "Kvrocks+BlobDB", "kvrocks-cron": "Kvrocks+cron"}
F1_NOT_RUN = "F1 not run"
F2_NOT_RUN = "F2 not run"
QUICK_BANNER = "quick: not a disk result, no verdicts"
SPARK = "▁▂▃▄▅▆▇█"
INFO = {
    "U1": ("Cache bigger than RAM, skewed reads", "a read-heavy cache whose hot keys are a small part of a dataset larger "
           "than memory", "How do the two servers serve skewed GET-heavy traffic from a dataset bigger than the block "
           "cache, closed-loop and at equal offered load?"),
    "U2": ("TTL churn and space reclaim", "a session or token store where every key expires", "Does disk usage follow "
           "the live data when everything written expires, and what does the churn cost in tail latency? Kvrocks runs "
           "twice: default and with a per-minute compaction checker."),
    "U3": ("Aged store, small updates, long run", "a long-lived counter or profile store updated in place",
           "After the keyspace has been overwritten twice, how stable is tail latency over a long mixed run, and how "
           "much background work is the server carrying?"),
    "U4": ("Hashes as records", "records stored as hashes with many small fields", "How fast are HGET, HSET, HGETALL "
           "and HDEL on a dataset of 20-field hashes?"),
    "U5": ("Mid-size values", "values of 1, 4 and 16 KB (user records, small documents)", "How do throughput, tail "
           "latency and disk footprint change with value size? Kvrocks runs with and without BlobDB."),
    "U6": ("Durable writes, connection sweep", "a store that must not lose acknowledged writes",
           "What does an fsync per acknowledged write cost at 1, 8, 64 and 256 connections, against the disk's own fsync floor?"),
    "U7": ("Restart and crash recovery", "a server that is restarted or killed under write load",
           "After SIGTERM or SIGKILL under write load at strict durability, is any acknowledged write lost, and how long "
           "until the server answers and is fast again?"),
    "U8": ("Bulk load and footprint", "an initial import and the disk space it takes",
           "How fast does each server take a bulk load, how long until it is quiet, and how much disk does it occupy "
           "after load, after aging, and (Kvrocks) after a full compaction?"),
}
OTHER_NOTES = {
    "U1": ["Cache sizing: TidesDB charges its index and bloom blocks to its block cache, as Kvrocks does with "
           "`cache_index_and_filter_blocks yes`, so both sides have 2048 MB of total read cache.",
           "Closed-loop latency is not compared across servers (they run at different throughputs): latency verdicts "
           "come from the open-loop phases, which run in full mode only."],
    "U2": ["Neither server had a manual COMPACT: forcing one on a single side would not be a like-for-like comparison.",
           "GETs draw keys from a 1e9-key space, so nearly all are negative lookups; the hit rate is reported as "
           "information, not checked."],
    "U3": ["Both servers had the same aging. Background debt is shown as information, not as a verdict."],
    "U4": ["HGET/HGETALL replies are not hit-counted by memtier, so the reply check covers the integer replies of "
           "HSET and HDEL only.",
           "Per-command ops/s follow the mix ops/s (the command ratio is fixed) and are not counted separately."],
    "U5": ["Both Kvrocks configurations are shown so that neither blob threshold is cherry-picked."],
    "U6": ["Strict durability on both sides. The disk's fsync floor was measured right before each server's sweep "
           "(below), because it drifts.",
           "Writes per fsync: not observable. The INFO of neither server reports an fsync count (its sync_* fields are "
           "replication counters), so group-commit batch size cannot be read.",
           "A cell whose per-second ops vary by more than a quarter (coefficient of variation above 0.25) is flagged "
           "unstable and gets no verdict: the disk's fsync rate was not steady."],
    "U7": ["This is a process kill (SIGTERM/SIGKILL), not an OS crash or power loss.",
           "pong_ms and recovered_ms are measured from docker run returning; docker's own time is shown separately.",
           "The recovery threshold is 2x the warm pre-kill p99 of the same server.",
           "Acked counts are not throughput: how many writes were acknowledged before the signal depends on the "
           "server's speed and says nothing about durability."],
    "U8": ["TidesKV has no COMPACT, so the compaction figure exists for Kvrocks only. It was taken after U1's load and "
           "both closed-loop phases, so it is not a fresh-load figure.",
           "Time to quiet cannot be shorter than the settle check's window; a value at that floor is shown as \"≤ floor\" "
           "and gets no verdict.",
           "U8 reads U1's load and U3's aging; it runs no server of its own."],
}


def fmt(v, unit):
    if v is None or (isinstance(v, float) and not math.isfinite(v)):
        return "–"
    if unit == "bytes":
        for size, name in ((1 << 30, "GiB"), (1 << 20, "MiB"), (1 << 10, "KiB")):
            if v >= size:
                return "%.2f GiB" % (v / size) if name == "GiB" else "%.1f %s" % (v / size, name)
        return "%d B" % v
    return {"ops/s": "{:,.0f}", "ms": "{:.3f} ms", "count": "{:,.0f}", "s": "{:.1f} s", "%": "{:.1f}%"}.get(unit, "{:g}").format(v)


def fmt_cell(c, unit, floor=None):
    if c is None:
        return "n/a"
    if c["errors"]:
        return "ERR: " + "; ".join(c["errors"])[:110]
    if floor and c["median"] <= floor:
        return "≤ %g s (floor)" % floor
    if c["n"] > 1 and c["lo"] != c["hi"]:
        return "%s [%s–%s]" % (fmt(c["median"], unit), fmt(c["lo"], unit), fmt(c["hi"], unit))
    return fmt(c["median"], unit)


def headline(entries):
    """The headline metric of a case; when it was not run (U3's open-loop cells in a quick run) the first metric with a
    verdict slot, so the summary row still shows a number."""
    return next((e for e in entries if e["headline"]), None) or next((e for e in entries if not e["info"]), None)


def fmt_ratio(v):
    ratio = v["ratio"]
    if ratio is None:
        return "–"
    return "∞" if math.isinf(ratio) else "%.2f (band ±%.0f%%)" % (ratio, 100 * min(v["band"], 9.99))


def spark(values, top, width=60):
    """Text sparkline of values on a 0..top scale; longer series are cut into buckets and drawn at their max."""
    if not values or top <= 0:
        return ""
    step = max(1, math.ceil(len(values) / width))
    cut = [max(values[i:i + step]) for i in range(0, len(values), step)]
    return "".join(SPARK[min(len(SPARK) - 1, int(v / top * (len(SPARK) - 1) + 0.5))] for v in cut)


def vs_name(e, case):
    return NAMES.get(e["vs"], e["vs"] or "–")


def _kvrocks_has_errors(e):
    return any(c and c["errors"] for s, c in e["configs"].items() if s.startswith("kvrocks"))


def _vs_text(case_s, head):
    if not head or not head["vs"]:
        return "–"
    others = {e["vs"] for e in case_s["metrics"] if e["vs"] and e["verdict"] and not e["info"]} - {head["vs"]}
    text = NAMES.get(head["vs"], head["vs"])
    if any(_kvrocks_has_errors(e) for e in case_s["metrics"] if e["headline"]):
        text = "best valid Kvrocks config (%s)" % text
    return text + ("; other metrics: " + ", ".join(sorted(NAMES.get(o, o) for o in others)) if others else "")


def _reason(head):
    t = head["configs"].get("tideskv")
    ks = [(s, c) for s, c in head["configs"].items() if s.startswith("kvrocks") and c]
    vs = next(((s, c) for s, c in ks if s == head["vs"]), ks[0] if ks else None)
    if not t or not vs:
        return head["title"]
    return "%s: TidesKV %s vs %s %s" % (head["title"], fmt_cell(t, head["unit"]), NAMES[vs[0]], fmt_cell(vs[1], head["unit"]))


def _matrix(summary):
    out = ["| Case | Models | Verdict | Ratio median [range] | vs Kvrocks config | Reason |", "|---|---|---|---|---|---|"]
    for case, cs in summary["cases"].items():
        head = headline(cs["metrics"])
        rng = "–"
        if head and head["verdict"] and head["verdict"]["ratio"] is not None:
            r = head["verdict"]["ratio"]
            rng = "∞" if math.isinf(r) else "%.2f" % r
            if head["ratio_range"]:
                rng += " [%.2f–%.2f]" % tuple(head["ratio_range"])
        label = cs["label"] + (" (quick)" if summary["quick"] else "")
        out.append("| %s | %s | %s | %s | %s | %s |" % (case, INFO[case][1], label, rng, _vs_text(cs, head),
                                                         _reason(head) if head else "no data"))
    return out


def _item(case, e):
    v = e["verdict"]
    return "%s (%s), ratio %s, vs %s" % (e["title"], case, fmt_ratio(v), vs_name(e, case))


def f1_items(matrix):
    """Hook for F1: ({'tideskv_pros': [...], 'tideskv_cons': [...], 'kvrocks_pros': [...], 'kvrocks_cons': [...]}).
    Task 9 fills this from the parsed matrix."""
    return {}


def _pros_cons(summary):
    out = ["## Pros and cons", ""]
    if summary["quick"]:
        return out + ["No verdicts in a quick run, so no pros and cons are drawn; see the per-case tables.", ""]
    lists = {k: [] for k in ("tideskv_pros", "tideskv_cons", "kvrocks_pros", "kvrocks_cons", "even")}
    skipped = 0
    for case, cs in summary["cases"].items():
        for e in cs["metrics"]:
            v = e["verdict"]
            if not v or not (e["headline"] or e["pros_cons"]):
                continue
            if v["label"] == "TidesKV better":
                lists["tideskv_pros"].append("TidesKV: " + _item(case, e)); lists["kvrocks_cons"].append("TidesKV: " + _item(case, e))
            elif v["label"] == "Kvrocks better":
                lists["kvrocks_pros"].append("Kvrocks: " + _item(case, e)); lists["tideskv_cons"].append("Kvrocks: " + _item(case, e))
            elif v["label"] == "tie":
                lists["even"].append("%s (%s)" % (e["title"], case))
            else:
                skipped += 1
    for k, items in (f1_items(summary["f1"]) if summary["f1"] else {}).items():
        lists.setdefault(k, []).extend(items)
    for title, key in (("TidesKV pros", "tideskv_pros"), ("TidesKV cons", "tideskv_cons"), ("Kvrocks pros", "kvrocks_pros"),
                       ("Kvrocks cons", "kvrocks_cons"), ("Even (inside the noise band)", "even")):
        out += ["**%s**" % title, ""] + ["- " + x for x in lists[key]] + ([] if lists[key] else ["- none"]) + [""]
    if skipped:
        out += ["%d metric(s) have no verdict (errors, withheld or single repetition); they are in the per-case tables." % skipped, ""]
    return out


def _setup(case, cs, summary):
    st = cs["setup"]
    out = ["**Setup.** Durability level: %s. Repetitions: %s." % (st["level"], len(cs["reps"]))]
    w = st.get("workload")
    if w:
        out.append("Headline cell: ratio %s, %s B values, key pattern %s, keys up to %s, %s threads x %s clients, "
                   "pipeline %s, %s s%s." % (w["ratio"], w["data_size"], w["key_pattern"], w["key_maximum"], w["threads"],
                                             w["clients"], w["pipeline"], w["test_time"],
                                             ", rate limit %s/connection" % w["rate_limit"] if w["rate_limit"] else ""))
    mem = _mem_bytes(summary["meta"].get("server_mem"))
    if st["after_load"] and mem:
        parts = ["%s %.2f (%s)" % (NAMES[s], b / float(mem), fmt(b, "bytes")) for s, b in st["after_load"].items()]
        out.append("Dataset:mem after the load%s, from on-disk bytes excluding the WAL archive (not the logical size), "
                   "against SERVER_MEM %s: %s." % (" (the 16 KB phase)" if case == "U5" else "",
                                                   summary["meta"].get("server_mem"), "; ".join(parts)))
        if case not in ("U6", "U7") and any(b < mem for b in st["after_load"].values()):
            out.append("**Note: data fits in page cache on at least one side, so this case does not exercise the disk.**")
    out.append("")
    out += ["- %s: %s" % (NAMES[s], ", ".join("`%s`" % x for x in lines) or "no config read back")
            for s, lines in st["config"].items()]
    return out + [""]


def _mem_bytes(text):
    try:
        return int(text[:-1]) * {"k": 1 << 10, "m": 1 << 20, "g": 1 << 30}[text[-1].lower()]
    except (TypeError, ValueError, KeyError, IndexError):
        return None


def _results(cs):
    servers = cs["servers"]
    head = ["Metric"] + [NAMES[s] for s in servers] + ["Ratio", "Verdict"]
    out = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    for e in cs["metrics"]:
        v = e["verdict"]
        verdict = "info" if e["info"] else "–"
        if v:
            verdict = v["label"] + (" vs %s" % NAMES.get(e["vs"], e["vs"]) if len(servers) > 2 and e["vs"] else "")
            verdict += " (%s)" % v["note"] if v["note"] else ""
            if any(_kvrocks_has_errors(x) for x in (e,)) and e["vs"]:
                verdict += " [vs best valid Kvrocks config]"
        cells = [fmt_cell(e["configs"].get(s), e["unit"], e.get("floor")) for s in servers]
        out.append("| %s | %s | %s | %s |" % (e["title"] + (" †" if e["note"] else ""), " | ".join(cells),
                                              fmt_ratio(v) if v else "–", verdict))
    return out


def _notes(cs):
    out = []
    for e in cs["metrics"]:
        if e["note"]:
            out.append("- † %s: %s." % (e["title"], e["note"]))
    seen = set()
    for f in cs["flags"]:
        key = (f["server"], f["rep"], f["phase"], f["kind"])
        if key in seen:
            continue
        seen.add(key)
        d = f["detail"]
        text = ("client-bound: memtier used %.0f%% of the client CPUs, no verdict for this cell" % d if f["kind"] == "client_bound"
                else "unstable: ops/s per second varied with a coefficient of variation of %.2f (limit %.2f), no verdict for this cell"
                % (d, uc_cells.UNSTABLE_CV) if f["kind"] == "unstable"
                else "under target: achieved %s of %s ops/s offered, latency verdict withheld" % (fmt(d[0], "ops/s"), fmt(d[1], "ops/s")))
        out.append("- %s rep%d %s: %s." % (NAMES[f["server"]], f["rep"], f["phase"], text))
    for e in cs["metrics"]:
        for s, c in e["configs"].items():
            for err in (c["errors"] if c else ()):
                out.append("- %s, %s: %s" % (NAMES[s], e["title"], err))
    return out + [""] if out else out


def _timelines(case, cs):
    tl = cs["timelines"]
    if not tl:
        return []
    top = max(v for reps in tl.values() for vals in reps.values() for v in vals) or 1
    what = ("disk bytes (excluding WAL archive) at each du sample", "bytes") if case == "U2" else (
        "p99.9 per 10 s window (max of ten one-second points)", "ms")
    out = ["**Timeline: %s**, one scale from 0 to %s for all lines (data: `%s/timeline.csv`):" % (
        what[0], fmt(top, what[1]), case), ""]
    for s, reps in tl.items():
        for r, vals in reps.items():
            out.append("- %s rep%d `%s` first %s, last %s, max %s" % (NAMES[s], r, spark(vals, top), fmt(vals[0], what[1]),
                                                                     fmt(vals[-1], what[1]), fmt(max(vals), what[1])))
    return out + [""]


def _extras(case, cs, summary):
    ex, out = cs["extras"], []
    if case == "U2" and ex.get("end_state"):
        out += ["**End state** (live bytes is an estimate: keys left x value size, not a measurement):", "",
                "| Config | rep | keys at end | value size | live bytes (estimate) | disk at end | disk / live |", "|---|---|---|---|---|---|---|"]
        for s, reps in ex["end_state"].items():
            for r, st in reps.items():
                live = st["live_estimate"]
                db = st["dbsize"] if st["dbsize"] == "unverified" else "{:,}".format(st["dbsize"])
                out.append("| %s | %d | %s | %s | %s | %s | %s |" % (
                    NAMES[s], r, db, st["value_size"] or "unknown", fmt(live, "bytes") if live else "–", fmt(st["du_end"], "bytes"),
                    "%.1fx" % (st["du_end"] / live) if live else "–"))
        out.append("")
    if case == "U3" and ex.get("debt"):
        out += ["**Background debt** at the start and end of the closed run (information only; the two engines report "
                "different things):", ""]
        for s, reps in ex["debt"].items():
            for r, d in reps.items():
                out.append("- %s rep%d: %s" % (NAMES[s], r, ", ".join("%s %s -> %s" % (k, _num(a), _num(b)) for k, (a, b) in d.items())))
        out.append("")
    if case == "U7":
        out += ["Per-point rows (SIGTERM and SIGKILL at each write time) are information; the \"all points\" rows are "
                "the ones counted in the case label and the pros and cons. A server that never recovered shows ERR with "
                "the cause.", ""]
    if case == "U6":
        floors = [("%s rep%d" % (NAMES[s], r), fs) for s, reps in (ex.get("fio") or {}).items() for r, fs in reps.items()]
        start = (summary.get("fio") or {}).get("fsync")
        if start:
            floors.insert(0, ("start of run", start))
        if floors:
            out += ["**Disk floor** (fio, 4 KiB writes with an fsync each, queue depth 1), measured right before each "
                    "server's sweep:", ""]
            out += ["- %s: %.0f writes/s%s" % (who, fs["iops"], _fsync_lat(fs)) for who, fs in floors] + [""]
    return out


def _fsync_lat(fs):
    if not fs.get("sync_mean_ms"):
        return ""
    return ", fsync latency mean %.3f ms%s" % (fs["sync_mean_ms"], ", p99 %.3f ms" % fs["sync_p99_ms"] if fs.get("sync_p99_ms") else "")


def _num(v):
    return "–" if v is None else ("%g" % v if isinstance(v, float) else str(v))


def _case_section(case, cs, summary):
    title, _models, question = INFO[case]
    out = ["### %s: %s" % (case, title), "", "**Question.** " + question, ""]
    out += _setup(case, cs, summary)
    out += ["**Results** (median [min–max] over repetitions; ratio above 1 means TidesKV is better):", ""]
    out += _results(cs) + [""] + _notes(cs)
    out += _extras(case, cs, summary) + _timelines(case, cs)
    out += ["**Caveats.**", ""] + ["- " + n for n in OTHER_NOTES.get(case, [])]
    if case in ("U1", "U2", "U5", "U8"):
        out.append("- Disk numbers exclude Kvrocks' WAL archive.")
    return out + [""]


def render_f1(matrix):
    """Hook for F1: lines for the parsed F1/matrix.json. Task 9 replaces the fallback with the family table."""
    return ["```json", json.dumps(matrix, indent=2, sort_keys=True), "```"]


def render(summary):
    meta, quick = summary["meta"], summary["quick"]
    out = ["# TidesKV vs Kvrocks: use-case suite", ""]
    if quick:
        out += ["> **%s**" % QUICK_BANNER, ""]
    out += ["Run %s on %s. TidesKV %s against %s." % (meta.get("started_utc", "?"), meta.get("host", "?"),
                                                      meta.get("tideskv_sha", "?"), meta.get("kvrocks_image", "?")), ""]
    out += ["## Summary", ""] + _matrix(summary) + [""]
    out += _pros_cons(summary)
    out += ["## Per-case results", ""]
    for case, cs in summary["cases"].items():
        out += _case_section(case, cs, summary)
    out += ["## F1: command coverage and semantics", ""]
    out += (render_f1(summary["f1"]) if summary["f1"] else [F1_NOT_RUN]) + [""]
    out += ["## F2: operations facts", ""] + ([summary["f2"]] if summary["f2"] else [F2_NOT_RUN]) + [""]
    out += uc_report_method.methodology(summary)
    if summary["failed"]:
        out += ["## Failed cells", ""] + ["- " + x for x in summary["failed"]] + [""]
    return "\n".join(out)
