"""Fix-round tests of uc_report (run: python3 -m unittest bench/compare/test_uc_report_fix.py)."""
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uc_cells  # noqa: E402
import uc_metrics  # noqa: E402
from test_uc_report import Run, entry, memtier, u4, with_hashes  # noqa: E402


class FixTest(unittest.TestCase):
    def test_no_closed_loop_latency_metric_has_a_verdict(self):
        for case, metrics in uc_metrics.METRICS.items():
            for m in metrics:
                if m.unit == "ms" and m.phase and not m.phase.startswith("open") and m.phase != "churn":  # churn is fixed-rate
                    self.assertTrue(m.info, "%s %s is closed-loop latency" % (case, m.key))
        run = Run(self)
        for rep in (1, 2):
            run.cell("U6", "tideskv", rep, "c64", memtier(10, p99=1.0, sets=100, gets=0))
            run.cell("U6", "kvrocks", rep, "c64", memtier(10, p99=50.0, sets=100, gets=0))
            run.done("U6", "tideskv", rep); run.done("U6", "kvrocks", rep)
        s, text = run.report()
        e = entry(s, "U6", "c64_p99")
        self.assertIsNone(e["verdict"])
        self.assertTrue(e["info"])
        self.assertNotIn("closed p99", text.split("## Pros and cons")[1].split("## Per-case")[0])

    def test_u3_headline_is_open_loop_p999(self):
        run = Run(self)
        for rep in (1, 2):
            for srv, p in (("tideskv", 1.0), ("kvrocks", 3.0)):
                run.cell("U3", srv, rep, "closed", memtier(1000, p999=p * 10))
                run.cell("U3", srv, rep, "open50", memtier(500, p999=p, rate=25, clients=10, threads=2))
                run.done("U3", srv, rep)
        s, _ = run.report()
        heads = [e["key"] for e in s["cases"]["U3"]["metrics"] if e["headline"]]
        self.assertEqual(heads, ["open50_p999"])
        self.assertEqual(entry(s, "U3", "open50_p999")["verdict"]["label"], "TidesKV better")
        self.assertIsNone(entry(s, "U3", "closed_p999")["verdict"])

    def du5(self, run, server, rep, name, apparent, wal, alloc, wal_alloc, case="U2"):
        run.write("%s/%s/rep%d/%s" % (case, server, rep, name),
                  "epoch_s,bytes,wal_archive_bytes,allocated_bytes,wal_archive_allocated_bytes\n1,%d,%d,%d,%d\n"
                  % (apparent, wal, alloc, wal_alloc))

    def u2_fixture(self, run, with_alloc):
        for rep in (1, 2):
            for srv in ("tideskv", "kvrocks"):
                if with_alloc:
                    self.du5(run, srv, rep, "du-end", 6400, 0, 3000 if srv == "tideskv" else 5000, 0 if srv == "tideskv" else 1000)
                else:
                    run.du("U2", srv, rep, "du-end", 6400, 0)
                run.write("U2/%s/rep%d/du.csv" % (srv, rep), run.read("U2/%s/rep%d/du-end" % (srv, rep)))
                run.write("U2/%s/rep%d/dbsize-end" % (srv, rep), "10")
                run.done("U2", srv, rep)

    def test_allocated_bytes_used_when_column_exists(self):
        run = Run(self)
        self.u2_fixture(run, True)
        s, text = run.report()
        e = entry(s, "U2", "du_end")
        self.assertEqual(e["configs"]["tideskv"]["median"], 3000)
        self.assertEqual(e["configs"]["kvrocks"]["median"], 4000)  # allocated minus the archive's allocated bytes
        self.assertIn("allocated", e["title"])
        self.assertEqual(entry(s, "U2", "du_end_apparent")["configs"]["tideskv"]["median"], 6400)
        self.assertIn("apparent size", text)

    def test_old_du_files_fall_back_to_apparent_size(self):
        run = Run(self)
        self.u2_fixture(run, False)
        s, _ = run.report()
        e = entry(s, "U2", "du_end")
        self.assertEqual(e["configs"]["tideskv"]["median"], 6400)
        self.assertIn("apparent size", e["title"])
        self.assertNotIn("du_end_apparent", [x["key"] for x in s["cases"]["U2"]["metrics"]])

    def test_u2_hit_rate_is_labelled_negative_lookups(self):
        run = Run(self)
        for rep in (1, 2):
            for srv in ("tideskv", "kvrocks"):
                run.cell("U2", srv, rep, "churn", memtier(100, misses=0.9, rate=5, clients=10, threads=2))
                run.done("U2", srv, rep)
        s, text = run.report()
        e = entry(s, "U2", "churn_hit")
        self.assertIn("negative lookups over a 1e9-key space", e["title"])
        self.assertTrue(e["info"])
        self.assertNotIn("expir", e["title"])

    def test_page_cache_banner_not_for_strict_cases(self):
        run = Run(self)
        for case, ph in (("U1", "closed-gauss"), ("U6", "c64")):
            for rep in (1, 2):
                for srv in ("tideskv", "kvrocks"):
                    run.cell(case, srv, rep, ph, memtier(100, sets=100, gets=0 if case == "U6" else 900))
                    run.du(case, srv, rep, "du-after-load", 1 << 20, 0)
                    run.done(case, srv, rep)
        _, text = run.report()
        u1 = text.split("### U1")[1].split("### U6")[0]
        u6 = text.split("### U6")[1].split("## F1")[0]
        self.assertIn("data fits in page cache", u1)
        self.assertNotIn("page cache", u6.split("**Results**")[0])

    def u8(self, run, settle=7, capped=False):
        for srv, wal in (("tideskv", 0), ("kvrocks", 100)):
            doc = {"du_after_load": {"bytes": 1000, "wal_archive_bytes": wal}, "u3_du_after_load": {"bytes": 200, "wal_archive_bytes": wal},
                   "du_after_age": {"bytes": 500, "wal_archive_bytes": wal}, "load_ops_per_s": 5, "settle_load_s": settle,
                   "settle_load_capped": capped, "kvrocks_after_compact": None}
            run.write("U8/%s/rep1/summary.json" % srv, json.dumps(doc))
            run.done("U8", srv, 1)

    def test_u8_u3_growth_and_compact_note(self):
        run = Run(self, reps=1)
        self.u8(run)
        s, text = run.report()
        self.assertEqual(entry(s, "U8", "u3_load")["configs"]["tideskv"]["median"], 200)
        self.assertEqual(entry(s, "U8", "age_growth")["configs"]["tideskv"]["median"], 300)
        self.assertIsNone(entry(s, "U8", "age_growth")["verdict"])
        self.assertIn("after U1's load and both closed-loop phases", text)

    def test_u8_settle_at_floor_has_no_verdict(self):
        run = Run(self, reps=2)
        run.write("meta.json", json.dumps({"mode": "full", "reps": 2, "quiesce_window_s": 5}))
        for rep in (1, 2):
            self.u8(run, settle=5)
            for srv in ("tideskv", "kvrocks"):
                run.write("U8/%s/rep%d/summary.json" % (srv, rep), run.read("U8/%s/rep1/summary.json" % srv))
                run.done("U8", srv, rep)
        s, text = run.report()
        e = entry(s, "U8", "settle_s")
        self.assertEqual(e["floor"], 5)
        self.assertEqual(e["verdict"]["label"], "no verdict")
        self.assertIn("≤ 5 s (floor)", text)

    def test_unstable_u6_cell_withholds_verdict(self):
        steady = {str(i): {"Count": 1000} for i in range(20)}
        jumpy = {str(i): {"Count": 1500 if i % 2 else 6800} for i in range(20)}
        self.assertLess(uc_cells.ops_cv(steady), 0.01)
        self.assertGreater(uc_cells.ops_cv(jumpy), 0.25)
        run = Run(self)
        for rep in (1, 2):
            run.cell("U6", "tideskv", rep, "c64", memtier(4000, sets=100, gets=0, series=jumpy))
            run.cell("U6", "kvrocks", rep, "c64", memtier(1000, sets=100, gets=0, series=steady))
            run.cell("U4", "tideskv", rep, "mix", with_hashes(memtier(4000, series=jumpy)))
            run.cell("U4", "kvrocks", rep, "mix", with_hashes(memtier(1000, series=steady)))
            for c in ("U6", "U4"):
                run.done(c, "tideskv", rep); run.done(c, "kvrocks", rep)
        s, text = run.report()
        e = entry(s, "U6", "c64_ops")
        self.assertEqual(e["verdict"]["label"], "no verdict")
        self.assertIn("unstable", e["verdict"]["note"])
        self.assertIn("unstable", text)
        self.assertEqual(entry(s, "U4", "mix_ops")["verdict"]["label"], "TidesKV better")  # U6 only

    def test_u6_shows_fio_for_each_server(self):
        run = Run(self)
        fio = {"jobs": [{"write": {"bw_bytes": 1, "iops": 1500.0, "lat_ns": {"mean": 1.0}, "clat_ns": {"percentile": {}}},
                         "sync": {"lat_ns": {"mean": 700000.0, "percentile": {"99.000000": 2000000}}}}]}
        for srv, iops in (("tideskv", 1500.0), ("kvrocks", 6800.0)):
            fio["jobs"][0]["write"]["iops"] = iops
            run.write("U6/%s/rep1/fio/fsync.json" % srv, "warning\n" + json.dumps(fio))
            run.cell("U6", srv, 1, "c64", memtier(10, sets=100, gets=0))
            run.done("U6", srv, 1)
        s, text = run.report()
        u6 = text.split("### U6")[1].split("## F1")[0]
        self.assertIn("1500 writes/s", u6)
        self.assertIn("6800 writes/s", u6)
        self.assertIn("writes per fsync: not observable", u6.lower())

    def test_range_values_use_their_own_units(self):
        import uc_report_text as rt
        cell = {"errors": [], "n": 2, "lo": 900 * 1024, "hi": 1.2 * (1 << 20), "median": 1 << 20}
        self.assertEqual(rt.fmt_cell(cell, "bytes"), "1.0 MiB [900.0 KiB–1.2 MiB]".replace("900.0 KiB", "900.0 KiB"))

    def test_settings_grouped_by_level(self):
        run = Run(self)
        for case, ph, level in (("U4", "mix", "relaxed"), ("U6", "c64", "strict")):
            for srv in ("tideskv", "kvrocks"):
                run.write("%s/config.txt" % srv, "# relaxed\nsync-mode none\n# strict\nsync-mode full\n")
                run.cell(case, srv, 1, ph, with_hashes(memtier(10)) if case == "U4" else memtier(10, sets=100, gets=0))
                run.done(case, srv, 1)
        _, text = run.report()
        sect = text.split("### Settings on each side")[1].split("### Not covered")[0]
        self.assertLess(sect.index("Relaxed"), sect.index("Strict"))
        self.assertIn("sync-mode none", sect.split("Strict")[0])
        self.assertNotIn("sync-mode none", sect.split("Strict")[1])

    def test_open_phase_status_ignores_closed_done(self):
        run = Run(self)
        for rep in (1, 2):
            for srv in ("tideskv", "kvrocks"):
                run.cell("U1", srv, rep, "closed-gauss", memtier(1000 if srv == "tideskv" else 500))
                run.cell("U1", srv, rep, "open25", memtier(500, rate=25, clients=10, threads=2))
                run.write("U1/%s/rep%d/closed.done" % (srv, rep), "ok")
                if srv == "tideskv":
                    run.done("U1", srv, rep)  # kvrocks pass B never finished: no done file
        s, _ = run.report()
        self.assertEqual(entry(s, "U1", "closed-gauss_ops")["verdict"]["label"], "TidesKV better")
        e = entry(s, "U1", "open25_p99")
        self.assertTrue(e["configs"]["kvrocks"]["errors"])
        self.assertEqual(e["verdict"]["label"], "no verdict")

    def test_missing_random_read_fio_is_stated(self):
        run = Run(self)
        u4(run, 1000, 500)
        _, text = run.report()
        self.assertIn("fio random-read baseline: not run", text)

    # ---- fix round 2 ----

    def test_ops_cv_ignores_trailing_partial_bucket(self):
        series = {str(i): {"Count": 1000 + (i % 2) * 20} for i in range(20)}
        series["20"] = {"Count": 8}  # memtier's last, partial second
        self.assertLess(uc_cells.ops_cv(series), 0.05)
        series["3"]["Count"] = 6000  # a real swing earlier in the run still counts
        self.assertGreater(uc_cells.ops_cv(series), 0.25)

    def test_ops_cv_too_short_is_none_and_noted(self):
        short = {str(i): {"Count": 1000 if i % 2 else 100} for i in range(5)}  # 4 buckets once the last is dropped
        self.assertIsNone(uc_cells.ops_cv(short))
        run = Run(self)
        for rep in (1, 2):
            run.cell("U6", "tideskv", rep, "c64", memtier(4000, sets=100, gets=0, series=short))
            run.cell("U6", "kvrocks", rep, "c64", memtier(1000, sets=100, gets=0, series=short))
            run.done("U6", "tideskv", rep); run.done("U6", "kvrocks", rep)
        s, text = run.report()
        self.assertEqual(entry(s, "U6", "c64_ops")["verdict"]["label"], "TidesKV better")
        self.assertIn("too short to judge stability", text)

    def test_missing_fio_is_stated_per_server(self):
        run = Run(self)
        fio = {"jobs": [{"write": {"bw_bytes": 1, "iops": 1500.0, "lat_ns": {"mean": 1.0}, "clat_ns": {"percentile": {}}}}]}
        run.write("U6/tideskv/rep1/fio/fsync.json", json.dumps(fio))
        run.write("U6/kvrocks/rep1/fio/fsync.log", "fio: error\nfio: pid=1 failed")
        for srv in ("tideskv", "kvrocks"):
            run.cell("U6", srv, 1, "c64", memtier(10, sets=100, gets=0))
            run.done("U6", srv, 1)
        _, text = run.report()
        u6 = text.split("### U6")[1].split("## F1")[0]
        self.assertIn("fio reference (4 KiB random write + fsync) before Kvrocks's sweep: not measured (", u6)
        self.assertIn("fio: pid=1 failed", u6)
        self.assertIn("1500 writes/s", u6)
        run2 = Run(self)
        for srv in ("tideskv", "kvrocks"):
            run2.cell("U6", srv, 1, "c64", memtier(10, sets=100, gets=0))
            run2.done("U6", srv, 1)
        _, text2 = run2.report()
        self.assertNotIn("was measured right before", text2)
        self.assertIn("fio reference (4 KiB random write + fsync) before TidesKV's sweep: not measured", text2)

    def test_mixed_disk_basis_is_named(self):
        run = Run(self)
        for rep in (1, 2):
            self.du5(run, "tideskv", rep, "du-end", 6400, 0, 3000, 0)
            run.du("U2", "kvrocks", rep, "du-end", 6400, 0)
            for srv in ("tideskv", "kvrocks"):
                run.write("U2/%s/rep%d/du.csv" % (srv, rep), run.read("U2/%s/rep%d/du-end" % (srv, rep)))
                run.write("U2/%s/rep%d/dbsize-end" % (srv, rep), "10")
                run.done("U2", srv, rep)
        s, text = run.report()
        title = entry(s, "U2", "du_end")["title"]
        self.assertIn("mixed: apparent size for Kvrocks", title)
        self.assertIn("du_end_apparent", [x["key"] for x in s["cases"]["U2"]["metrics"]])

    def test_u2_end_state_labels_disk_basis(self):
        run = Run(self)
        self.u2_fixture(run, True)
        _, text = run.report()
        self.assertIn("| disk at end (allocated) |", text)

    def test_methodology_wording(self):
        run = Run(self)
        u4(run, 1000, 500)
        _, text = run.report()
        meth = text.split("## Methodology")[1]
        self.assertIn("allocated bytes (`du --block-size=1`)", meth)
        self.assertIn("apparent size", meth.split("**Correctness")[0].split("**Data.**")[1])
        item3 = meth.split("**Open-loop latency.**")[1].split("**Equal resources")[0]
        self.assertIn("U2 churn p99.9", item3)
        self.assertIn("fixed rate", item3)

    def test_settings_levels_all_printed(self):
        run = Run(self)
        for case, ph in (("U4", "mix"), ("U6", "c64")):
            for srv in ("tideskv", "kvrocks"):
                run.write("%s/config.txt" % srv, "# relaxed\nsync-mode none\n# strict\nsync-mode full\n")
                run.cell(case, srv, 1, ph, with_hashes(memtier(10)) if case == "U4" else memtier(10, sets=100, gets=0))
                run.done(case, srv, 1)
        _, text = run.report()
        sect = text.split("### Settings on each side")[1].split("### Not covered")[0]
        self.assertEqual(sect.count("- kvrocks:"), 2)
        self.assertEqual(sect.count("- tideskv:"), 2)
        self.assertTrue(sect.endswith("\n"))


if __name__ == "__main__":
    unittest.main()
