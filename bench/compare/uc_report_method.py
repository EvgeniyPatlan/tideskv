"""Methodology section of the use-case suite report, generated from the run's own values (stdlib only)."""

NAMES = {"tideskv": "TidesKV", "kvrocks": "Kvrocks", "kvrocks-blob": "Kvrocks+BlobDB", "kvrocks-cron": "Kvrocks+cron"}

NOT_COVERED = (
    "Replication, failover and slot migration under load (needs a multi-node harness).",
    "Memory sweeps from 1x to 10x RAM, relaxed-mode pipeline and connection sweeps, scans after deletes.",
    "Power-loss simulation: U7 kills the server process, it does not cut power or crash the OS.",
    "MULTI/Lua atomicity across a crash.",
    "Kvrocks features TidesKV does not have are listed in F1 and F2, not benchmarked.",
    "Any change to either server's code or any tuning beyond the settings listed above.",
)

INVITATION = (
    "This comparison is meant to be fair, and the people who know Kvrocks best can tell where it is not. If a setting "
    "above is not what you would run in production, a workload is shaped in a way that favours one side, or a number "
    "looks wrong, please say so: every setting, command line and raw result needed to reproduce a number is in the run "
    "directory, and a corrected configuration will be re-run and published next to the original.")


def _fmt_bytes(n):
    return "%.1f GiB" % (n / float(1 << 30)) if n >= 1 << 30 else "%.0f MiB" % (n / float(1 << 20))


def _setting_lines(summary):
    """{level: {server: set of config lines}} and {level: [cases]} over every case's config block."""
    out, cases = {}, {}
    for name, case in summary["cases"].items():
        level = case["setup"]["level"]
        cases.setdefault(level, []).append(name)
        for server, lines in case["setup"]["config"].items():
            out.setdefault(level, {}).setdefault(server, set()).update(lines)
    return out, cases


def _shared(summary):
    """The parity settings as TidesKV and Kvrocks were configured in this run (the first case that has them)."""
    for case in summary["cases"].values():
        cfg = case["setup"]["config"]
        t = {l.split(" ", 1)[0]: l.split(" ", 1)[1] for l in cfg.get("tideskv", []) if " " in l}
        k = {l.split(" ", 1)[0]: l.split(" ", 1)[1] for l in cfg.get("kvrocks", []) if " " in l}
        if t and k:
            return ("Workers %s; block cache %s MB (TidesKV %s bytes); memtable %s MB (TidesKV %s bytes); compression %s." % (
                k.get("workers", "?"), k.get("rocksdb.block_cache_size", "?"), t.get("block-cache-size", "?"),
                k.get("rocksdb.write_buffer_size", "?"), t.get("memtable-size", "?"), k.get("rocksdb.compression", "?")))
    return "Same workers, block cache, memtable and compression on both sides."


def _flush_lat(fs):
    if not fs.get("sync_mean_ms"):
        return ""
    lat = ", flush latency mean %.3f ms" % fs["sync_mean_ms"]
    return lat + (", p99 %.3f ms" % fs["sync_p99_ms"] if fs.get("sync_p99_ms") else "")


def _fio_lines(fio):
    out = []
    reads = fio.get("read") or {}
    if reads:
        out.append("- fio 256 KiB random reads (O_DIRECT) on the benchmark filesystem: " + "; ".join(
            "QD%d %.0f MB/s, %.0f IOPS" % (qd, r["mbs"], r["iops"]) for qd, r in sorted(reads.items())) + ".")
    else:
        out.append("- fio random-read baseline: not run in this run.")
    for key, name in (("append", "durable-append floor (4 KiB sequential write + fdatasync, queue depth 1; the U6 floor)"),
                      ("fsync", "fio reference (4 KiB random write + fsync, queue depth 1)")):
        fs = fio.get(key)
        if fs:
            out.append("- %s at the start of the run: %.0f writes/s%s." % (name, fs["iops"], _flush_lat(fs)))
    if not fio.get("append") and not fio.get("fsync"):
        out.append("- fio flush baselines: measured per server before each U6 sweep (see U6), not at the start of the run.")
    return out


def _observed_text(summary):
    """'U1 TidesKV 1.33, Kvrocks 1.05; U5 v1k TidesKV ...' from every case's observed on-disk / logical ratios."""
    parts = []
    for case, cs in summary["cases"].items():
        obs = cs.get("observed") or {}
        labels = sorted({label for v in obs.values() for label in v}, key=lambda x: (len(x), x))
        for label in labels:
            vals = ["%s %.2f" % (NAMES.get(s, s), v[label]) for s, v in obs.items() if label in v]
            parts.append("%s%s %s" % (case, "" if label in ("after load", "at end") else " " + label, ", ".join(vals)))
    return "; ".join(parts)


def methodology(summary):
    meta, quick = summary["meta"], summary["quick"]
    reps = meta.get("reps", "?")
    lines = ["## Methodology", ""]
    lines += [
        "1. **Repetitions.** Every timed cell ran %s time(s), servers interleaved per repetition (TidesKV, then each "
        "Kvrocks configuration, then again). The tables give the median and the min-max range.%s" % (
            reps, " This is a quick run: one repetition, so no verdicts are drawn." if quick else ""),
        "2. **Verdict rule.** The ratio is oriented so that above 1 means TidesKV is better (ops/s: TidesKV over Kvrocks; "
        "latency, bytes, recovery time: Kvrocks over TidesKV). A result is a tie when the ratio is inside the larger of "
        "the two sides' relative spread (max-min over median) or 5%. Verdicts are per metric, against the Kvrocks "
        "configuration that is best for that metric, so TidesKV is never compared with a weaker Kvrocks setting. A case "
        "row says \"mixed\" when its metrics disagree; there is no weighting. A metric with an error on either side "
        "(a failed repetition, error replies) gets no verdict.",
        "3. **Open-loop latency.** For U1 and U3 the closed-loop run finds each server's maximum, then both run at 25%, "
        "50% and 75% of the lower server's maximum (memtier per-connection rate limit), so neither is measured "
        "saturated. Latency is only given a verdict from these equal-load cells, and withheld when a server did not "
        "reach 95% of the offered rate (\"under target\"). U2 churn p99.9 also gets a verdict: it runs at a fixed rate "
        "that is identical for both servers, and it is withheld when under target. Closed-loop latency is shown, "
        "never compared.",
        "4. **Equal resources.** Server on CPUs %s (%s), memory cap %s, open files %s; client on CPUs %s. %s Settings are "
        "read back from the running server before any cell is measured (a mismatch fails the cell)." % (
            meta.get("server_cpus", "?"), meta.get("server_cores", "?"), meta.get("server_mem", "?"),
            meta.get("server_nofile", "?"), meta.get("client_cpus", "?"), _shared(summary)),
        "5. **Durability.** U1-U5 and U8 run relaxed (TidesKV `sync-mode none`, Kvrocks `write_options.sync no`); "
        "relaxed is not power-loss safe on either side. U6 and U7 run strict (fsync before every acknowledged write on "
        "both sides). TidesKV's interval mode is not compared with Kvrocks `sync yes`. Strict mode is configured and read "
        "back, but per-ack fsync is not observed by the suite (neither server reports an fsync count).",
        "6. **Data.** LZ4 is on for both servers; memtier writes random values, and how well they compress is not assumed "
        "but measured. %s Disk figures are allocated bytes (`du --block-size=1`) after the server went "
        "quiet, excluding Kvrocks' WAL archive (default `wal_ttl_seconds 10800`), which is reported on its own line; "
        "apparent size is shown beside them. Runs recorded before the allocated column existed fall back to apparent "
        "size, and the tables label it as such." % (
            ("The observed on-disk / logical ratio (allocated bytes excluding the WAL archive after the load, over "
             "keys x value size; U2: disk at end over the live-bytes estimate) was: %s." % _observed_text(summary))
            if _observed_text(summary) else "No observed on-disk / logical ratio is available in this run."),
        "7. **Correctness.** Checked per cell from memtier's per-command byte and hit counts: every SET reply must be "
        "the 5-byte `+OK` (U1, U2, U3, U5, U6), every HSET and HDEL reply a 4-byte integer (U4), and GETs must hit at "
        "least 99% after a load (U1, U3, U5). A failed check makes the cell ERR, shown as such and not as a number. Not "
        "checked: U2's GET replies (nearly all are negative lookups by design; the hit rate is reported) and U4's HGET "
        "and HGETALL replies (memtier counts no hits for them and their lengths vary). U7's writer stops on an error "
        "reply, and its verify step reads back every acknowledged value.",
        "8. **Client not the bottleneck.** A cell whose memtier used over 85% of the client CPUs is flagged "
        "client-bound and gets no verdict.",
        "9. **Baselines and host.** Host: %s. Loopback networking inside one Docker network." % meta.get("host", "?")]
    lines += ["   " + x for x in _fio_lines(summary.get("fio") or {})]
    lines += ["10. **Raw data.** memtier JSON, INFO before and after each cell, configs as read back, the du series and "
              "the commit and image digests are in the run directory (TidesKV %s; %s; %s)." % (
                  meta.get("tideskv_sha", "?"), meta.get("kvrocks_image", "?"), meta.get("memtier_image", "?")), ""]
    lines += ["### Settings on each side", "",
              "Both servers get the shared parity configuration plus the case's documented choice. Every setting that "
              "was written, as read back:", ""]
    by_level, level_cases = _setting_lines(summary)
    for level in ("relaxed", "strict"):
        if level in by_level:
            lines += ["**%s** (%s)" % (level.capitalize(), ", ".join(level_cases[level])), ""]
            lines += ["- %s: %s" % (server, ", ".join("`%s`" % x for x in sorted(ls)) or "defaults")
                      for server, ls in sorted(by_level[level].items())]
            lines.append("")
    lines += ["### Not covered in v1", ""] + ["- " + x for x in NOT_COVERED]
    lines += ["", "### For Kvrocks maintainers", "", INVITATION, ""]
    return lines
