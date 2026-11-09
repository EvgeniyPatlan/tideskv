"""W7 (run.sh --large) section of the report: 256 KiB values on a miss-heavy cache (notes design 2026-11-09).

report.py calls collect() and render() when the run directory is a --large run.
"""
import json
import os

import report as base

SERVERS = ("tideskv", "kvrocks", "kvrocks-blob")
PHASES = ("R1", "R4", "R32", "R32w16", "M32")
VALUE_SIZE = 262144
FIO_QDS = (1, 2, 4, 8, 16, 32)
CLIENT_BOUND_PCT = 85.0
# devices whose byte counts are this close to the busiest are a stack (dm-crypt, LVM, the NVMe below them):
# the physical device is the one whose queue and latency matter
_STACK_TOLERANCE = 0.10


def _mb(n):
    return n / 1e6


def parse_memtier_w7(path):
    """GET and SET counts, rates and latencies, plus the run's totals."""
    with open(path) as f:
        stats = json.load(f)["ALL STATS"]
    out = {"duration_s": base._num(stats["Runtime"]["Total duration"]) / 1000.0,
           "count": base._num(stats["Totals"]["Count"])}
    for key, name in (("Gets", "get"), ("Sets", "set")):
        sec = stats.get(key) or {}
        out[name + "_count"] = base._num(sec.get("Count", 0))
        out[name + "_ops"] = base._num(sec.get("Ops/sec", 0.0))
        pct = sec.get("Percentile Latencies") or {}
        for src, dst in (("p50.00", "p50"), ("p99.00", "p99"), ("p99.90", "p999")):
            v = pct.get(src)
            out["%s_%s" % (name, dst)] = base._num(v) if v is not None and out[name + "_count"] else None
    return out


def parse_io_stat(text):
    """{'maj:min': {'rbytes': n, 'wbytes': n, 'rios': n, 'wios': n}} from a cgroup io.stat snapshot."""
    devs = {}
    for line in text.splitlines():
        parts = line.split()
        if not parts or ":" not in parts[0]:
            continue
        d = {}
        for kv in parts[1:]:
            k, _, v = kv.partition("=")
            if v.isdigit():
                d[k] = int(v)
        devs[parts[0]] = d
    return devs


def io_delta(before, after):
    """Per-device delta of rbytes/wbytes/rios/wios; devices missing before count from 0."""
    out = {}
    for dev, a in after.items():
        b = before.get(dev, {})
        out[dev] = {k: a.get(k, 0) - b.get(k, 0) for k in ("rbytes", "wbytes", "rios", "wios")}
    return out


def parse_diskstats(text):
    """[(epoch_ms, {'maj:min': (name, fields...)})] from diskstats_sampler output."""
    samples, cur = [], None
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] == "T" and parts[1].isdigit():
            cur = (int(parts[1]), {})
            samples.append(cur)
        elif cur is not None and len(parts) >= 14 and all(p.isdigit() for p in parts[3:14]):
            cur[1]["%s:%s" % (parts[0], parts[1])] = (parts[2],) + tuple(int(p) for p in parts[3:14])
    return [s for s in samples if s[1]]


def pick_device(deltas, names):
    """The busiest device by bytes; among a stack within 10% of it, the one that is not device-mapper."""
    load = {d: v["rbytes"] + v["wbytes"] for d, v in deltas.items()}
    if not load or max(load.values()) <= 0:
        return None
    top = max(load.values())
    close = sorted((d for d, n in load.items() if n >= top * (1 - _STACK_TOLERANCE)), key=lambda d: -load[d])
    physical = [d for d in close if not names.get(d, "").startswith("dm-")]
    return (physical or close)[0]


def device_queue(samples, dev):
    """(aqu-sz, r_await ms) for dev between the first and the last sample, or (None, None)."""
    have = [(t, s[dev]) for t, s in samples if dev in s]
    if len(have) < 2:
        return None, None
    (t0, a), (t1, b) = have[0], have[-1]
    wall = t1 - t0
    # fields after the name: reads, rmerged, rsect, read_ms, writes, wmerged, wsect, write_ms, inflight, io_ms, weighted
    aqu = (b[11] - a[11]) / float(wall) if wall > 0 else None
    reads = b[1] - a[1]
    r_await = (b[4] - a[4]) / float(reads) if reads > 0 else None
    return aqu, r_await


def _busiest_in_diskstats(samples):
    if len(samples) < 2:
        return None
    first, last = samples[0][1], samples[-1][1]
    deltas = {d: {"rbytes": (last[d][3] - first[d][3]) * 512, "wbytes": (last[d][7] - first[d][7]) * 512}
              for d in last if d in first}
    return pick_device(deltas, {d: v[0] for d, v in last.items()})


def parse_info_counters(text, keys=("block_cache_data_hit", "block_cache_data_miss")):
    """First value of each key in INFO output (Kvrocks lists the block-cache counters in two sections)."""
    out = {}
    for line in text.splitlines():
        k, sep, v = line.strip().partition(":")
        if sep and k in keys and k not in out and v.strip().isdigit():
            out[k] = int(v.strip())
    return out


def _hit_pct(before, after):
    try:
        hit = after["block_cache_data_hit"] - before["block_cache_data_hit"]
        miss = after["block_cache_data_miss"] - before["block_cache_data_miss"]
    except KeyError:
        return None
    return 100.0 * hit / (hit + miss) if hit + miss > 0 else None


def _read_opt(path):
    try:
        return base._read(path)
    except base.BAD_INPUT:
        return None


def _client_pct(path, ncpu):
    """Mean CPU of the timed memtier container as a % of the client cpuset, from docker stats samples."""
    if not os.path.exists(path) or not ncpu:
        return None
    try:
        s = base.parse_stats(path)
    except base.BAD_INPUT:
        return None
    return None if s["cpu_mean_pct"] is None else s["cpu_mean_pct"] / float(ncpu)


def _empty(server, phase):
    keys = ("get_ops", "set_ops", "get_p50", "get_p99", "get_p999", "set_p99", "read_mbs", "write_mbs",
            "reads_per_get", "write_amp", "device", "aqu_sz", "r_await_ms", "cpu_us_per_op", "mem_peak_bytes",
            "data_hit_pct", "client_cpu_pct")
    cell = {k: None for k in keys}
    cell.update({"server": server, "phase": phase, "client_bound": False, "settle_s": None,
                 "settle_capped": False, "error": None})
    return cell


def _disk_fields(cell, d, phase, m):
    io0, io1 = _read_opt(os.path.join(d, phase + ".io0")), _read_opt(os.path.join(d, phase + ".io1"))
    ds = _read_opt(os.path.join(d, phase + ".diskstats"))
    samples = parse_diskstats(ds) if ds else []
    names = {}
    for _, s in samples:
        names.update({dev: v[0] for dev, v in s.items()})
    dev = None
    if io0 is not None and io1 is not None:
        deltas = io_delta(parse_io_stat(io0), parse_io_stat(io1))
        dev = pick_device(deltas, names)
        dur = m["duration_s"]
        if dev is not None:
            delta = deltas[dev]
        else:  # the server did no I/O in the window
            delta = {"rbytes": 0, "wbytes": 0}
        if dur > 0:
            cell["read_mbs"] = _mb(delta["rbytes"]) / dur
            cell["write_mbs"] = _mb(delta["wbytes"]) / dur
        if m["get_count"]:
            cell["reads_per_get"] = delta["rbytes"] / float(m["get_count"] * VALUE_SIZE)
        if phase.startswith("M") and m["set_count"]:
            # io1s is io.stat after a host sync: a buffered writer's dirty pages from the window, written back
            # after it ends, still count against its SETs
            synced = _read_opt(os.path.join(d, phase + ".io1s"))
            wbytes = delta["wbytes"]
            if synced is not None and dev is not None:
                wbytes = io_delta(parse_io_stat(io0), parse_io_stat(synced)).get(dev, delta)["wbytes"]
            cell["write_amp"] = wbytes / float(m["set_count"] * VALUE_SIZE)
    if dev is None:
        dev = _busiest_in_diskstats(samples)
    if dev is not None:
        cell["device"] = names.get(dev, dev)
        cell["aqu_sz"], cell["r_await_ms"] = device_queue(samples, dev)


def _w7_cell(run_dir, server, phase, ncpu):
    cell = _empty(server, phase)
    d = os.path.join(run_dir, server, "large")
    if not os.path.isdir(d):
        reason = base.failure_reason(run_dir, server)
        cell["error"] = "not run: " + reason if reason else "not run"
        return cell
    cell["settle_s"], cell["settle_capped"] = base.read_settle(os.path.join(d, phase + ".settle"))
    status = _read_opt(os.path.join(d, phase + ".status"))
    if status != "ok":
        cell["error"] = status or "no status"
        return cell
    try:
        m = parse_memtier_w7(os.path.join(d, phase + ".json"))
    except base.BAD_INPUT as e:
        cell["error"] = "unreadable memtier result: " + str(e)
        return cell
    if m["count"] == 0:
        cell["error"] = "memtier recorded no requests"
        return cell
    cell.update({"get_ops": m["get_ops"], "set_ops": m["set_ops"], "get_p50": m["get_p50"],
                 "get_p99": m["get_p99"], "get_p999": m["get_p999"], "set_p99": m["set_p99"]})
    try:
        _disk_fields(cell, d, phase, m)
    except base.BAD_INPUT + (IndexError,):
        pass
    usec = base.read_cpu_usec(os.path.join(d, phase + ".cpu"))
    if usec is not None:
        cell["cpu_us_per_op"] = usec / float(m["count"])
    stats = os.path.join(d, phase + ".stats.jsonl")
    if os.path.exists(stats):
        try:
            cell["mem_peak_bytes"] = base.parse_stats(stats)["mem_peak_bytes"]
        except base.BAD_INPUT:
            pass
    if server.startswith("kvrocks"):
        i0, i1 = _read_opt(os.path.join(d, phase + ".info0")), _read_opt(os.path.join(d, phase + ".info1"))
        if i0 and i1:
            cell["data_hit_pct"] = _hit_pct(parse_info_counters(i0), parse_info_counters(i1))
    cell["client_cpu_pct"] = _client_pct(os.path.join(d, phase + ".client.jsonl"), ncpu)
    cell["client_bound"] = cell["client_cpu_pct"] is not None and cell["client_cpu_pct"] > CLIENT_BOUND_PCT
    return cell


def phases_of(meta):
    got = str(meta.get("w7_phases", "")).split()
    return tuple(p for p in got if p in PHASES) or tuple(p for p in PHASES if p != "R32w16")


def collect(run_dir, meta):
    ncpu = meta.get("client_cpu_count")
    ncpu = ncpu if isinstance(ncpu, int) and ncpu > 0 else None
    return [_w7_cell(run_dir, s, p, ncpu) for s in SERVERS for p in phases_of(meta)]


def parse_fio(path):
    """{'mbs', 'iops', 'lat_ms', 'p99_ms'} from fio's JSON (warnings before the JSON are skipped)."""
    with open(path) as f:
        text = f.read()
    job = json.loads(text[text.index("{"):])["jobs"][0]["read"]
    p99 = (job.get("clat_ns") or {}).get("percentile", {}).get("99.000000")
    return {"mbs": base._num(job["bw_bytes"]) / 1e6, "iops": base._num(job["iops"]),
            "lat_ms": base._num(job["lat_ns"]["mean"]) / 1e6, "p99_ms": None if p99 is None else base._num(p99) / 1e6}


def collect_fio(run_dir):
    out = {}
    for qd in FIO_QDS:
        p = os.path.join(run_dir, "fio", "qd%d.json" % qd)
        if os.path.exists(p):
            try:
                out[qd] = parse_fio(p)
            except (base.BAD_INPUT + (IndexError,)):
                out[qd] = None
    return out


def _f(v, fmt):
    return "–" if v is None else fmt % v


def _client(c):
    if c["client_cpu_pct"] is None:
        return "–"
    return "%.0f%%%s" % (c["client_cpu_pct"], " client-bound?" if c["client_bound"] else "")


def _phase_label(p):
    return "R32 (workers 16)" if p == "R32w16" else p


def _header(meta):
    m = lambda k: meta.get(k, "?")
    return ["## W7: 256 KiB values on a miss-heavy cache", "",
            "%s keys of %s B (`L:<n>`, random data), loaded once per server. Block cache %s MiB on both servers, "
            "server memory `%s`, workers 4 (R32 (workers 16) is the reads-in-flight sweep). Each phase restarts the "
            "server and drops its data from the page cache first. Reads are uniform over the working set: R1/R4/R32 "
            "read 1/32, 4/32 and all of the keys (at the full 131072 keys: 1x, 4x and 32x the block cache), M32 is "
            "90/10 GET/SET over all of them. memtier %s threads × %s "
            "clients, pipeline 1; warm-up %ss, timed %ss; relaxed durability." % (
                m("w7_keys"), m("w7_value_size"), m("block_cache_mb"), m("server_mem"), m("threads"), m("clients"),
                m("warmup_s"), m("duration_s")), ""]


def _tables(cells):
    out = ["| Server | Phase | GET/s | SET/s | GET p50 / p99 / p99.9 ms | SET p99 ms | CPU µs/op | peak mem "
           "| client CPU | settle s |",
           "|---|---|---|---|---|---|---|---|---|---|"]
    notes = []
    for c in cells:
        name = base.NAMES[c["server"]]
        if c["error"] is not None:
            out.append("| %s | %s | ERR | ERR | ERR | ERR | – | – | – | %s |" % (
                name, _phase_label(c["phase"]), base._fmt_settle(c["settle_s"], c["settle_capped"])))
            notes.append("- %s %s: %s" % (name, c["phase"], c["error"]))
            continue
        lat = "%s / %s / %s" % (_f(c["get_p50"], "%.2f"), _f(c["get_p99"], "%.2f"), _f(c["get_p999"], "%.2f"))
        out.append("| %s | %s | %d | %d | %s | %s | %s | %s | %s | %s |" % (
            name, _phase_label(c["phase"]), round(c["get_ops"]), round(c["set_ops"]), lat, _f(c["set_p99"], "%.2f"),
            _f(c["cpu_us_per_op"], "%.1f"), "–" if c["mem_peak_bytes"] is None else base._gib(c["mem_peak_bytes"]),
            _client(c), base._fmt_settle(c["settle_s"], c["settle_capped"])))
    out += ["", "| Server | Phase | device | read MB/s | write MB/s | disk reads / GET | write amp | aqu-sz "
            "| r_await ms | Kvrocks data-block hit % |",
            "|---|---|---|---|---|---|---|---|---|---|"]
    for c in cells:
        if c["error"] is not None:
            continue
        out.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            base.NAMES[c["server"]], _phase_label(c["phase"]), c["device"] or "–", _f(c["read_mbs"], "%.1f"),
            _f(c["write_mbs"], "%.1f"), _f(c["reads_per_get"], "%.2f"), _f(c["write_amp"], "%.2f"),
            _f(c["aqu_sz"], "%.2f"), _f(c["r_await_ms"], "%.2f"), _f(c["data_hit_pct"], "%.1f")))
    return out, notes


def _fio_lines(meta, fio):
    out = ["### Disk ceiling (fio)", ""]
    status = meta.get("fio_status")
    if status is not None and status != "ok":
        out += ["fio did not run: %s." % status, ""]
        if not fio:
            return out
    if not fio:
        return out + ["No fio results.", ""]
    out += ["256 KiB random reads, O_DIRECT, libaio, %s s per queue depth, on the bench filesystem (`%s`, file %s)." % (
        meta.get("fio_runtime_s", "?"), meta.get("fio_image", "?"), meta.get("fio_size", "?")), "",
            "| QD | MB/s | IOPS | mean lat ms | p99 lat ms |", "|---|---|---|---|---|"]
    for qd in FIO_QDS:
        if qd not in fio:
            continue
        r = fio[qd]
        if r is None:
            out.append("| %d | unreadable | – | – | – |" % qd)
        else:
            out.append("| %d | %.1f | %d | %.2f | %s |" % (qd, r["mbs"], round(r["iops"]), r["lat_ms"],
                                                          _f(r["p99_ms"], "%.2f")))
    return out + [""]


def render(meta, cells, fio, disks, configs, run_dir=None):
    m = lambda k: meta.get(k, "?")
    out = ["# TidesKV vs Kvrocks: W7 large values", "",
           "TidesKV `%s`, `%s`, `%s`; size `%s`; server cpus `%s`, memory `%s`; client cpus `%s`; host %s; %s." % (
               m("tideskv_sha"), m("kvrocks_image"), m("memtier_image"), m("size"), m("server_cpus"),
               m("server_mem"), m("client_cpus"), m("host"), m("started_utc")), ""]
    out += _header(meta)
    tables, notes = _tables(cells)
    out += tables + [""]
    out += ["- Device numbers are the server cgroup's io.stat deltas over the timed run, on its busiest device "
            "(the physical one of a dm stack). Disk reads / GET = bytes read ÷ (GETs × 256 KiB); write amp = bytes "
            "written ÷ (SETs × 256 KiB), M32 only, counted after a host sync so that dirty pages written back after the "
            "window count too.",
            "- aqu-sz (Δweighted ms ÷ Δwall ms) and r_await (Δread ms ÷ Δreads) come from /proc/diskstats sampled "
            "once a second; they are the device's, host-wide.",
            "- Client CPU is the timed memtier container's mean CPU as a share of its cpuset; above %d%% the run is "
            "flagged client-bound?." % CLIENT_BOUND_PCT, ""]
    if notes:
        out += ["Errors:"] + notes + [""]
    out += _fio_lines(meta, fio)
    failed = [s for s in SERVERS if run_dir and base.failure_reason(run_dir, s)]
    if failed:
        out += ["## Failed runs", ""]
        for s in failed:
            out += ["### " + base.NAMES[s], "", base.failure_reason(run_dir, s), "", "Last 20 lines of server.log:",
                    "", "```", base.log_tail(run_dir, s), "```", ""]
    out += ["## Disk and load", "", "| Server | logical | after load | end | space amp (end/logical) | load time "
            "| settle after load | settle after restart |", "|---|---|---|---|---|---|---|---|"]
    out += [base._disk_row(s, disks) for s in SERVERS]
    out += ["", "## Fairness", "", "Settings read back from each running server with CONFIG GET, once per start.", ""]
    for s in SERVERS:
        if s in configs:
            out += ["### " + base.NAMES[s], "", "```", configs[s], "```", ""]
    return "\n".join(out) + "\n"


def is_large(meta, run_dir):
    return str(meta.get("size", "")).startswith("large") or os.path.isdir(os.path.join(run_dir, "tideskv", "large"))


def load_extras(run_dir, disks, configs):
    """disk.json and config.txt of kvrocks-blob, which report.load does not know."""
    s = "kvrocks-blob"
    p = os.path.join(run_dir, s, "disk.json")
    if os.path.exists(p):
        try:
            with open(p) as f:
                disks[s] = json.load(f)
            disks[s]["settle_load_s"], disks[s]["settle_load_capped"] = base.read_settle(
                os.path.join(run_dir, s, "settle-load"))
        except (base.BAD_INPUT + (IndexError,)):
            disks[s] = None
    text = _read_opt(os.path.join(run_dir, s, "config.txt"))
    if text is not None:
        configs[s] = text
