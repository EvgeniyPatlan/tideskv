"""Report for run.sh --cases (the Kvrocks common cases C1-C8; plan 2026-11-12 Task 6).

report.py calls write() when meta.json says mode "cases". Layout: see cases.sh.
"""
import json
import os

import report as base

SERVERS = base.SERVERS
LEVELS = base.LEVELS
DEFAULT_CASES = ("C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8")
C8_PARTS = ("c1", "c2", "c5")  # summed; the scan part is reported on its own
NOISE_BAND = (0.95, 1.05)
NOT_PASS = ("diverged", "diff did not run")


def _case_key(c):
    return int(c[1:]) if c[1:].isdigit() else 99


def diff_status(run_dir):
    """{case: status} from cases-diff.json; every default case is "diff did not run" when it cannot be read."""
    try:
        with open(os.path.join(run_dir, "cases-diff.json")) as f:
            d = json.load(f)
        if not isinstance(d, dict) or not d:
            raise ValueError("not a case map")
        return {str(c): str(s) for c, s in sorted(d.items(), key=lambda kv: _case_key(kv[0]))}
    except base.BAD_INPUT:
        return {c: "diff did not run" for c in DEFAULT_CASES}


def _status_ok(path):
    """Raises ValueError with the status text unless <path> holds "ok"."""
    if not os.path.exists(path):
        raise ValueError("no status")
    text = base._read(path) or "empty status"
    if text != "ok":
        raise ValueError(text)


def _cpu_per_op(base_path, count, duration_s):
    """Cgroup CPU delta per op, else the docker stats mean; and the stats' peak memory."""
    cpu, mem = None, None
    try:
        s = base.parse_stats(base_path + ".stats.jsonl")
        cpu = base.cpu_us_per_op(s["cpu_mean_pct"], duration_s, count)
        mem = s["mem_peak_bytes"]
    except base.BAD_INPUT:
        pass
    usec = base.read_cpu_usec(base_path + ".cpu")
    if usec is not None:
        cpu = usec / float(count)
    return cpu, mem


def _read_single(d, name):
    """One memtier run of a case C1-C7 (a repeat or the rate pass)."""
    _status_ok(os.path.join(d, name + ".status"))
    m = base.parse_memtier(os.path.join(d, name + ".json"))
    if m["count"] == 0:
        raise ValueError("memtier recorded no requests")
    m["cpu_us_per_op"], m["mem_peak_bytes"] = _cpu_per_op(os.path.join(d, name), m["count"], m["duration_s"])
    m["scan_p99"] = None
    return m


def _read_c8(d, name):
    """One C8 repeat: ops summed over c1, c2 and c5, the worst latency of them, the scan's p99 on its own."""
    _status_ok(os.path.join(d, name + ".status"))
    parts = {p: base.parse_memtier(os.path.join(d, "%s.%s.json" % (name, p))) for p in C8_PARTS + ("scan",)}
    main = [parts[p] for p in C8_PARTS]
    count = sum(m["count"] for m in main)
    if count == 0:
        raise ValueError("memtier recorded no requests")
    duration = max(m["duration_s"] for m in main)
    out = {"count": count, "ops": sum(m["ops"] for m in main), "duration_s": duration,
           "scan_p99": parts["scan"]["p99"]}
    for k in ("p50", "p99", "p999"):
        out[k] = max(m[k] for m in main)
    out["cpu_us_per_op"], out["mem_peak_bytes"] = _cpu_per_op(os.path.join(d, name), count, duration)
    return out


def _empty(server, level, case, status):
    return {"server": server, "level": level, "case": case, "status": status, "ops": None, "p50": None,
            "p99": None, "p999": None, "scan_p99": None, "cpu_us_per_op": None, "mem_peak_bytes": None,
            "du_bytes": None, "runs": 0, "rate_ops": None, "rate_p99": None, "rate_cpu_us_per_op": None,
            "error": None, "extra_errors": []}


def _attempt(read, extra_errors, label, *args):
    try:
        return read(*args)
    except base.BAD_INPUT as e:
        extra_errors.append("%s: %s" % (label, e))
        return None


def _fill_runs(cell, d, repeats):
    read = _read_c8 if cell["case"] == "C8" else _read_single
    errs, runs = [], []
    for k in range(1, repeats + 1):
        r = _attempt(read, errs, "r%d" % k, d, "%s.r%d" % (cell["case"], k))
        if r is not None:
            runs.append(r)
    if errs or not runs:
        cell["error"] = "; ".join(errs) or "no repeats"
        return
    med = sorted(runs, key=lambda r: r["ops"])[len(runs) // 2]
    cell.update({k: med[k] for k in ("ops", "p50", "p99", "p999", "scan_p99", "cpu_us_per_op", "mem_peak_bytes")})
    cell["runs"] = len(runs)


def _fill_rate_and_du(cell, d):
    c = cell["case"]
    if c != "C8":
        errs = []
        r = _attempt(_read_single, errs, "rate pass", d, c + ".rate")
        if r is not None:
            cell.update({"rate_ops": r["ops"], "rate_p99": r["p99"], "rate_cpu_us_per_op": r["cpu_us_per_op"]})
        cell["extra_errors"] += errs
    try:
        cell["du_bytes"] = int(base._read(os.path.join(d, c + ".du")))
    except base.BAD_INPUT as e:
        cell["extra_errors"].append("data dir size (%s.du): %s" % (c, e))


def _cell(run_dir, server, level, case, status, repeats):
    cell = _empty(server, level, case, status)
    if status in NOT_PASS:
        return cell
    d = os.path.join(run_dir, server, level)
    if not os.path.isdir(d):
        reason = base.failure_reason(run_dir, server)
        cell["error"] = "not run: " + reason if reason else "not run"
        return cell
    _fill_runs(cell, d, repeats)
    _fill_rate_and_du(cell, d)
    return cell


def _repeats(meta):
    try:
        return max(1, int(base._num(meta["case_repeats"])))
    except base.BAD_INPUT:
        return 1


def collect(run_dir, meta):
    status = diff_status(run_dir)
    repeats = _repeats(meta)
    return [_cell(run_dir, s, lv, c, st, repeats) for lv in LEVELS for c, st in status.items() for s in SERVERS]


def load_expiry(run_dir):
    """{server: expiry.json dict, or {"error": reason}}."""
    out = {}
    for s in SERVERS:
        try:
            with open(os.path.join(run_dir, s, "expiry.json")) as f:
                d = json.load(f)
            for k in ("keys_loaded", "keys_live_expected", "keys_other", "dbsize_after", "bytes_before",
                      "bytes_after", "waited_s"):
                base._num(d[k])
            out[s] = d
        except base.BAD_INPUT as e:
            reason = base.failure_reason(run_dir, s)
            out[s] = {"error": "not run: " + reason if reason else "unreadable expiry.json: %s" % e}
    return out


# ---- rendering ----

def _ratio(t, k):
    if t["ops"] is None or k["ops"] is None or not k["ops"]:
        return "–"
    r = t["ops"] / k["ops"]
    return "≈" if NOISE_BAND[0] <= r <= NOISE_BAND[1] else "%.2f" % r


def _txt(c, v, fmt):
    """The cell's value, its status text when the case did not pass, ERR when its files were bad."""
    if c["status"] != "pass":
        return c["status"]
    if c["error"] is not None:
        return "ERR"
    return "–" if v is None else fmt % v


def _lat(c):
    if c["status"] != "pass" or c["error"] is not None or c["p50"] is None:
        return _txt(c, None, "")
    return "%.3f / %.3f / %.3f" % (c["p50"], c["p99"], c["p999"])


def _ok_err(c, v, fmt):
    """Like _txt, but a rate-pass or du problem shows ERR even when the repeats were fine."""
    if c["status"] == "pass" and c["error"] is None and v is None:
        return "ERR"
    return _txt(c, v, fmt)


def _runs(t, k):
    return "–" if t["status"] != "pass" else "%d / %d" % (t["runs"], k["runs"])


def _case_order(cases):
    return sorted(cases, key=lambda c: (c != "C8", _case_key(c)))


def _throughput(by, cases, lv):
    out = ["| Case | TidesKV ops/s | Kvrocks ops/s | ratio | TidesKV p50/p99/p999 ms | Kvrocks p50/p99/p999 ms | runs |",
           "|---|---|---|---|---|---|---|"]
    for c in _case_order(cases):
        t, k = by[("tideskv", lv, c)], by[("kvrocks", lv, c)]
        out.append("| %s | %s | %s | %s | %s | %s | %s |" % (
            c, _txt(t, t["ops"] and round(t["ops"]), "%d"), _txt(k, k["ops"] and round(k["ops"]), "%d"),
            _ratio(t, k), _lat(t), _lat(k), _runs(t, k)))
    return out


def _scan_line(by, cases, lv):
    if "C8" not in cases:
        return []
    t, k = by[("tideskv", lv, "C8")], by[("kvrocks", lv, "C8")]
    if t["scan_p99"] is None and k["scan_p99"] is None:
        return []
    f = lambda c: "–" if c["scan_p99"] is None else "%.3f" % c["scan_p99"]
    return ["", "C8 runs C1, C2 and C5 next to a rate-limited SCAN: ops/s is the sum of the three, each latency the "
            "worst of the three. SCAN p99 ms: TidesKV %s, Kvrocks %s." % (f(t), f(k))]


def _rate_table(meta, by, cases, lv):
    target = meta.get("case_rate", "?")
    out = ["| Case | target ops/s | TidesKV achieved ops/s | Kvrocks achieved ops/s | TidesKV p99 ms "
           "| Kvrocks p99 ms | TidesKV CPU µs/op | Kvrocks CPU µs/op |", "|---|---|---|---|---|---|---|---|"]
    for c in _case_order(cases):
        if c == "C8":
            continue
        t, k = by[("tideskv", lv, c)], by[("kvrocks", lv, c)]
        out.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
            c, target, _ok_err(t, t["rate_ops"] and round(t["rate_ops"]), "%d"),
            _ok_err(k, k["rate_ops"] and round(k["rate_ops"]), "%d"), _ok_err(t, t["rate_p99"], "%.3f"),
            _ok_err(k, k["rate_p99"], "%.3f"), _ok_err(t, t["rate_cpu_us_per_op"], "%.1f"),
            _ok_err(k, k["rate_cpu_us_per_op"], "%.1f")))
    return out


def _resources(by, cases, lv):
    out = ["| Case | TidesKV peak mem | Kvrocks peak mem | TidesKV disk after | Kvrocks disk after |",
           "|---|---|---|---|---|"]
    for c in _case_order(cases):
        t, k = by[("tideskv", lv, c)], by[("kvrocks", lv, c)]
        out.append("| %s | %s | %s | %s | %s |" % (
            c, _txt(t, t["mem_peak_bytes"], "%d") if t["mem_peak_bytes"] is None else base._gib(t["mem_peak_bytes"]),
            _txt(k, k["mem_peak_bytes"], "%d") if k["mem_peak_bytes"] is None else base._gib(k["mem_peak_bytes"]),
            _ok_err(t, t["du_bytes"], "%d") if t["du_bytes"] is None else base._gib(t["du_bytes"]),
            _ok_err(k, k["du_bytes"], "%d") if k["du_bytes"] is None else base._gib(k["du_bytes"])))
    return out


def _expiry_table(expiry):
    out = ["| Server | keys loaded (C1) | live expected (C1) | other keys | expected DBSIZE | DBSIZE after expiry "
           "| excess | bytes before | bytes after | bytes freed | waited s |",
           "|---|---|---|---|---|---|---|---|---|---|---|"]
    notes = []
    for s in SERVERS:
        e = expiry.get(s) or {"error": "no expiry result"}
        if "error" in e:
            out.append("| %s | ERR | – | – | – | – | – | – | – | – | – |" % base.NAMES[s])
            notes.append("- %s expiry: %s" % (base.NAMES[s], e["error"]))
            continue
        want = e["keys_live_expected"] + e["keys_other"]
        out.append("| %s | %d | %d | %d | %d | %d | %d | %s | %s | %s | %d |" % (
            base.NAMES[s], e["keys_loaded"], e["keys_live_expected"], e["keys_other"], want, e["dbsize_after"],
            e["dbsize_after"] - want, base._gib(e["bytes_before"]), base._gib(e["bytes_after"]),
            base._gib(e["bytes_before"] - e["bytes_after"]), e["waited_s"]))
    return out, notes


def _titles(meta):
    relaxed = meta.get("relaxed_mode", "none")
    return {"relaxed": "Relaxed durability (TidesKV `sync-mode %s` vs Kvrocks `rocksdb.write_options.sync no`)" % relaxed,
            "strict": "Strict durability (TidesKV `sync-mode full` vs Kvrocks `rocksdb.write_options.sync yes`)"}


def _level_sections(meta, by, cases):
    out = []
    for lv, title in _titles(meta).items():
        out += ["## " + title, "", "### Throughput and latency (median of %s repeats by ops/s)" % meta.get(
            "case_repeats", "?"), ""] + _throughput(by, cases, lv) + _scan_line(by, cases, lv)
        out += ["", "### Fixed rate (%s ops/s target)" % meta.get("case_rate", "?"), ""]
        out += _rate_table(meta, by, cases, lv)
        out += ["", "### Resources", ""] + _resources(by, cases, lv) + [""]
    return out


def _error_notes(cells):
    notes = []
    for c in cells:
        label = "%s %s %s" % (base.NAMES[c["server"]], c["level"], c["case"])
        if c["error"] is not None:
            notes.append("- %s: %s" % (label, c["error"]))
        notes += ["- %s: %s" % (label, e) for e in c["extra_errors"]]
    return notes


def _failed_runs(run_dir):
    failed = [s for s in SERVERS if run_dir and base.failure_reason(run_dir, s)]
    out = ["## Failed runs", ""] if failed else []
    for s in failed:
        out += ["### " + base.NAMES[s], "", base.failure_reason(run_dir, s), "", "Last 20 lines of server.log:", "",
                "```", base.log_tail(run_dir, s), "```", ""]
    return out


def render(meta, cells, expiry, disks, configs, run_dir=None):
    by = {(c["server"], c["level"], c["case"]): c for c in cells}
    cases = sorted({c["case"] for c in cells}, key=_case_key)
    status = {c["case"]: c["status"] for c in cells}
    out = base.header_lines(meta, "# TidesKV vs Kvrocks: common cases C1-C8")
    out += ["%s repeats per case, then one fixed-rate pass at %s ops/s (the achieved rate is what is shown). A ratio "
            "above 1 means TidesKV is ahead on throughput; `≈` means within %.2f-%.2f." % (
                meta.get("case_repeats", "?"), meta.get("case_rate", "?"), *NOISE_BAND), ""]
    out += ["## Correctness", "", "The differential test (TidesKV against Kvrocks, `cases-diff.log`) gates the timed "
            "runs; a case that did not pass shows its status instead of numbers.", "", "| Case | diff status |",
            "|---|---|"] + ["| %s | %s |" % (c, status[c]) for c in cases] + [""]
    out += _level_sections(meta, by, cases)
    table, exp_notes = _expiry_table(expiry)
    out += ["## Expiry", "", "C1's keys carry a TTL. After it passed and the server settled, DBSIZE should be live "
            "expected plus the other datasets' keys; excess is the keys still counted that should have expired. "
            "Bytes freed is the data dir's size before minus after the wait.", ""] + table + [""]
    notes = _error_notes(cells) + exp_notes
    if notes:
        out += ["Errors:"] + notes + [""]
    out += _failed_runs(run_dir)
    out += ["## Disk and load", "", "| Server | logical | after load | end | space amp (end/logical) | load time "
            "| settle after load | settle after restart |", "|---|---|---|---|---|---|---|---|"]
    out += [base._disk_row(s, disks) for s in SERVERS] + [""]
    return "\n".join(out + base.fairness_lines(meta, configs)) + "\n"


def failed(cells, expiry):
    return (any(c["status"] != "pass" or c["error"] is not None or c["extra_errors"] for c in cells)
            or any("error" in (e or {"error": 1}) for e in expiry.values()))


def write(run_dir, meta, disks, configs):
    cells, expiry = collect(run_dir, meta), load_expiry(run_dir)
    with open(os.path.join(run_dir, "report.md"), "w") as f:
        f.write(render(meta, cells, expiry, disks, configs, run_dir))
    with open(os.path.join(run_dir, "summary.json"), "w") as f:
        json.dump({"meta": meta, "cells": cells, "expiry": expiry, "disks": disks}, f, indent=2)
    return 1 if failed(cells, expiry) else 0
