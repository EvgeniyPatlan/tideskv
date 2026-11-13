#!/usr/bin/env python3
"""Metric table of the use-case suite report (stdlib only).

METRICS maps a case to its Metric list; Metric.extract(rep_dir) reads one repetition directory and returns a number
(see uc_cells for NotRun / CellError). U7's per-point metrics depend on the kill points of the run, so use
metrics_for(case, run_dir).
"""
import os
import re
import statistics
from dataclasses import dataclass, replace
from typing import Callable

from uc_cells import (CellError, NotRun, _json, _num, _reads, cache_hit_rate, du_peak, du_rows, hit_rate, ops, over_10x,
                      pct)


@dataclass(frozen=True)
class Metric:
    key: str
    title: str
    higher_is_better: bool
    headline: bool
    extract: Callable[[str], float | None]
    unit: str = ""            # ops/s, ms, bytes, count, s, %
    phase: str | None = None  # the memtier cell it reads (under-target / client-bound flags apply to it)
    info: bool = False        # shown, never given a verdict
    only: str | None = None   # "kvrocks": exists on Kvrocks only
    pros_cons: bool = True    # counted in the pros and cons lists and the case label (headline always)
    note: str = ""
    disk_src: str | None = None   # du file or summary.json:<field> read; {basis} in the title says allocated or apparent
    twin_of: str | None = None    # apparent-size twin of a disk metric (dropped when that metric is apparent already)
    floor_key: str | None = None  # meta.json key of a measurement floor: values at or under it get no verdict




def _du_last(name, col=1):
    """Last row of a du file; col 1 = used (allocated, else apparent), 2 = WAL archive, 3 = apparent size."""
    def f(rep):
        _reads(rep, name)
        return float(du_rows(os.path.join(rep, name))[-1][col])
    return f


# ---- U8 ----

def _u8_used(d, apparent=False):
    if "allocated_bytes" in d and not apparent:
        return float(max(0, d["allocated_bytes"] - d["wal_archive_allocated_bytes"]))
    return float(max(0, d["bytes"] - d["wal_archive_bytes"]))


def _u8(field_name, apparent=False):
    def f(rep):
        d = _json(rep, "summary.json").get(field_name)
        if d is None:
            raise NotRun(field_name)
        return _u8_used(d, apparent)
    return f


def _u8_plain(field_name):
    return lambda rep: _num(_json(rep, "summary.json")[field_name])


def _u8_growth(rep):
    d = _json(rep, "summary.json")
    if d.get("du_after_age") is None or d.get("u3_du_after_load") is None:
        raise NotRun("U3 disk figures")
    return _u8_used(d["du_after_age"]) - _u8_used(d["u3_du_after_load"])


def _u8_settle(rep):
    d = _json(rep, "summary.json")
    if d.get("settle_load_capped"):
        raise CellError("settle hit the cap (%s s) before the server went quiet" % d["settle_load_s"])
    return _num(d["settle_load_s"])


# ---- U7 ----

POINT_RE = re.compile(r"^(term|kill)-\d+$")


def u7_points(rep_dir):
    """{point: [dirs]} for the kill points of a repetition (run<i> subdirectories when UC_U7_TIMES > 1)."""
    out = {}
    try:
        names = sorted(n for n in os.listdir(rep_dir) if POINT_RE.match(n))
    except OSError:
        return out
    for n in names:
        d = os.path.join(rep_dir, n)
        runs = sorted((x for x in os.listdir(d) if re.match(r"^run\d+$", x)), key=lambda x: int(x[3:]))
        out[n] = [os.path.join(d, r) for r in runs] or [d]
    return out


def _u7_load_point(d):
    """CellError when the point was signalled before its acked-write target (ack.status not ok; older runs have none)."""
    path = os.path.join(d, "ack.status")
    if os.path.exists(path):
        status = _reads(d, "ack.status").splitlines()[0:1]
        if status != ["ok"]:
            raise CellError(status[0] if status else "empty ack.status")


def _u7_value(d, field_name):
    _u7_load_point(d)
    try:
        if field_name in ("acked_signal", "until_p99"):
            return _num(float(_reads(d, {"acked_signal": "acked-at-signal", "until_p99": "until-p99-ms"}[field_name])))
        if field_name in ("lost", "corrupt"):
            return _num(_json(d, "verify.json")[field_name])
        if field_name == "docker_run_ms":
            return _num(float(_reads(d, "restart.docker_run_ms")))
        if field_name == "term_s":
            return _num(float(_reads(d, "term.seconds")))
        v = _json(d, "probe.json")[field_name]
    except (KeyError, ValueError):
        raise CellError("missing %s in %s" % (field_name, os.path.basename(d)))
    if field_name == "restart_misses" and v is None:
        raise NotRun(field_name)
    if v is None:
        raise CellError("%s: no recovery within the probe timeout" % field_name)
    return _num(v)


def u7_metric(field_name, agg, point=None):
    """Value of a probe/verify field: summed (lost, corrupt, restart_misses) or the median over runs and points."""
    def f(rep):
        pts = u7_points(rep)
        if point is not None:
            pts = {point: pts.get(point, [])}
        dirs = [d for ds in pts.values() for d in ds]
        if not dirs:
            raise NotRun("no kill points")
        vals = [_u7_value(d, field_name) for d in dirs]
        return float(sum(vals)) if agg == "sum" else float(statistics.median(vals))
    return f


U7_FIELDS = (("lost", "lost writes", "count", "sum", True), ("corrupt", "corrupt values", "count", "sum", True),
             ("restart_misses", "restart misses", "count", "sum", True), ("pong_ms", "first PONG", "ms", "median", True),
             ("recovered_ms", "recovered (p99 within 2x)", "ms", "median", True))


def _u7_headline():
    return [Metric(k + "_all", "%s, all points" % t, False, k == "lost", u7_metric(k, agg), unit=u, pros_cons=pc)
            for k, t, u, agg, pc in U7_FIELDS]


def u7_point_metrics(points):
    """Per-point metrics (shown in the case table, not counted in the pros and cons)."""
    out = []
    for p in points:
        for k, t, u, _agg, _pc in U7_FIELDS + (("docker_run_ms", "docker run (not in PONG time)", "ms", "median", False),):
            out.append(Metric("%s:%s" % (k, p), "%s, %s" % (t, p), False, False, u7_metric(k, "median" if u == "ms" else "sum", p),
                              unit=u, pros_cons=False, info=k == "docker_run_ms"))
        n = p.split("-")[1]
        out.append(Metric("acked_signal:%s" % p, "acked writes at the signal (target %s), %s" % (n, p), False, False,
                          u7_metric("acked_signal", "median", p), unit="count", pros_cons=False, info=True))
        out.append(Metric("until_p99:%s" % p, "recovery threshold (2x warm p99), %s" % p, False, False,
                          u7_metric("until_p99", "median", p), unit="ms", pros_cons=False, info=True))
        if p.startswith("term"):
            out.append(Metric("term_s:%s" % p, "TERM shutdown, %s" % p, False, False, u7_metric("term_s", "median", p),
                              unit="s", pros_cons=False, info=True))
    return out


# ---- the table ----

def _m(key, title, hib, headline, fn, unit, phase=None, **kw):
    return Metric(key, title, hib, headline, fn, unit=unit, phase=phase, **kw)


def _open():
    out = []
    for ph in ("open25", "open50", "open75"):
        out += [_m(ph + "_p99", "%s p99" % ph, False, False, pct(ph, "p99.00"), "ms", ph),
                _m(ph + "_p999", "%s p99.9" % ph, False, False, pct(ph, "p99.90"), "ms", ph)]
    return out


def _build_metrics():
    m = {}
    m["U1"] = [_m("closed-gauss_ops", "closed-gauss ops/s", True, True, ops("closed-gauss"), "ops/s", "closed-gauss"),
               _m("closed-uniform_ops", "closed-uniform ops/s", True, False, ops("closed-uniform"), "ops/s", "closed-uniform")] \
        + _open() + [_m("cache_hit", "block cache hit rate (closed-gauss)", True, False, cache_hit_rate("closed-gauss"), "%",
                        info=True, only="kvrocks", note="Kvrocks only: TidesKV's INFO has no hit/miss counters")]
    basis = "{basis}, excl. WAL archive"
    m["U2"] = [_m("du_end", "disk at end (%s)" % basis, False, True, _du_last("du-end"), "bytes", disk_src="du-end"),
               _m("du_end_apparent", "disk at end (apparent size, excl. WAL archive)", False, False, _du_last("du-end", 3),
                  "bytes", info=True, twin_of="du_end"),
               _m("churn_p999", "churn p99.9", False, False, pct("churn", "p99.90"), "ms", "churn"),
               _m("du_peak", "peak disk (%s)" % basis, False, False, du_peak, "bytes", disk_src="du.csv"),
               _m("churn_ops", "churn ops/s (fixed rate)", True, False, ops("churn"), "ops/s", "churn", info=True),
               _m("churn_hit", "GET hit rate (negative lookups over a 1e9-key space)", True, False, hit_rate("churn"), "%",
                  "churn", info=True, note="most GETs ask for keys never written, so a low rate is by design; reported, not checked"),
               _m("wal_archive", "WAL archive at end ({basis})", False, False, _du_last("du-end", 2), "bytes", info=True,
                  disk_src="du-end")]
    m["U3"] = [_m("closed_p999", "closed p99.9 (not compared: servers run at different loads)", False, False,
                  pct("closed", "p99.90"), "ms", "closed", info=True),
               _m("closed_ops", "closed ops/s", True, False, ops("closed"), "ops/s", "closed"),
               _m("windows_over_10x", "windows over 10x median p99.9", False, False, over_10x("closed"), "count", "closed")] \
        + [replace(x, headline=x.key == "open50_p999") for x in _open() if x.key.endswith("_p999")]
    m["U4"] = [_m("mix_ops", "mix ops/s", True, True, ops("mix"), "ops/s", "mix"),
               _m("mix_p99", "mix p99 (not compared: closed loop)", False, False, pct("mix", "p99.00"), "ms", "mix", info=True)] \
        + [_m(s.lower() + "_ops", "%s ops/s" % s.upper().rstrip("S"), True, False, ops("mix", s), "ops/s", "mix",
              pros_cons=False, note="fixed command ratio: follows mix ops/s")
           for s in ("Hgets", "Hsets", "Hgetalls", "Hdels")]
    m["U5"] = []
    for sz in ("v1k", "v4k", "v16k"):
        src = "du-after-load-" + sz
        m["U5"] += [_m(sz + "_ops", "%s ops/s" % sz, True, sz == "v4k", ops(sz), "ops/s", sz),
                    _m(sz + "_p99", "%s p99 (not compared: closed loop)" % sz, False, False, pct(sz, "p99.00"), "ms", sz,
                       info=True),
                    _m(sz + "_du", "%s disk after load (%s)" % (sz, basis), False, False, _du_last(src), "bytes", disk_src=src),
                    _m(sz + "_du_apparent", "%s disk after load (apparent size, excl. WAL archive)" % sz, False, False,
                       _du_last(src, 3), "bytes", info=True, twin_of=sz + "_du")]
    m["U6"] = []
    for c in ("c1", "c8", "c64", "c256"):
        m["U6"] += [_m(c + "_ops", "%s ops/s" % c, True, c == "c64", ops(c), "ops/s", c),
                    _m(c + "_p99", "%s p99 (not compared: closed loop)" % c, False, False, pct(c, "p99.00"), "ms", c, info=True)]
    m["U7"] = _u7_headline()
    src = "summary.json:"
    m["U8"] = [_m("du_after_load", "disk after load (%s)" % basis, False, True, _u8("du_after_load"), "bytes",
                  disk_src=src + "du_after_load"),
               _m("du_after_load_apparent", "disk after load (apparent size, excl. WAL archive)", False, False,
                  _u8("du_after_load", True), "bytes", info=True, twin_of="du_after_load"),
               _m("load_ops", "bulk load ops/s", True, False, _u8_plain("load_ops_per_s"), "ops/s"),
               _m("settle_s", "time to quiet after load", False, False, _u8_settle, "s", floor_key="quiesce_window_s"),
               _m("u3_load", "U3 disk after its load (%s)" % basis, False, False, _u8("u3_du_after_load"), "bytes", info=True,
                  disk_src=src + "u3_du_after_load"),
               _m("age_growth", "U3 disk growth during aging (%s)" % basis, False, False, _u8_growth, "bytes", info=True,
                  disk_src=src + "du_after_age"),
               _m("du_after_age", "disk after U3 aging (%s)" % basis, False, False, _u8("du_after_age"), "bytes",
                  disk_src=src + "du_after_age"),
               _m("after_compact", "disk after COMPACT (%s)" % basis, False, False, _u8("kvrocks_after_compact"), "bytes",
                  info=True, only="kvrocks", disk_src=src + "kvrocks_after_compact",
                  note="Kvrocks only: TidesKV has no COMPACT. Measured after U1's load and both closed-loop phases, "
                       "on a store that has served traffic")]
    return m


METRICS = _build_metrics()


def metrics_for(case, run_dir):
    """METRICS[case], plus U7's per-point metrics for the kill points found under the run directory."""
    base = list(METRICS.get(case, []))
    if case != "U7":
        return base
    points = set()
    for _root, dirs, _files in os.walk(os.path.join(run_dir, case)):
        points.update(d for d in dirs if POINT_RE.match(d))
    order = sorted(points, key=lambda p: (p.split("-")[0] != "term", int(p.split("-")[1])))
    return base + u7_point_metrics(order)
