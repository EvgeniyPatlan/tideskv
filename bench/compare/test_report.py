"""Unit tests for report.py (run: python3 -m unittest bench/compare/test_report.py)."""
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import report  # noqa: E402

SETGET = os.path.join(HERE, "testdata", "memtier-setget.json")
COMMAND = os.path.join(HERE, "testdata", "memtier-command.json")


def make_run(root, servers=("tideskv", "kvrocks"), levels=("relaxed", "strict"),
             workloads=("W1", "W2", "W3", "W4", "W5", "W6")):
    with open(os.path.join(root, "meta.json"), "w") as f:
        json.dump({"tideskv_sha": "abc1234", "kvrocks_image": "apache/kvrocks:2.17.0",
                   "memtier_image": "redislabs/memtier_benchmark:2.1.4", "size": "quick",
                   "relaxed_mode": "none", "warmup_s": 10, "duration_s": 30,
                   "server_cpus": "0-3", "server_mem": "8g", "host": "test", "started_utc": "t"}, f)
    for s in servers:
        os.makedirs(os.path.join(root, s), exist_ok=True)
        with open(os.path.join(root, s, "disk.json"), "w") as f:
            json.dump({"after_load_bytes": 2 << 30, "end_bytes": 3 << 30, "load_seconds": 60,
                       "logical_bytes": 1 << 30}, f)
        with open(os.path.join(root, s, "config.txt"), "w") as f:
            f.write("workers 4\n")
        for lv in levels:
            d = os.path.join(root, s, lv)
            os.makedirs(d, exist_ok=True)
            for w in workloads:
                shutil.copy(COMMAND if w in ("W3", "W4", "W5") else SETGET, os.path.join(d, w + ".json"))
                with open(os.path.join(d, w + ".stats.jsonl"), "w") as f:
                    f.write('{"CPUPerc":"150.00%","MemUsage":"1.5GiB / 8GiB"}\n')
                    f.write('{"CPUPerc":"250.00%","MemUsage":"2GiB / 8GiB"}\n')
                with open(os.path.join(d, w + ".status"), "w") as f:
                    f.write("ok\n")
            shutil.copy(SETGET, os.path.join(d, "W6-del.json"))


class ParseTest(unittest.TestCase):
    def test_parse_memtier_totals(self):
        m = report.parse_memtier(SETGET)
        self.assertEqual(m["count"], 416298)
        self.assertAlmostEqual(m["ops"], 138764.01)
        self.assertAlmostEqual(m["p50"], 0.015)
        self.assertAlmostEqual(m["p99"], 0.039)
        self.assertAlmostEqual(m["p999"], 0.135)
        self.assertAlmostEqual(m["duration_s"], 3.0)

    def test_parse_memtier_command_mode(self):
        m = report.parse_memtier(COMMAND)
        self.assertEqual(m["count"], 419824)

    def test_parse_mem_units(self):
        self.assertEqual(report.parse_mem("1.5GiB / 8GiB"), int(1.5 * (1 << 30)))
        self.assertEqual(report.parse_mem("512MiB / 8GiB"), 512 << 20)
        self.assertEqual(report.parse_mem("100KiB / 8GiB"), 100 << 10)
        self.assertEqual(report.parse_mem("12B / 8GiB"), 12)

    def test_parse_stats_mean_cpu_and_peak_mem(self):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, "s.jsonl")
            with open(p, "w") as f:
                f.write('{"CPUPerc":"150.00%","MemUsage":"1.5GiB / 8GiB"}\n')
                f.write('{"CPUPerc":"250.00%","MemUsage":"2GiB / 8GiB"}\n')
            s = report.parse_stats(p)
            self.assertAlmostEqual(s["cpu_mean_pct"], 200.0)
            self.assertEqual(s["mem_peak_bytes"], 2 << 30)

    def test_cpu_us_per_op(self):
        # 200 % for 3 s = 6 CPU-seconds over 600000 ops = 10 us/op
        self.assertAlmostEqual(report.cpu_us_per_op(200.0, 3.0, 600000), 10.0)
        self.assertIsNone(report.cpu_us_per_op(200.0, 3.0, 0))


class CollectRenderTest(unittest.TestCase):
    def test_full_run_has_24_ok_cells(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            cells = report.collect(t)
            self.assertEqual(len(cells), 24)
            self.assertTrue(all(c["error"] is None for c in cells))

    def test_missing_json_marks_err(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            os.remove(os.path.join(t, "kvrocks", "strict", "W4.json"))
            cells = report.collect(t)
            bad = [c for c in cells if c["error"]]
            self.assertEqual(len(bad), 1)
            self.assertEqual((bad[0]["server"], bad[0]["level"], bad[0]["workload"]), ("kvrocks", "strict", "W4"))

    def test_status_text_marks_err(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            with open(os.path.join(t, "tideskv", "relaxed", "W3.status"), "w") as f:
                f.write("probe: WRONGTYPE Operation against a key\n")
            bad = [c for c in report.collect(t) if c["error"]]
            self.assertEqual(bad[0]["error"], "probe: WRONGTYPE Operation against a key")

    def test_zero_count_marks_err(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            p = os.path.join(t, "tideskv", "relaxed", "W1.json")
            d = json.load(open(p))
            d["ALL STATS"]["Totals"]["Count"] = 0
            json.dump(d, open(p, "w"))
            bad = [c for c in report.collect(t) if c["error"]]
            self.assertEqual(bad[0]["error"], "memtier recorded no requests")

    def test_render_tables_ratio_and_err(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            os.remove(os.path.join(t, "kvrocks", "strict", "W4.json"))
            meta, cells, disks, configs = report.load(t)
            md = report.render(meta, cells, disks, configs)
            self.assertIn("## Relaxed durability (TidesKV `sync-mode none` vs Kvrocks `rocksdb.write_options.sync no`)", md)
            self.assertIn("## Strict durability (TidesKV `sync-mode full` vs Kvrocks `rocksdb.write_options.sync yes`)", md)
            self.assertIn("| W | TidesKV ops/s | Kvrocks ops/s | ratio | TidesKV p50/p99/p999 ms | Kvrocks p50/p99/p999 ms |", md)
            self.assertIn("| W1 | 138764 | 138764 | 1.00 | 0.015 / 0.039 / 0.135 | 0.015 / 0.039 / 0.135 |", md)
            self.assertIn("| W4 | 139936 | ERR | – | 0.015 / 0.031 / 0.119 | ERR |", md)
            self.assertIn("## Fairness", md)
            self.assertIn("workers 4", md)

    def test_main_writes_files_and_exit_status(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            self.assertEqual(report.main([t]), 0)
            self.assertTrue(os.path.exists(os.path.join(t, "report.md")))
            summary = json.load(open(os.path.join(t, "summary.json")))
            self.assertEqual(len(summary["cells"]), 24)

    def test_exit_status_one_when_any_cell_err(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            os.remove(os.path.join(t, "tideskv", "strict", "W6.json"))
            self.assertEqual(report.main([t]), 1)
            self.assertTrue(os.path.exists(os.path.join(t, "report.md")))  # partial report still written

    def test_partial_run_with_one_server(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t, servers=("tideskv",))
            cells = report.collect(t)
            self.assertEqual(len([c for c in cells if c["error"] == "not run"]), 12)


if __name__ == "__main__":
    unittest.main()
