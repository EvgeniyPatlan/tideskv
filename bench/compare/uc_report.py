#!/usr/bin/env python3
"""Report of the use-case suite (stdlib only): uc_report.py RUN_DIR writes RUN_DIR/report.md and summary.json.

build(run_dir) reads the run directory (layout in uc_cases_run.sh's header) into a summary dict; render(summary) turns it
into markdown (uc_report_text.py). One verdict per metric, from uc_verdict, against the best valid Kvrocks config for
that metric. Exit status 1 when any cell failed.
"""
import json
import math
import os
import re
import sys

import uc_cells
import uc_verdict
from uc_cells import CellError, NotRun
from uc_metrics import metrics_for
from uc_report_text import headline, CASES, F1_NOT_RUN, F2_NOT_RUN, NAMES, render  # noqa: F401  (render is re-exported)

HERE = os.path.dirname(os.path.abspath(__file__))
SERVERS = {"U2": ("tideskv", "kvrocks", "kvrocks-cron"), "U5": ("tideskv", "kvrocks", "kvrocks-blob")}
DEFAULT_SERVERS = ("tideskv", "kvrocks")
STRICT_CASES = ("U6", "U7")
LATENCY_UNIT = "ms"


def _rep_numbers(case_dir):
    try:
        return sorted(int(n[3:]) for n in os.listdir(case_dir) if re.fullmatch(r"rep\d+", n))
    except OSError:
        return []


def rep_status(rep_dir, phase):
    """'ok' or the reason a repetition did not finish. A two-pass case's closed phases are judged by closed.done (pass
    A), everything else (open phases, the repetition as a whole) by done, which pass B writes last."""
    names = ("closed.done", "done") if phase is not None and not phase.startswith("open") else ("done",)
    for name in names:
        try:
            return uc_cells.read_text(os.path.join(rep_dir, name)) or "empty done file"
        except OSError:
            continue
    return "not finished (no done file)"


def _collect(metric, servers, reps, case_dir):
    """({server: Summary or None (metric not on this side)}, whether the phase ran anywhere, CellError messages)."""
    out, ran, cell_errors = {}, False, []
    for s in servers:
        if metric.only and not s.startswith(metric.only):
            out[s] = None
            continue
        values, errors = [], []
        for r in reps:
            rep_dir = os.path.join(case_dir, s, "rep%d" % r)
            status = rep_status(rep_dir, metric.phase) if os.path.isdir(rep_dir) else "not run"
            if status != "ok":
                values.append(None); errors.append("rep%d: %s" % (r, status))
                continue
            try:
                values.append(metric.extract(rep_dir)); ran = True
            except NotRun:
                values.append(None); errors.append("rep%d: not run" % r)
            except CellError as e:
                values.append(None); errors.append("rep%d: %s" % (r, e)); ran = True
                cell_errors.append("%s rep%d %s: %s" % (NAMES[s], r, metric.title, e))
        out[s] = uc_verdict.summarize(values, errors)
    return out, ran, cell_errors


def _ratio_range(t, k, hib):
    """Worst-case and best-case ratio from the two sides' min-max, oriented like uc_verdict.ratio; None if unknowable."""
    if t.errors or k.errors:
        return None
    num, den = (t, k) if hib else (k, t)
    if den.lo == 0 or den.hi == 0:
        return None
    return (num.lo / den.hi, num.hi / den.lo)


def _pick_k(ks, hib):
    valid = [n for n, s in ks.items() if not s.errors and s.n > 0]
    if not valid:
        return None
    return uc_verdict.best_kvrocks(ks, hib)


def _withhold(metric, flags):
    """Why this metric's verdict is not given, from the cell flags of its phase."""
    if not metric.phase:
        return None
    mine = [f for f in flags if f["phase"] == metric.phase]
    if any(f["kind"] == "unstable" for f in mine):
        return "unstable: ops/s varied between seconds (CV above %.2f), the disk was not steady" % uc_cells.UNSTABLE_CV
    if any(f["kind"] == "client_bound" for f in mine):
        return "client-bound: memtier used over %d%% of the client CPUs" % uc_cells.CLIENT_BOUND_PCT
    if metric.unit == LATENCY_UNIT and any(f["kind"] == "under_target" for f in mine):
        return "under target: a server did not reach 95% of the offered rate, latency is not comparable"
    return None


def _decide(metric, sums, flags, quick, floor=None):
    t = sums.get("tideskv")
    ks = {n: s for n, s in sums.items() if n.startswith("kvrocks") and s is not None}
    if metric.info or t is None or not ks:
        return None, None, None
    best = _pick_k(ks, metric.higher_is_better)
    k = ks[best or next(iter(ks))]
    v = uc_verdict.verdict(t, k, metric.higher_is_better, quick)
    why = _withhold(metric, flags) or ("time to quiet is at the measurement floor of %g s" % floor if floor else None)
    if why and v.label != uc_verdict.LABEL_NO_VERDICT:
        v = uc_verdict.Verdict(uc_verdict.LABEL_NO_VERDICT, v.ratio, v.band, why)
    elif why and not v.note:
        v = uc_verdict.Verdict(v.label, v.ratio, v.band, why)
    return v, best, _ratio_range(t, k, metric.higher_is_better)


def _flags(case_dir, servers, reps, phases, client_cpus):
    out = []
    for s in servers:
        for r in reps:
            rep_dir = os.path.join(case_dir, s, "rep%d" % r)
            for ph in phases:
                for kind, detail in uc_cells.cell_flags(rep_dir, ph, client_cpus).items():
                    out.append({"server": s, "rep": r, "phase": ph, "kind": kind, "detail": detail})
    return out


def _at_floor(metric, sums, meta):
    """The measurement floor when any valid value of this metric is at or under it, else None."""
    floor = meta.get(metric.floor_key) if metric.floor_key else None
    valid = [x for x in sums.values() if x and not x.errors and x.n]
    return floor if floor and any(x.median <= floor for x in valid) else None


def _basis(metric, servers, reps, case_dir):
    """'allocated' when every du source of the metric has allocated bytes, 'apparent size' when none has, else
    'mixed: apparent size for <which>'."""
    got = {}
    for s in servers:
        for r in reps:
            b = uc_cells.disk_basis(os.path.join(case_dir, s, "rep%d" % r), metric.disk_src)
            if b:
                got["%s rep%d" % (NAMES[s], r)] = b
    old = [k for k, b in got.items() if b != "allocated"]
    if not old:
        return "allocated"
    return "apparent size" if len(old) == len(got) else "mixed: apparent size for " + ", ".join(old)


def _metric_entry(metric, sums, verdict, best, rng, flags, title, floor):
    why = _withhold(metric, flags)
    return {"key": metric.key, "title": title, "floor": floor, "unit": metric.unit, "higher_is_better": metric.higher_is_better,
            "headline": metric.headline, "info": metric.info, "pros_cons": metric.pros_cons, "note": metric.note,
            "phase": metric.phase, "withheld": why,
            "configs": {s: None if x is None else {"median": x.median, "lo": x.lo, "hi": x.hi, "n": x.n,
                                                   "errors": list(x.errors)} for s, x in sums.items()},
            "verdict": None if verdict is None else {"label": verdict.label, "ratio": verdict.ratio, "band": verdict.band,
                                                     "note": verdict.note},
            "vs": best, "ratio_range": rng}


def _counted(entries):
    return [e for e in entries if e["verdict"] and (e["headline"] or e["pros_cons"])]


def _case_label(entries):
    verdicts = [uc_verdict.Verdict(e["verdict"]["label"], e["verdict"]["ratio"], e["verdict"]["band"], "")
                for e in sorted(_counted(entries), key=lambda e: not e["headline"])]
    return uc_verdict.case_label(verdicts)


def _config_block(run_dir, server, level):
    """Lines of the last config.txt block written under the case's durability level (blocks start with '# <level>')."""
    try:
        text = uc_cells.read_text(os.path.join(run_dir, server, "config.txt"))
    except OSError:
        return []
    blocks, cur = [], None
    for line in text.splitlines():
        if line.startswith("# "):
            cur = [line[2:].strip(), []]
            blocks.append(cur)
        elif cur is not None and line.strip():
            cur[1].append(line.strip())
    chosen = [b for b in blocks if b[0] == level] or blocks
    return chosen[-1][1] if chosen else []


def _size_bytes(text):
    m = re.fullmatch(r"(\d+)([kmgKMG]?)", str(text).strip())
    return int(m.group(1)) * {"": 1, "k": 1 << 10, "m": 1 << 20, "g": 1 << 30}[m.group(2).lower()] if m else None


def _setup(case, run_dir, case_dir, servers, rep1, headline_phase):
    """Per server: config lines, disk after load (excl. WAL archive) and the workload line of the headline cell."""
    level = "strict" if case in STRICT_CASES else "relaxed"
    out = {"level": level, "config": {}, "after_load": {}, "workload": None}
    for s in servers:
        out["config"][s] = _config_block(run_dir, s, level)
        rep_dir = os.path.join(case_dir, s, "rep%d" % rep1)
        names = ["du-after-load-v16k"] if case == "U5" else ["du-after-load"]
        for n in names:
            try:
                out["after_load"][s] = uc_cells.du_rows(os.path.join(rep_dir, n))[-1][1]
            except (OSError, CellError):
                pass
    if headline_phase:
        try:
            cfg = json.loads(uc_cells.read_text(os.path.join(case_dir, "tideskv", "rep%d" % rep1, headline_phase + ".json")))
            out["workload"] = {k: cfg["configuration"].get(k) for k in (
                "ratio", "data_size", "key_pattern", "key_maximum", "threads", "clients", "pipeline", "test_time",
                "rate_limit", "expiry_range")}
        except (OSError, ValueError, KeyError):
            pass
    return out


def _timelines(case, case_dir, servers, reps):
    """U2: disk bytes (excl. WAL archive) per du.csv row; U3: p99.9 per 10 s window. {server: {rep: [values]}}."""
    out = {}
    for s in servers:
        for r in reps:
            rep_dir = os.path.join(case_dir, s, "rep%d" % r)
            try:
                if case == "U2":
                    vals = [row[1] for row in uc_cells.du_rows(os.path.join(rep_dir, "du.csv"))]
                elif case == "U3":
                    vals = uc_cells.p999_windows(rep_dir, "closed")[2]
                else:
                    continue
            except (OSError, CellError, NotRun):
                continue
            out.setdefault(s, {})[r] = vals
    return out


def _extras(case, case_dir, servers, reps):
    out = {}
    for s in servers:
        for r in reps:
            rep_dir = os.path.join(case_dir, s, "rep%d" % r)
            try:
                if case == "U2":
                    state = uc_cells.u2_end_state(rep_dir)
                    last = uc_cells.du_rows(os.path.join(rep_dir, "du-end"))[-1]
                    state["du_end"], state["basis"] = last[1], last[5]
                    out.setdefault("end_state", {}).setdefault(s, {})[r] = state
                elif case == "U6":
                    try:
                        res = uc_cells.fio_job(os.path.join(rep_dir, "fio", "fsync.json"), "write")
                    except (OSError, ValueError, KeyError, IndexError, TypeError):
                        res = {"missing": uc_cells.fio_status(os.path.join(rep_dir, "fio"))}
                    out.setdefault("fio", {}).setdefault(s, {})[r] = res
                elif case == "U3":
                    kind = "tideskv" if s == "tideskv" else "kvrocks"
                    out.setdefault("debt", {}).setdefault(s, {})[r] = uc_cells.debt_values(rep_dir, "closed", kind)
            except (OSError, ValueError, KeyError, IndexError, TypeError, CellError, NotRun):
                continue
    return out


def _build_case(case, run_dir, meta, quick):
    case_dir = os.path.join(run_dir, case)
    candidates = SERVERS.get(case, DEFAULT_SERVERS)
    servers = [s for s in candidates if os.path.isdir(os.path.join(case_dir, s))]
    reps = sorted({r for s in servers for r in _rep_numbers(os.path.join(case_dir, s))})
    metrics = metrics_for(case, run_dir)
    phases = sorted({m.phase for m in metrics if m.phase})
    flags = _flags(case_dir, servers, reps, phases, meta.get("client_cpus"))
    entries, failed, bases = [], [], {}
    for s in servers:
        for r in reps:
            st = rep_status(os.path.join(case_dir, s, "rep%d" % r), None)
            if st != "ok":
                failed.append("%s %s rep%d: %s" % (case, NAMES[s], r, st))
    for m in metrics:
        sums, ran, cell_errors = _collect(m, servers, reps, case_dir)
        failed += ["%s %s" % (case, e) for e in cell_errors]
        if not ran:
            continue
        title, floor = m.title, _at_floor(m, sums, meta)
        if m.disk_src:
            bases[m.key] = _basis(m, servers, reps, case_dir)
            title = title.replace("{basis}", bases[m.key])
        elif m.twin_of and bases.get(m.twin_of) == "apparent size":
            continue  # the main figure is apparent size already
        v, best, rng = _decide(m, sums, flags, quick, floor)
        entries.append(_metric_entry(m, sums, v, best, rng, flags, title, floor))
    head = headline(entries)
    return {"servers": servers, "reps": reps, "metrics": entries, "flags": flags, "failed": sorted(set(failed)),
            "label": _case_label(entries) if entries else uc_verdict.LABEL_NO_VERDICT,
            "setup": _setup(case, run_dir, case_dir, servers, reps[0] if reps else 1, head["phase"] if head else None),
            "timelines": _timelines(case, case_dir, servers, reps), "extras": _extras(case, case_dir, servers, reps)}


def _read_meta(run_dir):
    try:
        return json.loads(uc_cells.read_text(os.path.join(run_dir, "meta.json")))
    except (OSError, ValueError):
        return {}


def _f2_path(run_dir):
    for p in (os.path.join(run_dir, "f2_operations.md"), os.path.join(HERE, "f2_operations.md")):
        if os.path.exists(p):
            return p
    return None


def build(run_dir):
    """The summary dict (summary.json content) of a run directory."""
    meta = _read_meta(run_dir)
    quick = meta.get("mode") == "quick"
    cases = {c: _build_case(c, run_dir, meta, quick) for c in CASES if os.path.isdir(os.path.join(run_dir, c))
             and c != "F1"}
    f1 = None
    try:
        f1 = json.loads(uc_cells.read_text(os.path.join(run_dir, "F1", "matrix.json")))
    except (OSError, ValueError):
        pass
    f2 = _f2_path(run_dir)
    return {"run_dir": run_dir, "meta": meta, "quick": quick, "cases": cases, "f1": f1,
            "f2": uc_cells.read_text(f2) if f2 else None,
            "fio": uc_cells.fio_baselines(run_dir),
            "failed": [x for c in cases.values() for x in c["failed"]]}


def _clean(o):
    if isinstance(o, float) and not math.isfinite(o):
        return None
    if isinstance(o, dict):
        return {str(k): _clean(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [_clean(v) for v in o]
    return o


def write_timeline_csvs(summary, run_dir):
    for case, unit in (("U2", "bytes"), ("U3", "p999_ms")):
        tl = summary["cases"].get(case, {}).get("timelines")
        if not tl:
            continue
        with open(os.path.join(run_dir, case, "timeline.csv"), "w") as f:
            f.write("server,rep,index,%s\n" % unit)
            for s, reps in tl.items():
                for r, vals in reps.items():
                    f.writelines("%s,%s,%d,%s\n" % (s, r, i, v) for i, v in enumerate(vals))


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: uc_report.py <run_dir>\n")
        return 2
    run_dir = argv[1]
    summary = build(run_dir)
    write_timeline_csvs(summary, run_dir)
    with open(os.path.join(run_dir, "report.md"), "w") as f:
        f.write(render(summary))
    with open(os.path.join(run_dir, "summary.json"), "w") as f:
        json.dump(_clean(summary), f, indent=2)
    return 1 if summary["failed"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
