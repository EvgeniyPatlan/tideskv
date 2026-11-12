#!/usr/bin/env python3
"""Metric table of the use-case suite report (stdlib only).

METRICS maps a case to its Metric list; Metric.extract(rep_dir) reads one repetition directory and returns a number
(see uc_cells for NotRun / CellError). U7's per-point metrics depend on the kill points of the run, so use
metrics_for(case, run_dir).
"""
import os
import re
import statistics
from dataclasses import dataclass
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




def _du_last(name, col=1):
    def f(rep):
        _reads(rep, name)
        return float(du_rows(os.path.join(rep, name))[-1][col])
    return f


# ---- U8 ----

def _u8(field_name, wal=True, only=None):
    def f(rep):
        d = _json(rep, "summary.json").get(field_name)
        if d is None:
            raise NotRun(field_name)
        return float(max(0, d["bytes"] - d["wal_archive_bytes"])) if wal else _num(d)
    return f


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


def _u7_value(d, field_name):
    try:
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
    m["U2"] = [_m("du_end", "disk at end (excl. WAL archive)", False, True, _du_last("du-end"), "bytes"),
               _m("churn_p999", "churn p99.9", False, False, pct("churn", "p99.90"), "ms", "churn"),
               _m("du_peak", "peak disk (excl. WAL archive)", False, False, du_peak, "bytes"),
               _m("churn_ops", "churn ops/s (fixed rate)", True, False, ops("churn"), "ops/s", "churn", info=True),
               _m("churn_hit", "churn GET hit rate (misses expected)", True, False, hit_rate("churn"), "%", "churn", info=True),
               _m("wal_archive", "WAL archive at end", False, False, _du_last("du-end", 2), "bytes", info=True)]
    m["U3"] = [_m("closed_p999", "closed p99.9", False, True, pct("closed", "p99.90"), "ms", "closed"),
               _m("closed_ops", "closed ops/s", True, False, ops("closed"), "ops/s", "closed"),
               _m("windows_over_10x", "windows over 10x median p99.9", False, False, over_10x("closed"), "count", "closed")] \
        + [x for x in _open() if x.key.endswith("_p999")]
    m["U4"] = [_m("mix_ops", "mix ops/s", True, True, ops("mix"), "ops/s", "mix"),
               _m("mix_p99", "mix p99", False, False, pct("mix", "p99.00"), "ms", "mix")] \
        + [_m(s.lower() + "_ops", "%s ops/s" % s.upper().rstrip("S"), True, False, ops("mix", s), "ops/s", "mix",
              pros_cons=False, note="fixed command ratio: follows mix ops/s")
           for s in ("Hgets", "Hsets", "Hgetalls", "Hdels")]
    m["U5"] = []
    for sz in ("v1k", "v4k", "v16k"):
        m["U5"] += [_m(sz + "_ops", "%s ops/s" % sz, True, sz == "v4k", ops(sz), "ops/s", sz),
                    _m(sz + "_p99", "%s p99" % sz, False, False, pct(sz, "p99.00"), "ms", sz),
                    _m(sz + "_du", "%s disk after load (excl. WAL archive)" % sz, False, False,
                       _du_last("du-after-load-" + sz), "bytes")]
    m["U6"] = []
    for c in ("c1", "c8", "c64", "c256"):
        m["U6"] += [_m(c + "_ops", "%s ops/s" % c, True, c == "c64", ops(c), "ops/s", c),
                    _m(c + "_p99", "%s p99" % c, False, False, pct(c, "p99.00"), "ms", c)]
    m["U7"] = _u7_headline()
    m["U8"] = [_m("du_after_load", "disk after load (excl. WAL archive)", False, True, _u8("du_after_load"), "bytes"),
               _m("load_ops", "bulk load ops/s", True, False, _u8("load_ops_per_s", wal=False), "ops/s"),
               _m("settle_s", "time to quiet after load", False, False, _u8_settle, "s"),
               _m("du_after_age", "disk after U3 aging (excl. WAL archive)", False, False, _u8("du_after_age"), "bytes"),
               _m("after_compact", "disk after COMPACT (excl. WAL archive)", False, False, _u8("kvrocks_after_compact"),
                  "bytes", info=True, only="kvrocks", note="Kvrocks only: TidesKV has no COMPACT")]
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
