"""Final-review fix tests of uc_report (run: python3 -m unittest bench/compare/test_uc_report_final.py)."""
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_uc_report import Run, entry, memtier  # noqa: E402


def u7_point(run, srv, pt, lost=0, acked=None, status=None, until=0.1):
    d = "U7/%s/rep1/%s/" % (srv, pt)
    run.write(d + "verify.json", json.dumps({"acked": 10, "lost": lost, "corrupt": 0}))
    run.write(d + "probe.json", json.dumps({"pong_ms": 40, "recovered_ms": 100, "restart_misses": 0}))
    run.write(d + "restart.docker_run_ms", "150")
    run.write(d + "until-p99-ms", str(until))
    if acked is not None:
        run.write(d + "acked-at-signal", str(acked))
        run.write(d + "acked-target", pt.split("-")[1])
    if status is not None:
        run.write(d + "ack.status", status)


class U7LoadPointTest(unittest.TestCase):
    def test_acked_at_signal_and_threshold_are_shown_per_point(self):
        run = Run(self, reps=1)
        for srv, until in (("tideskv", 0.096), ("kvrocks", 0.112)):
            for pt in ("term-1271", "kill-1271"):
                u7_point(run, srv, pt, acked=1290 if srv == "tideskv" else 1305, status="ok", until=until)
            run.done("U7", srv, 1)
        s, text = run.report()
        self.assertEqual(entry(s, "U7", "acked_signal:term-1271")["configs"]["kvrocks"]["median"], 1305)
        self.assertTrue(entry(s, "U7", "acked_signal:term-1271")["info"])
        self.assertEqual(entry(s, "U7", "until_p99:kill-1271")["configs"]["tideskv"]["median"], 0.096)
        self.assertIn("| acked writes at the signal (target 1271), term-1271 |", text)
        self.assertIn("recovery threshold (2x warm p99), kill-1271", text)
        self.assertIn("0.112 ms", text)
        self.assertNotIn("10, 60 and 120 s", text)

    def test_point_that_missed_its_load_point_is_err(self):
        run = Run(self, reps=1)
        for srv in ("tideskv", "kvrocks"):
            u7_point(run, srv, "term-50", acked=50, status="ok")
            u7_point(run, srv, "kill-50", acked=7, status="load point not reached: 7 of 50 acknowledged writes in 300 s"
                     if srv == "kvrocks" else "ok")
            run.done("U7", srv, 1)
        s, text = run.report()
        e = entry(s, "U7", "lost:kill-50")
        self.assertIn("load point not reached", " ".join(e["configs"]["kvrocks"]["errors"]))
        self.assertFalse(entry(s, "U7", "lost:term-50")["configs"]["kvrocks"]["errors"])
        self.assertTrue(entry(s, "U7", "lost_all")["configs"]["kvrocks"]["errors"])
        self.assertTrue(any("load point not reached" in f for f in s["failed"]))


def du5(run, case, srv, rep, name, alloc, wal=0):
    run.write("%s/%s/rep%d/%s" % (case, srv, rep, name),
              "epoch_s,bytes,wal_archive_bytes,allocated_bytes,wal_archive_allocated_bytes\n1,%d,%d,%d,%d\n"
              % (alloc, wal, alloc, wal))


def load_cfg(run, case, srv, rep, name, keys, size):
    run.write("%s/%s/rep%d/%s/1.json" % (case, srv, rep, name),
              json.dumps({"configuration": {"key_minimum": 1, "key_maximum": keys, "data_size": size}}))


def fio_doc(iops):
    return json.dumps({"jobs": [{"write": {"bw_bytes": 1, "iops": iops, "lat_ns": {"mean": 1.0}, "clat_ns": {"percentile": {}}},
                                 "sync": {"lat_ns": {"mean": 500000.0}}}]})


def u8_doc(settle, capped):
    return json.dumps({"du_after_load": {"bytes": 10, "wal_archive_bytes": 0}, "load_ops_per_s": 5, "settle_load_s": settle,
                       "settle_load_capped": capped, "du_after_age": None, "kvrocks_after_compact": None})


class AdvertisedMetricsTest(unittest.TestCase):
    def test_u1_open_p50_is_verdicted(self):
        run = Run(self)
        for rep in (1, 2):
            for srv, p50 in (("tideskv", 0.2), ("kvrocks", 0.6)):
                doc = memtier(1000, rate=50, clients=10, threads=2)
                doc["ALL STATS"]["Totals"]["Percentile Latencies"]["p50.00"] = p50
                run.cell("U1", srv, rep, "open50", doc)
                run.done("U1", srv, rep)
        s, _ = run.report()
        e = entry(s, "U1", "open50_p50")
        self.assertEqual(e["verdict"]["label"], "TidesKV better")
        self.assertNotIn("open50_p50", [x["key"] for x in s["cases"].get("U3", {}).get("metrics", [])])

    def test_u3_u5_disk_bytes_per_op(self):
        run = Run(self)
        for rep in (1, 2):
            for srv, io in (("tideskv", 100000), ("kvrocks", 300000)):
                run.cell("U3", srv, rep, "closed", memtier(1000, sets=800, gets=200))
                run.write("U3/%s/rep%d/closed.io" % (srv, rep), str(io))
                run.done("U3", srv, rep)
                run.cell("U5", srv, rep, "v4k", memtier(1000, sets=200, gets=800))
                run.write("U5/%s/rep%d/v4k.io" % (srv, rep), str(io * 10))
                run.done("U5", srv, rep)
        s, text = run.report()
        e = entry(s, "U3", "closed_bop")
        self.assertEqual(e["configs"]["tideskv"]["median"], 100.0)  # 100000 bytes / 1000 ops
        self.assertEqual(e["verdict"]["label"], "TidesKV better")
        self.assertFalse(e["higher_is_better"])
        self.assertEqual(entry(s, "U5", "v4k_bop")["configs"]["kvrocks"]["median"], 3000.0)
        self.assertIn("disk writes per op", text)

    def test_u6_ratio_to_the_floor_and_both_floors(self):
        run = Run(self, reps=1)
        for srv, floor in (("tideskv", 400.0), ("kvrocks", 500.0)):
            run.write("U6/%s/rep1/fio/fsync.json" % srv, fio_doc(250.0))
            run.write("U6/%s/rep1/fio/append.json" % srv, fio_doc(floor))
            run.cell("U6", srv, 1, "c64", memtier(1000, sets=100, gets=0))
            run.done("U6", srv, 1)
        run.write("fio/append.json", fio_doc(700.0))
        run.write("fio/fsync.json", fio_doc(350.0))
        s, text = run.report()
        e = entry(s, "U6", "c64_floor")
        self.assertEqual(e["configs"]["tideskv"]["median"], 2.5)
        self.assertTrue(e["info"])
        u6 = text.split("### U6")[1].split("## F1")[0]
        self.assertIn("fio reference (4 KiB random write + fsync)", u6)
        self.assertIn("durable-append floor (4 KiB sequential write + fdatasync)", u6)
        self.assertIn("400 writes/s", u6)
        self.assertIn("2.50x", u6)
        meth = text.split("## Methodology")[1]
        self.assertIn("durable-append floor (4 KiB sequential write + fdatasync", meth)
        self.assertIn("700 writes/s", meth)
        self.assertIn("per-ack fsync is not observed by the suite", meth)


class CensoredQuietTest(unittest.TestCase):
    def u8(self, run, t, k, tcap=False, kcap=False):
        run.write("meta.json", json.dumps({"mode": "full", "reps": 2, "quiesce_window_s": 5, "quiesce_max_s": 600}))
        for rep in (1, 2):
            run.write("U8/tideskv/rep%d/summary.json" % rep, u8_doc(t + rep, tcap))
            run.write("U8/kvrocks/rep%d/summary.json" % rep, u8_doc(k + rep, kcap))
            run.done("U8", "tideskv", rep); run.done("U8", "kvrocks", rep)
        return run.report()

    def test_capped_side_shows_lower_bound_not_err(self):
        s, text = self.u8(Run(self), 20, 599, kcap=True)
        e = entry(s, "U8", "settle_s")
        self.assertFalse(e["configs"]["kvrocks"]["errors"])
        self.assertEqual(e["verdict"]["label"], "TidesKV better")
        self.assertIn("≥ 600 s", text)
        self.assertFalse(s["failed"])

    def test_both_capped_has_no_verdict(self):
        s, _ = self.u8(Run(self), 599, 599, tcap=True, kcap=True)
        v = entry(s, "U8", "settle_s")["verdict"]
        self.assertEqual((v["label"], v["note"]), ("no verdict", "both capped"))

    def test_capped_side_inside_the_band_has_no_verdict(self):
        s, _ = self.u8(Run(self), 580, 599, kcap=True)
        v = entry(s, "U8", "settle_s")["verdict"]
        self.assertEqual(v["label"], "no verdict")
        self.assertIn("capped", v["note"])


class ReportWordingTest(unittest.TestCase):
    def test_compression_is_observed_not_assumed(self):
        run = Run(self, reps=1)
        for srv, alloc, wal in (("tideskv", 2660, 0), ("kvrocks", 2100, 1000)):
            du5(run, "U1", srv, 1, "du-after-load", alloc + wal, wal)
            load_cfg(run, "U1", srv, 1, "load", 2, 1000)
            run.cell("U1", srv, 1, "closed-gauss", memtier(1000))
            run.done("U1", srv, 1)
        s, text = run.report()
        self.assertNotIn("3x", text)
        self.assertEqual(s["cases"]["U1"]["observed"]["tideskv"]["after load"], 1.33)
        meth = text.split("## Methodology")[1]
        self.assertIn("LZ4 is on for both servers", meth)
        self.assertIn("observed on-disk / logical", meth)
        self.assertIn("U1 TidesKV 1.33, Kvrocks 1.05", meth)

    def test_reason_lists_every_non_tie_metric(self):
        run = Run(self)
        for rep in (1, 2):
            for srv, ops, io in (("tideskv", 1000, 300000), ("kvrocks", 1000, 100000)):
                run.cell("U5", srv, rep, "v4k", memtier(ops + rep, sets=200, gets=800))
                run.write("U5/%s/rep%d/v4k.io" % (srv, rep), str(io))
                run.done("U5", srv, rep)
        s, text = run.report()
        row = next(l for l in text.splitlines() if l.startswith("| U5"))
        self.assertIn("| tie |", row)  # the tie headline and one Kvrocks win: not mixed (only one side wins)
        self.assertIn("v4k disk writes per op", row)
        self.assertIn("Kvrocks better", row)

    def test_evict_failure_is_a_caveat(self):
        run = Run(self, reps=1)
        for srv in ("tideskv", "kvrocks"):
            u7_point(run, srv, "term-20", acked=20, status="ok")
            run.done("U7", srv, 1)
        run.write("U7/kvrocks/rep1/term-20/evict-failed", "restart: could not drop the data dir from the page cache\n")
        s, text = run.report()
        u7 = text.split("### U7")[1].split("## F1")[0]
        self.assertIn("Kvrocks rep1 term-20: restart: could not drop the data dir from the page cache", u7)
        self.assertFalse(s["failed"])

    def test_cache_hit_reads_closed_gauss_and_u4_shows_command_weights(self):
        import uc_metrics
        self.assertEqual(next(m for m in uc_metrics.METRICS["U1"] if m.key == "cache_hit").phase, "closed-gauss")
        run = Run(self)
        from test_uc_report import u4
        u4(run, 1000, 500)
        _, text = run.report()
        u4s = text.split("### U4")[1].split("## F1")[0]
        self.assertIn("commands HGET 10, HSET 6, HGETALL 3, HDEL 1 (weights)", u4s)
        self.assertNotIn("ratio 1:9", u4s)
        self.assertIn("HDEL runs without a paired HSET", u4s)

    def test_methodology_names_the_checked_replies(self):
        run = Run(self)
        from test_uc_report import u4
        u4(run, 1000, 500)
        _, text = run.report()
        item = text.split("**Correctness.**")[1].split("8. **")[0]
        for needle in ("`+OK`", "HSET and HDEL", "U2's GET replies", "U4's HGET and HGETALL replies"):
            self.assertIn(needle, item)
        self.assertNotIn("any error reply", item)


if __name__ == "__main__":
    unittest.main()
