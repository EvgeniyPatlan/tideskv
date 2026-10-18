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
            with open(p) as f:
                d = json.load(f)
            d["ALL STATS"]["Totals"]["Count"] = 0
            with open(p, "w") as f:
                json.dump(d, f)
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
            with open(os.path.join(t, "summary.json")) as f:
                summary = json.load(f)
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


class FailedServerTest(unittest.TestCase):
    def test_failed_server_shows_reason_and_log_tail(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t, servers=("tideskv",))
            os.makedirs(os.path.join(t, "kvrocks"))
            with open(os.path.join(t, "kvrocks", "run.failed"), "w") as f:
                f.write("parity: kvrocks workers is [8], expected [4]\n")
            with open(os.path.join(t, "kvrocks", "server.log"), "w") as f:
                f.write("".join("log line %d\n" % i for i in range(1, 31)))
            self.assertEqual(report.main([t]), 1)
            with open(os.path.join(t, "report.md")) as f:
                md = f.read()
            self.assertIn("parity: kvrocks workers is [8], expected [4]", md)
            self.assertIn("log line 30", md)
            self.assertIn("log line 11", md)
            self.assertNotIn("log line 10\n", md)
            self.assertIn("not run: parity", md)


class RobustnessTest(unittest.TestCase):
    def _write(self, t, rel, text):
        with open(os.path.join(t, *rel.split("/")), "w") as f:
            f.write(text)

    def _cell(self, cells, server="tideskv", level="relaxed", w="W1"):
        return [c for c in cells if (c["server"], c["level"], c["workload"]) == (server, level, w)][0]

    def test_empty_stats_file_keeps_ops_and_dashes(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            self._write(t, "tideskv/relaxed/W1.stats.jsonl", "")
            c = self._cell(report.collect(t))
            self.assertIsNone(c["error"])
            self.assertIsNone(c["cpu_us_per_op"])
            self.assertIsNone(c["mem_peak_bytes"])
            self.assertEqual(report.main([t]), 0)

    def test_truncated_stats_line_is_skipped(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            self._write(t, "tideskv/relaxed/W1.stats.jsonl",
                        '{"CPUPerc":"200.00%","MemUsage":"2GiB / 8GiB"}\n{"CPUPerc":"15')
            c = self._cell(report.collect(t))
            self.assertIsNone(c["error"])
            self.assertIsNotNone(c["cpu_us_per_op"])
            self.assertEqual(c["mem_peak_bytes"], 2 << 30)

    def test_dashes_in_stats_leave_dashes(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            self._write(t, "tideskv/relaxed/W1.stats.jsonl", '{"CPUPerc":"--","MemUsage":"-- / --"}\n')
            c = self._cell(report.collect(t))
            self.assertIsNone(c["error"])
            self.assertIsNone(c["cpu_us_per_op"])
            self.assertIsNone(c["mem_peak_bytes"])
            self.assertEqual(report.main([t]), 0)

    def test_empty_status_is_error(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            self._write(t, "tideskv/relaxed/W1.status", "\n")
            c = self._cell(report.collect(t))
            self.assertEqual(c["error"], "empty status")
            self.assertEqual(report.main([t]), 1)
            with open(os.path.join(t, "report.md")) as f:
                self.assertIn("ERR", f.read())

    def test_non_object_memtier_json_is_error(self):
        for body in ("[]", "42", "{}", '{"ALL STATS": []}'):
            with tempfile.TemporaryDirectory() as t:
                make_run(t)
                self._write(t, "tideskv/relaxed/W1.json", body)
                c = self._cell(report.collect(t))
                self.assertTrue(c["error"].startswith("unreadable memtier result: "), body)
                self.assertEqual(report.main([t]), 1)

    def test_bad_del_json_leaves_none(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            self._write(t, "tideskv/relaxed/W6-del.json", "{not json")
            c = self._cell(report.collect(t), w="W6")
            self.assertIsNone(c["error"])
            self.assertIsNone(c["del_ops"])
            self.assertEqual(report.main([t]), 0)

    def test_missing_meta_still_writes_report(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            os.remove(os.path.join(t, "meta.json"))
            self.assertEqual(report.main([t]), 0)
            self.assertTrue(os.path.exists(os.path.join(t, "report.md")))
            os.remove(os.path.join(t, "kvrocks", "strict", "W4.json"))
            self.assertEqual(report.main([t]), 1)

    def test_malformed_meta_still_writes_report(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            self._write(t, "meta.json", "{broken")
            self.assertEqual(report.main([t]), 0)
            with open(os.path.join(t, "report.md")) as f:
                self.assertIn("?", f.read())

    def test_bad_and_missing_disk_rows(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            self._write(t, "tideskv/disk.json", "{bad")
            os.remove(os.path.join(t, "kvrocks", "disk.json"))
            self.assertEqual(report.main([t]), 0)
            with open(os.path.join(t, "report.md")) as f:
                md = f.read()
            self.assertIn("| TidesKV | unreadable |", md)
            self.assertIn("| Kvrocks | not run |", md)

    def test_non_finite_or_wrong_typed_memtier_numbers_are_error(self):
        for key, val in (("Ops/sec", "abc"), ("Ops/sec", None), ("Ops/sec", True),
                         ("Ops/sec", float("nan")), ("Count", "x"), ("Count", float("inf"))):
            with tempfile.TemporaryDirectory() as t:
                make_run(t)
                p = os.path.join(t, "tideskv", "relaxed", "W1.json")
                with open(p) as f:
                    d = json.load(f)
                d["ALL STATS"]["Totals"][key] = val
                with open(p, "w") as f:
                    json.dump(d, f)
                c = self._cell(report.collect(t))
                self.assertTrue(str(c["error"]).startswith("unreadable memtier result: "), (key, val))
                self.assertEqual(report.main([t]), 1)
        with tempfile.TemporaryDirectory() as t:  # bad DEL ops leaves del_ops None
            make_run(t)
            p = os.path.join(t, "tideskv", "relaxed", "W6-del.json")
            with open(p) as f:
                d = json.load(f)
            d["ALL STATS"]["Totals"]["Ops/sec"] = "x"
            with open(p, "w") as f:
                json.dump(d, f)
            self.assertIsNone(self._cell(report.collect(t), w="W6")["del_ops"])
            self.assertEqual(report.main([t]), 0)

    def test_disk_json_overflow_and_wrong_types_are_unreadable(self):
        for body in ('{"after_load_bytes":1,"end_bytes":1,"load_seconds":1e999,"logical_bytes":1}',
                     '{"after_load_bytes":1,"end_bytes":1,"load_seconds":1,"logical_bytes":"0"}',
                     '{"after_load_bytes":1,"end_bytes":1e999,"load_seconds":1,"logical_bytes":1e-999}'):
            with tempfile.TemporaryDirectory() as t:
                make_run(t)
                self._write(t, "tideskv/disk.json", body)
                self.assertEqual(report.main([t]), 0, body)
                with open(os.path.join(t, "report.md")) as f:
                    self.assertIn("| TidesKV | unreadable |", f.read())

    def test_non_utf8_status_config_meta_do_not_crash(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            for rel in ("tideskv/relaxed/W1.status", "tideskv/config.txt", "meta.json"):
                with open(os.path.join(t, *rel.split("/")), "wb") as f:
                    f.write(b"\x00\xff\xfe")
            self.assertEqual(report.main([t]), 1)
            self.assertIsNotNone(self._cell(report.collect(t))["error"])
            self.assertTrue(os.path.exists(os.path.join(t, "report.md")))


if __name__ == "__main__":
    unittest.main()


def _set_meta(root, **kv):
    p = os.path.join(root, "meta.json")
    with open(p) as f:
        meta = json.load(f)
    meta.update(kv)
    with open(p, "w") as f:
        json.dump(meta, f)


def _del_ops(root, server, level, ops):
    with open(SETGET) as f:
        d = json.load(f)
    d["ALL STATS"]["Totals"]["Ops/sec"] = ops
    with open(os.path.join(root, server, level, "W6-del.json"), "w") as f:
        json.dump(d, f)


def _write(root, rel, text):
    with open(os.path.join(root, *rel.split("/")), "w") as f:
        f.write(text)


def _cellof(cells, server, level, w):
    return [c for c in cells if (c["server"], c["level"], c["workload"]) == (server, level, w)][0]


class FinalFixTest(unittest.TestCase):
    def test_cpu_from_cgroup_delta_wins_over_stats(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            _write(t, "tideskv/relaxed/W1.cpu", "9000000\n")  # usage_usec over the timed run
            c = _cellof(report.collect(t), "tideskv", "relaxed", "W1")
            self.assertAlmostEqual(c["cpu_us_per_op"], 9000000 / 416298.0)
            self.assertEqual(c["mem_peak_bytes"], 2 << 30)  # memory still comes from docker stats

    def test_bad_cpu_file_falls_back_to_stats(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            _write(t, "tideskv/relaxed/W1.cpu", "garbage\n")
            c = _cellof(report.collect(t), "tideskv", "relaxed", "W1")
            self.assertAlmostEqual(c["cpu_us_per_op"], 200.0 / 100 * 3.0 * 1e6 / 416298)

    def test_settle_column_in_cpu_table(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            _write(t, "tideskv/relaxed/W2.settle", "7\n")
            _write(t, "kvrocks/relaxed/W2.settle", "60 capped\n")
            meta, cells, disks, configs = report.load(t)
            c = _cellof(cells, "kvrocks", "relaxed", "W2")
            self.assertEqual((c["settle_s"], c["settle_capped"]), (60, True))
            md = report.render(meta, cells, disks, configs)
            self.assertIn("| W | TidesKV CPU µs/op | Kvrocks CPU µs/op | TidesKV peak mem | Kvrocks peak mem "
                          "| TidesKV settle s | Kvrocks settle s |", md)
            self.assertIn("| 7 | ≥60 (cap) |", md)
            self.assertIn("| – | – |", md)  # cells without a .settle file
            self.assertEqual(report.main([t]), 0)  # a capped settle is not an error

    def test_garbage_settle_is_dash(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            _write(t, "tideskv/relaxed/W1.settle", "soon\n")
            c = _cellof(report.collect(t), "tideskv", "relaxed", "W1")
            self.assertIsNone(c["settle_s"])

    def test_load_and_restart_settle_in_disk_table(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            _write(t, "tideskv/settle-load", "12\n")
            _write(t, "tideskv/settle-restart", "600 capped\n")
            md = report.render(*report.load(t))
            self.assertIn("| settle after load | settle after restart |", md)
            self.assertIn("| 60s | 12 | ≥600 (cap) |", md)

    def test_w6_target_vs_achieved_and_shortfall_note(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            _set_meta(t, w6_del_target_per_s=20, w6_del_clients=20)
            _del_ops(t, "tideskv", "relaxed", 4.13)
            _del_ops(t, "kvrocks", "relaxed", 19.6)
            md = report.render(*report.load(t))
            self.assertIn("W6 delete traffic (target 20 DEL/s over 20 connections): TidesKV 4.1, Kvrocks 19.6 DEL/s.", md)
            self.assertIn("Note: TidesKV relaxed W6 reached 4.1 of the 20 DEL/s target (21%)", md)
            self.assertNotIn("Note: Kvrocks relaxed W6", md)
            self.assertEqual(report.main([t]), 0)  # a shortfall is a note, not an ERR

    def test_header_shows_client_side_and_quiesce(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            _set_meta(t, threads=4, clients=50, client_cpus="8-11",
                      memtier_run_flags="--distinct-client-seed --randomize", server_nofile=65536,
                      server_cores="0-3 = 2 physical cores", quiesce_cpu_pct=5, quiesce_window_s=5,
                      quiesce_write_bytes=1048576, quiesce_max_s=60)
            md = report.render(*report.load(t))
            self.assertIn("memtier 4 threads × 50 clients on cpus `8-11`, flags `--distinct-client-seed --randomize`", md)
            self.assertIn("Before each warm-up, after the load and after the strict restart, the harness waits until "
                          "the server uses under 5% of one core and writes at most 1048576 bytes over 5 s "
                          "(cap 60 s); the settle columns show that wait in seconds.", md)
            self.assertIn("Both server containers: cpus `0-3` (0-3 = 2 physical cores), memory `8g`, open files 65536.", md)
