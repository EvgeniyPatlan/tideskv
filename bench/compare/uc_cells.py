#!/usr/bin/env python3
"""Readers of one repetition directory (<run>/<case>/<server>/rep<N>/) for the use-case suite report (stdlib only).

They raise NotRun when the phase was never run and CellError when the cell exists but its numbers cannot be used: a
failed phase, error replies, too few GET hits, unreadable files (spec section 5.6).
"""
import json
import os
import re
import statistics

import uc_verdict

CLIENT_BOUND_PCT = 85.0
MIN_HIT_RATE = 0.99
WINDOW_POINTS = 10
SET_REPLY_BYTES = 5    # "+OK\r\n"
INT_REPLY_BYTES = 4    # ":0\r\n" / ":1\r\n"
HERE = os.path.dirname(os.path.abspath(__file__))


class NotRun(Exception):
    """The phase this metric reads was not run."""


class CellError(Exception):
    """The cell ran but its numbers cannot be used; the message names the cause."""


def read_text(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().strip()


def _num(v):
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise CellError("not a number: %r" % (v,))
    return float(v)


def case_of(rep_dir):
    return os.path.basename(os.path.dirname(os.path.dirname(rep_dir)))


def _reads(rep_dir, name):
    path = os.path.join(rep_dir, name)
    if not os.path.exists(path):
        raise NotRun(name)
    try:
        return read_text(path)
    except OSError as e:
        raise CellError("cannot read %s: %s" % (name, e))


def _json(rep_dir, name):
    try:
        return json.loads(_reads(rep_dir, name))
    except ValueError as e:
        raise CellError("%s is not valid JSON: %s" % (name, e))


# ---- memtier cells ----

def _series_rx(sec):
    series = sec.get("Time-Serie")
    if not isinstance(series, dict):
        return None
    return sum(v.get("Bytes RX", 0) for v in series.values())


def _check_replies(case, stats):
    """Raise CellError when the replies were not what a valid run gets. Misses are an error only where GETs must hit."""
    problems = []
    for name, reply in (("Sets", SET_REPLY_BYTES), ("Hsets", INT_REPLY_BYTES), ("Hdels", INT_REPLY_BYTES)):
        sec = stats.get(name)
        if not sec or not sec.get("Count"):
            continue
        rx = _series_rx(sec)
        if rx is None:
            problems.append("no %s byte counts to check the replies" % name.upper())
        elif rx != reply * sec["Count"]:
            problems.append("%s replies are not all the expected %d bytes (error replies: %d bytes for %d)"
                            % (name.upper(), reply, rx, sec["Count"]))
    gets = stats.get("Gets")
    if case in ("U1", "U3", "U5") and gets and gets.get("Count"):
        hits, misses = gets.get("Hits/sec", 0.0), gets.get("Misses/sec", 0.0)
        if hits + misses > 0 and hits / (hits + misses) < MIN_HIT_RATE:
            problems.append("GET hit rate %.1f%% is below the expected %d%% after the load"
                            % (100.0 * hits / (hits + misses), round(MIN_HIT_RATE * 100)))
    if problems:
        raise CellError("errors: " + "; ".join(problems))


def cell(rep_dir, phase):
    """The memtier JSON of PHASE, after the status and reply checks (NotRun / CellError otherwise)."""
    status = _reads(rep_dir, phase + ".status").splitlines()[0:1]
    if status != ["ok"]:
        raise CellError("%s: %s" % (phase, status[0] if status else "no status"))
    d = _json(rep_dir, phase + ".json")
    try:
        stats = d["ALL STATS"]
        if not stats["Totals"]["Count"]:
            raise CellError("%s: memtier recorded no requests" % phase)
    except (KeyError, TypeError):
        raise CellError("%s: unreadable memtier result" % phase)
    _check_replies(case_of(rep_dir), stats)
    return d


def _section(rep_dir, phase, name):
    try:
        return cell(rep_dir, phase)["ALL STATS"][name]
    except (KeyError, TypeError):
        raise CellError("%s: no %s section in the memtier result" % (phase, name))


def ops(phase, section="Totals"):
    return lambda rep: _num(_section(rep, phase, section)["Ops/sec"])


def pct(phase, key):
    return lambda rep: _num(_section(rep, phase, "Totals")["Percentile Latencies"][key])


def hit_rate(phase):
    def f(rep):
        gets = _section(rep, phase, "Gets")
        total = gets["Hits/sec"] + gets["Misses/sec"]
        if total <= 0:
            raise NotRun("no GET hit counts")
        return 100.0 * gets["Hits/sec"] / total
    return f


def windows_over_10x(series, points=WINDOW_POINTS):
    """(windows over 10x the median window p99.9, window count, [p99.9 per window]) from memtier's Time-Serie.
    A window is `points` one-second points and holds the max p99.9 of them; a short last window is dropped."""
    pts = [series[k].get("p99.90") for k in sorted(series, key=int) if series[k].get("Count")]
    pts = [p for p in pts if p is not None]
    wins = [max(pts[i:i + points]) for i in range(0, len(pts) - points + 1, points)] or ([max(pts)] if pts else [])
    if not wins:
        raise CellError("no latency time series")
    median = statistics.median(wins)
    return sum(1 for w in wins if w > 10 * median), len(wins), wins


def p999_windows(rep_dir, phase):
    try:
        return windows_over_10x(cell(rep_dir, phase)["ALL STATS"]["Totals"]["Time-Serie"])
    except (KeyError, TypeError, AttributeError):
        raise CellError("%s: no Time-Serie in the memtier result" % phase)


def over_10x(phase):
    return lambda rep: float(p999_windows(rep, phase)[0])


# ---- disk ----

def du_rows(path):
    """[(epoch_s, bytes excluding the WAL archive, wal_archive_bytes)] of a du sampler file (header + rows)."""
    out = []
    try:
        for line in read_text(path).splitlines()[1:]:
            e, total, wal = (int(x) for x in line.split(","))
            out.append((e, max(0, total - wal), wal))
    except (OSError, ValueError) as e:
        raise CellError("unreadable du file %s: %s" % (os.path.basename(path), e))
    if not out:
        raise CellError("%s has no rows" % os.path.basename(path))
    return out


def du_peak(rep):
    _reads(rep, "du.csv")
    return float(max(r[1] for r in du_rows(os.path.join(rep, "du.csv"))))


def u2_end_state(rep_dir):
    """dbsize-end (int or 'unverified'), value size and the live-bytes estimate (dbsize-end x value-size, or None)."""
    raw = _reads(rep_dir, "dbsize-end")
    size = None
    try:
        size = int(_reads(rep_dir, "value-size").split()[0])
    except (NotRun, ValueError, IndexError):
        pass
    if not raw.isdigit():
        return {"dbsize": "unverified", "value_size": size, "live_estimate": None}
    return {"dbsize": int(raw), "value_size": size, "live_estimate": int(raw) * size if size else None}


# ---- info ----

def parse_info(text):
    out = {}
    for line in text.splitlines():
        k, sep, v = line.strip().partition(":")
        if sep:
            out[k] = v
    return out


def cache_hit_rate(phase):
    """Kvrocks block-cache hit rate over the timed run: delta of block_cache_hit and block_cache_miss (INFO rocksdb)."""
    def f(rep):
        a, b = parse_info(_reads(rep, phase + ".info0")), parse_info(_reads(rep, phase + ".info1"))
        try:
            hit = int(b["block_cache_hit"]) - int(a["block_cache_hit"])
            miss = int(b["block_cache_miss"]) - int(a["block_cache_miss"])
        except (KeyError, ValueError):
            raise NotRun("no block_cache counters")
        if hit + miss <= 0:
            raise CellError("no block cache lookups during the run")
        return 100.0 * hit / (hit + miss)
    return f


def debt_fields(kind):
    """Names of UC_DEBT_FIELDS_<kind> from uc_cases.sh (the runner's list, so the report cannot drift from it)."""
    try:
        text = read_text(os.path.join(HERE, "uc_cases.sh"))
    except OSError:
        return []
    m = re.search(r"UC_DEBT_FIELDS_%s=\(([^)]*)\)" % re.escape(kind), text)
    return m.group(1).split() if m else []


def debt_values(rep_dir, phase, kind):
    """{field: (before, after)} from a cell's INFO snapshots; numeric per-column-family fields name[cf] are summed."""
    a, b = parse_info(_reads(rep_dir, phase + ".info0")), parse_info(_reads(rep_dir, phase + ".info1"))

    def get(info, name):
        keys = [k for k in info if k == name or k.startswith(name + "[")]
        if not keys:
            return None
        try:
            return sum(float(info[k]) for k in keys)
        except ValueError:
            return info[keys[0]]
    return {f: (get(a, f), get(b, f)) for f in debt_fields(kind)}


# ---- flags: client-bound and under-target ----

def cpu_count(spec):
    """Number of CPUs in a cpuset string such as 8-15 or 0,2,4-5."""
    n = 0
    for part in spec.split(","):
        lo, _, hi = part.partition("-")
        n += int(hi) - int(lo) + 1 if hi else 1
    return n


def client_cpu_pct(rep_dir, phase, client_cpus):
    """Mean memtier CPU use over the timed run as a percent of the client cpuset, or None without samples."""
    try:
        text = read_text(os.path.join(rep_dir, phase + ".client.jsonl"))
        cpus = []
        for line in text.splitlines():
            try:
                cpus.append(float(json.loads(line)["CPUPerc"].rstrip("%")))
            except (ValueError, KeyError, TypeError):
                continue
        return sum(cpus) / len(cpus) / cpu_count(client_cpus) if cpus else None
    except (OSError, ValueError):
        return None


def cell_flags(rep_dir, phase, client_cpus):
    """{'client_bound': pct, 'under_target': (achieved, target)} for the entries that apply to this cell."""
    out = {}
    pct_used = client_cpu_pct(rep_dir, phase, client_cpus) if client_cpus else None
    if pct_used is not None and pct_used > CLIENT_BOUND_PCT:
        out["client_bound"] = pct_used
    try:
        d = json.loads(read_text(os.path.join(rep_dir, phase + ".json")))
        conf, achieved = d["configuration"], d["ALL STATS"]["Totals"]["Ops/sec"]
        target = conf["rate_limit"] * conf["clients"] * conf["threads"]
        if target > 0 and uc_verdict.flag_under_target(achieved, target):
            out["under_target"] = (achieved, float(target))
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return out




# ---- fio baselines ----

def _fio_job(path, direction):
    text = read_text(path)
    job = json.loads(text[text.index("{"):])["jobs"][0]
    d = job[direction]
    pc = (d.get("clat_ns") or {}).get("percentile", {})
    sync = (job.get("sync") or {}).get("lat_ns") or {}
    return {"mbs": d["bw_bytes"] / 1e6, "iops": d["iops"], "lat_ms": d["lat_ns"]["mean"] / 1e6 if d.get("lat_ns") else None,
            "p99_ms": pc["99.000000"] / 1e6 if "99.000000" in pc else None,
            "sync_mean_ms": sync["mean"] / 1e6 if sync.get("mean") else None,
            "sync_p99_ms": (sync.get("percentile") or {}).get("99.000000", 0) / 1e6 or None}


def fio_baselines(run_dir):
    """{'read': {qd: result}, 'fsync': result or None} from <run>/fio (warnings before the JSON are skipped)."""
    d, out = os.path.join(run_dir, "fio"), {"read": {}, "fsync": None}
    try:
        names = os.listdir(d)
    except OSError:
        return out
    for n in sorted(names):
        m = re.fullmatch(r"qd(\d+)\.json", n)
        try:
            if m:
                out["read"][int(m.group(1))] = _fio_job(os.path.join(d, n), "read")
            elif n == "fsync.json":
                out["fsync"] = _fio_job(os.path.join(d, n), "write")
        except (OSError, ValueError, KeyError, TypeError):
            continue
    return out
