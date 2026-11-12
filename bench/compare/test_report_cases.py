"""Unit tests for report_cases.py (run: python3 -m unittest bench/compare/test_report_cases.py)."""
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import report  # noqa: E402
import report_cases  # noqa: E402
import test_report  # noqa: E402

CASES = ["C%d" % i for i in range(1, 9)]
SERVERS = ("tideskv", "kvrocks")


def memtier(path, ops, p50=0.1, p99=0.5, p999=1.0, count=None, duration_ms=30000):
    count = count if count is not None else int(ops * duration_ms / 1000)
    doc = {"ALL STATS": {"Runtime": {"Total duration": duration_ms},
                         "Totals": {"Count": count, "Ops/sec": ops,
                                    "Percentile Latencies": {"p50.00": p50, "p99.00": p99, "p99.90": p999}}}}
    with open(path, "w") as f:
        json.dump(doc, f)


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def make_cases_run(root, cases=CASES, repeats=3, rate=10000, diff=None, ops=None):
    """A complete cases run; ops maps (server, case) to the ops/s of every repeat (default 1000)."""
    ops = ops or {}
    write(os.path.join(root, "meta.json"), json.dumps({
        "mode": "cases", "case_repeats": repeats, "case_rate": rate, "tideskv_sha": "abc1234",
        "kvrocks_image": "apache/kvrocks:2.17.0", "memtier_image": "m", "size": "quick", "relaxed_mode": "none",
        "warmup_s": 10, "duration_s": 30, "server_cpus": "0-3", "server_mem": "8g", "host": "test",
        "started_utc": "t", "w6_del_target_per_s": 0}))
    write(os.path.join(root, "cases-diff.json"), json.dumps(diff or {c: "pass" for c in cases}))
    for s in SERVERS:
        sd = os.path.join(root, s)
        os.makedirs(sd, exist_ok=True)
        write(os.path.join(sd, "config.txt"), "workers 4\n")
        write(os.path.join(sd, "disk.json"), json.dumps(
            {"after_load_bytes": 2 << 30, "end_bytes": 3 << 30, "load_seconds": 60, "logical_bytes": 1 << 30}))
        write(os.path.join(sd, "expiry.json"), json.dumps(
            {"keys_loaded": 1000, "keys_live_expected": 800, "keys_other": 5000, "dbsize_after": 5800,
             "bytes_before": 4 << 30, "bytes_after": 3 << 30, "waited_s": 70}))
        for lv in ("relaxed", "strict"):
            d = os.path.join(sd, lv)
            os.makedirs(d)
            for c in cases:
                for k in range(1, repeats + 1):
                    _repeat(d, c, k, ops.get((s, c), 1000))
                if c != "C8":
                    _single(d, c + ".rate", 9400)
                write(os.path.join(d, c + ".du"), str(1 << 30))


def _single(d, name, ops, **kw):
    memtier(os.path.join(d, name + ".json"), ops, **kw)
    write(os.path.join(d, name + ".status"), "ok\n")
    write(os.path.join(d, name + ".cpu"), "30000000\n")
    write(os.path.join(d, name + ".stats.jsonl"), '{"CPUPerc":"150.00%","MemUsage":"1.5GiB / 8GiB"}\n')


def _repeat(d, c, k, ops):
    name = "%s.r%d" % (c, k)
    if c != "C8":
        _single(d, name, ops)
        return
    for part in ("c1", "c2", "c5", "scan"):
        memtier(os.path.join(d, "%s.%s.json" % (name, part)), ops if part != "scan" else 1, p99=0.5)
    write(os.path.join(d, name + ".status"), "ok\n")
    write(os.path.join(d, name + ".cpu"), "30000000\n")
    write(os.path.join(d, name + ".stats.jsonl"), '{"CPUPerc":"150.00%","MemUsage":"1.5GiB / 8GiB"}\n')


def cell(cells, server, level, case):
    return next(c for c in cells if (c["server"], c["level"], c["case"]) == (server, level, case))


class CasesReportTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, True)

    def collect(self):
        meta, _, _, _ = report.load(self.root)
        return meta, report_cases.collect(self.root, meta)

    def render(self):
        meta, cells = self.collect()
        _, _, disks, configs = report.load(self.root)
        return report_cases.render(meta, cells, report_cases.load_expiry(self.root), disks, configs, self.root)

    def test_median_of_three_by_ops(self):
        make_cases_run(self.root, cases=["C1"])
        d = os.path.join(self.root, "tideskv", "relaxed")
        for k, (ops, p99) in enumerate(((100, 0.1), (300, 0.3), (200, 0.2)), 1):
            _single(d, "C1.r%d" % k, ops, p99=p99)
        _, cells = self.collect()
        c = cell(cells, "tideskv", "relaxed", "C1")
        self.assertEqual(c["ops"], 200)
        self.assertAlmostEqual(c["p99"], 0.2)
        self.assertEqual(c["runs"], 3)
        self.assertIsNone(c["error"])

    def test_ratio_noise_band(self):
        make_cases_run(self.root, cases=["C1", "C2"], ops={("tideskv", "C1"): 1040, ("kvrocks", "C1"): 1000,
                                                           ("tideskv", "C2"): 1060, ("kvrocks", "C2"): 1000})
        text = self.render()
        row1 = next(l for l in text.splitlines() if l.startswith("| C1 | 1040"))
        row2 = next(l for l in text.splitlines() if l.startswith("| C2 | 1060"))
        self.assertIn("| ≈ |", row1)
        self.assertIn("| 1.06 |", row2)

    def test_diverged_case_has_no_numbers(self):
        make_cases_run(self.root, diff={**{c: "pass" for c in CASES}, "C3": "diverged"})
        _single(os.path.join(self.root, "tideskv", "relaxed"), "C3.r1", 777777)
        meta, cells = self.collect()
        c = cell(cells, "tideskv", "relaxed", "C3")
        self.assertEqual(c["status"], "diverged")
        self.assertIsNone(c["ops"])
        text = self.render()
        row = next(l for l in text.splitlines() if l.startswith("| C3 |") and "diverged" in l and "|" in l)
        self.assertNotIn("777777", text)
        self.assertNotIn("ERR", row)

    def test_c8_sums_parts_and_takes_worst_p99(self):
        make_cases_run(self.root, cases=["C8"], repeats=1)
        d = os.path.join(self.root, "tideskv", "relaxed")
        for part, ops, p99 in (("c1", 100, 0.5), ("c2", 200, 2.5), ("c5", 300, 1.5), ("scan", 1, 9.0)):
            memtier(os.path.join(d, "C8.r1.%s.json" % part), ops, p99=p99)
        _, cells = self.collect()
        c = cell(cells, "tideskv", "relaxed", "C8")
        self.assertEqual(c["ops"], 600)
        self.assertAlmostEqual(c["p99"], 2.5)
        self.assertAlmostEqual(c["scan_p99"], 9.0)
        self.assertIsNone(c["error"])

    def test_c8_missing_part_is_err(self):
        make_cases_run(self.root, cases=["C8"], repeats=1)
        os.remove(os.path.join(self.root, "kvrocks", "strict", "C8.r1.c2.json"))
        _, cells = self.collect()
        c = cell(cells, "kvrocks", "strict", "C8")
        self.assertIsNotNone(c["error"])
        self.assertIsNone(c["ops"])
        text = self.render()
        self.assertIn("Errors:", text)
        self.assertIn("Kvrocks strict C8: r1:", text)

    def test_fixed_rate_reports_achieved_ops(self):
        make_cases_run(self.root, cases=["C1"], rate=10000)
        text = self.render()
        row = next(l for l in text.splitlines() if l.startswith("| C1 | 10000 |"))
        self.assertIn("| 9400 | 9400 |", row)

    def test_expiry_table_bytes_freed(self):
        make_cases_run(self.root, cases=["C1"])
        d = os.path.join(self.root, "kvrocks", "expiry.json")
        write(d, json.dumps({"keys_loaded": 1000, "keys_live_expected": 800, "keys_other": 5000,
                             "dbsize_after": 5900, "bytes_before": 4 << 30, "bytes_after": 3 << 30, "waited_s": 70}))
        text = self.render()
        self.assertIn("## Expiry", text)
        self.assertIn("| TidesKV | 1000 | 800 | 5000 | 5800 | 5800 | 0 | 4.00 GiB | 3.00 GiB | 1.00 GiB |", text)
        self.assertIn("| Kvrocks | 1000 | 800 | 5000 | 5800 | 5900 | 100 | 4.00 GiB | 3.00 GiB | 1.00 GiB |", text)

    def test_missing_expiry_is_err_not_raise(self):
        make_cases_run(self.root, cases=["C1"])
        os.remove(os.path.join(self.root, "tideskv", "expiry.json"))
        text = self.render()
        self.assertIn("TidesKV expiry", text)

    def test_c8_row_first(self):
        make_cases_run(self.root)
        text = self.render()
        lines = [l for l in text.splitlines() if l.startswith("| C")]
        thr = text.split("## Relaxed")[1].split("\n\n")[2].splitlines()
        self.assertTrue(thr[2].startswith("| C8 |"), thr[:4])
        self.assertTrue(thr[3].startswith("| C1 |"))
        self.assertTrue(lines)

    def test_correctness_section(self):
        make_cases_run(self.root, diff={**{c: "pass" for c in CASES}, "C4": "diff did not run"})
        text = self.render()
        self.assertIn("## Correctness", text)
        self.assertIn("| C4 | diff did not run |", text)
        self.assertIn("## Fairness", text)
        self.assertIn("## Disk and load", text)

    def test_exit_code(self):
        make_cases_run(self.root)
        self.assertEqual(report.main([self.root]), 0)
        bad = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, bad, True)
        make_cases_run(bad, diff={**{c: "pass" for c in CASES}, "C2": "diverged"})
        self.assertEqual(report.main([bad]), 1)

    def test_dispatch_from_report_main(self):
        make_cases_run(self.root)
        self.assertEqual(report.main([self.root]), 0)
        with open(os.path.join(self.root, "report.md")) as f:
            text = f.read()
        self.assertIn("## Correctness", text)
        with open(os.path.join(self.root, "summary.json")) as f:
            self.assertIn("cells", json.load(f))

    def test_w_mode_report_unchanged(self):
        test_report.make_run(self.root)
        meta, cells, disks, configs = report.load(self.root)
        with open(os.path.join(HERE, "testdata", "report-w-golden.md")) as f:
            self.assertEqual(report.render(meta, cells, disks, configs, self.root), f.read())


if __name__ == "__main__":
    unittest.main()
