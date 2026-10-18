#!/usr/bin/env python3
"""Turns a bench/compare run directory into report.md and summary.json (spec 2026-10-18 §7).

Usage: report.py <run_dir>    exit 0 when every cell is ok, 1 otherwise.
"""
import json
import math
import os
import re
import sys

SERVERS = ("tideskv", "kvrocks")
LEVELS = ("relaxed", "strict")
WORKLOADS = ("W1", "W2", "W3", "W4", "W5", "W6")
NAMES = {"tideskv": "TidesKV", "kvrocks": "Kvrocks"}
BAD_INPUT = (OSError, ValueError, KeyError, TypeError, AttributeError, OverflowError, ZeroDivisionError)
UNITS = {"B": 1, "KiB": 1 << 10, "MiB": 1 << 20, "GiB": 1 << 30, "TiB": 1 << 40,
         "kB": 1000, "MB": 1000 ** 2, "GB": 1000 ** 3}


def _num(v):
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v):
        raise ValueError("not a finite number: %r" % (v,))
    return v


def parse_memtier(path):
    with open(path) as f:
        stats = json.load(f)["ALL STATS"]
    totals = stats["Totals"]
    pct = totals["Percentile Latencies"]
    return {"count": _num(totals["Count"]), "ops": _num(totals["Ops/sec"]), "p50": _num(pct["p50.00"]),
            "p99": _num(pct["p99.00"]), "p999": _num(pct["p99.90"]),
            "duration_s": _num(stats["Runtime"]["Total duration"]) / 1000.0}


def parse_mem(text):
    used = text.split("/")[0].strip()
    m = re.fullmatch(r"([0-9.]+)\s*([A-Za-z]+)", used)
    if not m or m.group(2) not in UNITS:
        raise ValueError("unparsable memory usage: " + text)
    return int(float(m.group(1)) * UNITS[m.group(2)])


def parse_stats(path):
    cpus, mems = [], []
    with open(path) as f:
        for line in f:
            if not line.strip():
                continue
            try:  # a truncated line or a "--" sample is skipped, the rest still count
                s = json.loads(line)
                cpu = float(s["CPUPerc"].rstrip("%"))
                mem = parse_mem(s["MemUsage"])
            except BAD_INPUT:
                continue
            if not (math.isfinite(cpu) and math.isfinite(mem)):
                continue
            cpus.append(cpu)
            mems.append(mem)
    return {"cpu_mean_pct": sum(cpus) / len(cpus) if cpus else None,
            "mem_peak_bytes": max(mems) if mems else None}


def cpu_us_per_op(cpu_mean_pct, duration_s, count):
    if not count or cpu_mean_pct is None:
        return None
    return cpu_mean_pct / 100.0 * duration_s * 1e6 / count


def _read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().strip()


def failure_reason(run_dir, server):
    """First line of <server>/run.failed, or None when the server's run did not fail."""
    try:
        text = _read(os.path.join(run_dir, server, "run.failed"))
    except BAD_INPUT:
        return None
    return (text.splitlines() or ["failed (no reason recorded)"])[0]


def log_tail(run_dir, server, n=20):
    try:
        return "\n".join(_read(os.path.join(run_dir, server, "server.log")).splitlines()[-n:])
    except BAD_INPUT:
        return "(no server.log)"


def _cell(run_dir, server, level, w):
    cell = {"server": server, "level": level, "workload": w, "ops": None, "p50": None, "p99": None,
            "p999": None, "cpu_us_per_op": None, "mem_peak_bytes": None, "del_ops": None, "error": None}
    d = os.path.join(run_dir, server, level)
    if not os.path.isdir(d):
        reason = failure_reason(run_dir, server)
        cell["error"] = "not run: " + reason if reason else "not run"
        return cell
    status_path = os.path.join(d, w + ".status")
    try:
        status = (_read(status_path) or "empty status") if os.path.exists(status_path) else "no status"
    except BAD_INPUT as e:
        status = "unreadable status: " + str(e)
    if status != "ok":
        cell["error"] = status
        return cell
    try:
        m = parse_memtier(os.path.join(d, w + ".json"))
    except BAD_INPUT as e:
        cell["error"] = "unreadable memtier result: " + str(e)
        return cell
    if m["count"] == 0:
        cell["error"] = "memtier recorded no requests"
        return cell
    cell.update({k: m[k] for k in ("ops", "p50", "p99", "p999")})
    stats_path = os.path.join(d, w + ".stats.jsonl")
    if os.path.exists(stats_path):
        try:
            s = parse_stats(stats_path)
            cell["cpu_us_per_op"] = cpu_us_per_op(s["cpu_mean_pct"], m["duration_s"], m["count"])
            cell["mem_peak_bytes"] = s["mem_peak_bytes"]
        except BAD_INPUT:
            pass
    if w == "W6" and os.path.exists(os.path.join(d, "W6-del.json")):
        try:
            cell["del_ops"] = parse_memtier(os.path.join(d, "W6-del.json"))["ops"]
        except BAD_INPUT:
            pass
    return cell


def collect(run_dir):
    return [_cell(run_dir, s, lv, w) for lv in LEVELS for w in WORKLOADS for s in SERVERS]


def load(run_dir):
    try:
        with open(os.path.join(run_dir, "meta.json")) as f:
            meta = json.load(f)
        if not isinstance(meta, dict):
            meta = {}
    except BAD_INPUT:
        meta = {}
    disks, configs = {}, {}
    for s in SERVERS:
        p = os.path.join(run_dir, s, "disk.json")
        if os.path.exists(p):
            try:
                with open(p) as f:
                    disks[s] = json.load(f)
            except BAD_INPUT:
                disks[s] = None
        p = os.path.join(run_dir, s, "config.txt")
        if os.path.exists(p):
            try:
                configs[s] = _read(p)
            except BAD_INPUT:
                pass
    return meta, collect(run_dir), disks, configs


def _lat(c):
    return "ERR" if c["error"] is not None else "%.3f / %.3f / %.3f" % (c["p50"], c["p99"], c["p999"])


def _ops(c):
    return "ERR" if c["error"] is not None else "%d" % round(c["ops"])


def _gib(n):
    return "%.2f GiB" % (n / float(1 << 30))


def _disk_row(s, disks):
    if s not in disks:
        return "| %s | not run | – | – | – | – |" % NAMES[s]
    d = disks[s]
    try:
        for k in ("logical_bytes", "after_load_bytes", "end_bytes", "load_seconds"):
            _num(d[k])
        amp = "%.2f" % (d["end_bytes"] / float(d["logical_bytes"])) if d["logical_bytes"] else "–"
        return "| %s | %s | %s | %s | %s | %ds |" % (NAMES[s], _gib(d["logical_bytes"]), _gib(d["after_load_bytes"]),
                                                    _gib(d["end_bytes"]), amp, d["load_seconds"])
    except BAD_INPUT:
        return "| %s | unreadable | – | – | – | – |" % NAMES[s]


def render(meta, cells, disks, configs, run_dir=None):
    by = {(c["server"], c["level"], c["workload"]): c for c in cells}
    relaxed = meta.get("relaxed_mode", "none")
    m = lambda k: meta.get(k, "?")
    out = ["# TidesKV vs Kvrocks", "",
           "TidesKV `%s`, `%s`, `%s`; size `%s`; warm-up %ss, timed %ss; server cpus `%s`, memory `%s`; host %s; %s." % (
               m("tideskv_sha"), m("kvrocks_image"), m("memtier_image"), m("size"),
               m("warmup_s"), m("duration_s"), m("server_cpus"), m("server_mem"),
               m("host"), m("started_utc")),
           "", "A ratio above 1 means TidesKV is ahead on throughput.", ""]
    titles = {"relaxed": "Relaxed durability (TidesKV `sync-mode %s` vs Kvrocks `rocksdb.write_options.sync no`)" % relaxed,
              "strict": "Strict durability (TidesKV `sync-mode full` vs Kvrocks `rocksdb.write_options.sync yes`)"}
    for lv in LEVELS:
        out += ["## " + titles[lv], "",
                "| W | TidesKV ops/s | Kvrocks ops/s | ratio | TidesKV p50/p99/p999 ms | Kvrocks p50/p99/p999 ms |",
                "|---|---|---|---|---|---|"]
        notes = []
        for w in WORKLOADS:
            t, k = by[("tideskv", lv, w)], by[("kvrocks", lv, w)]
            ratio = "–" if t["error"] is not None or k["error"] is not None or not k["ops"] else "%.2f" % (t["ops"] / k["ops"])
            out.append("| %s | %s | %s | %s | %s | %s |" % (w, _ops(t), _ops(k), ratio, _lat(t), _lat(k)))
            for c in (t, k):
                if c["error"] is not None:
                    notes.append("- %s %s: %s" % (NAMES[c["server"]], w, c["error"]))
        out += [""]
        out += ["| W | TidesKV CPU µs/op | Kvrocks CPU µs/op | TidesKV peak mem | Kvrocks peak mem |",
                "|---|---|---|---|---|"]
        for w in WORKLOADS:
            t, k = by[("tideskv", lv, w)], by[("kvrocks", lv, w)]
            f = lambda c: "–" if c["cpu_us_per_op"] is None else "%.1f" % c["cpu_us_per_op"]
            g = lambda c: "–" if c["mem_peak_bytes"] is None else _gib(c["mem_peak_bytes"])
            out.append("| %s | %s | %s | %s | %s |" % (w, f(t), f(k), g(t), g(k)))
        dels = ["%s %d DEL/s" % (NAMES[s], round(by[(s, lv, "W6")]["del_ops"]))
                for s in SERVERS if by[(s, lv, "W6")]["del_ops"] is not None]
        if dels:
            out += ["", "W6 delete traffic: " + ", ".join(dels) + "."]
        if notes:
            out += ["", "Errors:"] + notes
        out += [""]
    failed = [x for x in SERVERS if run_dir and failure_reason(run_dir, x)]
    if failed:
        out += ["## Failed runs", ""]
        for x in failed:
            out += ["### " + NAMES[x], "", failure_reason(run_dir, x), "", "Last 20 lines of server.log:", "",
                    "```", log_tail(run_dir, x), "```", ""]
    out += ["## Disk and load", "", "| Server | logical | after load | end | space amp (end/logical) | load time |",
            "|---|---|---|---|---|---|"]
    for s in SERVERS:
        out.append(_disk_row(s, disks))
    out += ["", "## Fairness", "", "Settings read back from each running server with CONFIG GET.", ""]
    for s in SERVERS:
        if s in configs:
            out += ["### " + NAMES[s], "", "```", configs[s], "```", ""]
    return "\n".join(out) + "\n"


def main(argv):
    if len(argv) != 1:
        sys.stderr.write("usage: report.py <run_dir>\n")
        return 2
    run_dir = argv[0]
    meta, cells, disks, configs = load(run_dir)
    with open(os.path.join(run_dir, "report.md"), "w") as f:
        f.write(render(meta, cells, disks, configs, run_dir))
    with open(os.path.join(run_dir, "summary.json"), "w") as f:
        json.dump({"meta": meta, "cells": cells, "disks": disks}, f, indent=2)
    return 1 if any(c["error"] is not None for c in cells) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
