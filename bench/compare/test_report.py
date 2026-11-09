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


# ---- W7 (run.sh --large) ----
import report_w7  # noqa: E402

W7 = os.path.join(HERE, "testdata", "w7")
W7_SERVERS = ("tideskv", "kvrocks", "kvrocks-blob")
W7_PHASES = ("R1", "R4", "R32", "R32w16", "M32")


def make_w7_run(root, servers=W7_SERVERS, phases=W7_PHASES, client_cpu="200.00%", fio_status="ok"):
    with open(os.path.join(root, "meta.json"), "w") as f:
        json.dump({"tideskv_sha": "abc1234", "kvrocks_image": "apache/kvrocks:2.17.0",
                   "memtier_image": "redislabs/memtier_benchmark:2.1.4", "size": "large", "relaxed_mode": "none",
                   "warmup_s": 60, "duration_s": 180, "server_cpus": "0-3", "server_mem": "4g", "threads": 8,
                   "clients": 4, "client_cpus": "8-11", "client_cpu_count": 4, "w7_keys": 131072,
                   "w7_value_size": 262144, "block_cache_mb": 1024, "w7_phases": " ".join(phases),
                   "fio_status": fio_status, "fio_image": "xridge/fio@sha256:x", "fio_runtime_s": 30,
                   "fio_size": "8g", "host": "test", "started_utc": "t"}, f)
    for s in servers:
        d = os.path.join(root, s, "large")
        os.makedirs(d)
        with open(os.path.join(root, s, "disk.json"), "w") as f:
            json.dump({"after_load_bytes": 34 << 30, "end_bytes": 40 << 30, "load_seconds": 120,
                       "logical_bytes": 32 << 30}, f)
        with open(os.path.join(root, s, "config.txt"), "w") as f:
            f.write("# relaxed load\nworkers 4\n")
        for p in phases:
            shutil.copy(os.path.join(W7, "memtier-m32.json"), os.path.join(d, p + ".json"))
            for ext in ("io0", "io1", "io2", "diskstats", "info0", "info1"):
                shutil.copy(os.path.join(W7, "M32." + ext), os.path.join(d, p + "." + ext))
            for name, text in ((".status", "ok\n"), (".cpu", "89997000\n"), (".settle", "5\n"),
                               (".stats.jsonl", '{"CPUPerc":"390.00%","MemUsage":"3.5GiB / 4GiB"}\n'),
                               (".client.jsonl", '{"CPUPerc":"%s","MemUsage":"20MiB / 62GiB"}\n' % client_cpu)):
                with open(os.path.join(d, p + name), "w") as f:
                    f.write(text)
    os.makedirs(os.path.join(root, "fio"))
    for qd in (1, 8):
        shutil.copy(os.path.join(W7, "fio-qd8.json"), os.path.join(root, "fio", "qd%d.json" % qd))


def _w7(cells, server, phase):
    return [c for c in cells if (c["server"], c["phase"]) == (server, phase)][0]


class W7ParseTest(unittest.TestCase):
    def test_memtier_gets_and_sets(self):
        m = report_w7.parse_memtier_w7(os.path.join(W7, "memtier-m32.json"))
        self.assertEqual((m["get_count"], m["set_count"], m["count"]), (404973, 45015, 449988))
        self.assertAlmostEqual(m["get_ops"], 13498.39)
        self.assertAlmostEqual(m["set_ops"], 1500.42)
        self.assertAlmostEqual(m["get_p999"], 9.151)
        self.assertAlmostEqual(m["set_p99"], 10.239)
        self.assertAlmostEqual(m["duration_s"], 30.002)

    def test_get_only_run_has_no_set_latency(self):
        with open(os.path.join(W7, "memtier-m32.json")) as f:
            d = json.load(f)
        d["ALL STATS"]["Sets"]["Count"] = 0
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, "r.json")
            with open(p, "w") as f:
                json.dump(d, f)
            self.assertIsNone(report_w7.parse_memtier_w7(p)["set_p99"])

    def test_busiest_device_prefers_the_physical_one_of_a_stack(self):
        with open(os.path.join(W7, "M32.io0")) as f0, open(os.path.join(W7, "M32.io1")) as f1:
            deltas = report_w7.io_delta(report_w7.parse_io_stat(f0.read()), report_w7.parse_io_stat(f1.read()))
        self.assertEqual(deltas["259:0"]["rbytes"], 132701552640)
        self.assertEqual(deltas["8:0"]["rbytes"], 4096)  # new device counts from 0
        names = {"259:0": "nvme0n1", "252:0": "dm-0", "252:1": "dm-1", "8:0": "sda"}
        self.assertEqual(report_w7.pick_device(deltas, names), "259:0")
        self.assertEqual(report_w7.pick_device({"252:0": {"rbytes": 10, "wbytes": 0}}, names), "252:0")
        self.assertIsNone(report_w7.pick_device({"259:0": {"rbytes": 0, "wbytes": 0}}, names))

    def test_aqu_sz_and_r_await_from_first_and_last_sample(self):
        with open(os.path.join(W7, "M32.diskstats")) as f:
            samples = report_w7.parse_diskstats(f.read())
        self.assertEqual(len(samples), 3)
        aqu, r_await = report_w7.device_queue(samples, "259:0")
        self.assertAlmostEqual(aqu, 120000 / 30000.0)  # Δweighted ms / Δwall ms
        self.assertAlmostEqual(r_await, 25000 / 10000.0)  # Δread ms / Δreads
        self.assertEqual(report_w7.device_queue(samples[:1], "259:0"), (None, None))
        self.assertEqual(report_w7.device_queue(samples, "9:9"), (None, None))

    def test_info_counters_take_the_first_section(self):
        with open(os.path.join(W7, "M32.info0")) as f:
            self.assertEqual(report_w7.parse_info_counters(f.read()),
                             {"block_cache_data_hit": 1000, "block_cache_data_miss": 3000})

    def test_fio_json_after_warnings(self):
        r = report_w7.parse_fio(os.path.join(W7, "fio-qd8.json"))
        self.assertAlmostEqual(r["mbs"], 4949.914029)
        self.assertEqual(round(r["iops"]), 18882)
        self.assertAlmostEqual(r["p99_ms"], 0.823296)


class W7ReportTest(unittest.TestCase):
    def test_cells_per_server_and_phase(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t)
            meta, _, _, _ = report.load(t)
            cells = report_w7.collect(t, meta)
            self.assertEqual(len(cells), 15)
            self.assertTrue(all(c["error"] is None for c in cells))
            c = _w7(cells, "kvrocks", "M32")
            self.assertEqual(c["device"], "nvme0n1")
            self.assertAlmostEqual(c["read_mbs"], 132701.55264 / 30.002)
            self.assertAlmostEqual(c["reads_per_get"], 1.25)
            self.assertAlmostEqual(c["write_amp"], 2.0)
            self.assertAlmostEqual(c["aqu_sz"], 4.0)
            self.assertAlmostEqual(c["r_await_ms"], 2.5)
            self.assertAlmostEqual(c["data_hit_pct"], 25.0)
            self.assertAlmostEqual(c["cpu_us_per_op"], 89997000 / 449988.0)
            self.assertEqual(c["mem_peak_bytes"], int(3.5 * (1 << 30)))
            self.assertAlmostEqual(c["client_cpu_pct"], 50.0)
            self.assertFalse(c["client_bound"])
            self.assertIsNone(_w7(cells, "kvrocks", "R32")["write_amp"])  # write amp is M32's
            self.assertIsNone(_w7(cells, "tideskv", "M32")["data_hit_pct"])  # a Kvrocks counter

    def test_render_w7_tables_and_fio(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t)
            self.assertEqual(report.main([t]), 0)
            with open(os.path.join(t, "report.md")) as f:
                md = f.read()
            self.assertIn("## W7: 256 KiB values on a miss-heavy cache", md)
            self.assertNotIn("## Relaxed durability", md)
            self.assertIn("| Server | Phase | GET/s | SET/s | GET p50 / p99 / p99.9 ms | SET p99 ms | CPU µs/op "
                          "| peak mem | client CPU | settle s |", md)
            self.assertIn("| TidesKV | M32 | 13498 | 1500 | ", md)
            self.assertIn("| 10.24 | 200.0 | 3.50 GiB | 50% | 5 |", md)
            self.assertIn("| Kvrocks+BlobDB | R32 (workers 16) | 13498 |", md)
            self.assertIn("| write amp | write amp incl. settle | aqu-sz | r_await ms | Kvrocks data-block hit % |", md)
            self.assertIn("| Kvrocks | M32 | nvme0n1 | 4423.1 | 786.6 | 1.25 | 2.00 | 2.50 | 4.00 | 2.50 | 25.0 |", md)
            self.assertIn("| TidesKV | R32 | nvme0n1 | 4423.1 | 786.6 | 1.25 | – | – | 4.00 | 2.50 | – |", md)
            # BlobDB values bypass the block cache: no data-block hit rate for it
            self.assertIn("| Kvrocks+BlobDB | M32 | nvme0n1 | 4423.1 | 786.6 | 1.25 | 2.00 | 2.50 | 4.00 | 2.50 "
                          "| – (index only) |", md)
            self.assertIn("\"– (index only)\"", md)
            self.assertIn("### Disk ceiling (fio)", md)
            self.assertIn("| 8 | 4949.9 | 18882 | 0.42 | 0.82 |", md)
            self.assertIn("| 1 | 4949.9 |", md)
            self.assertIn("| Kvrocks+BlobDB | 32.00 GiB |", md)
            with open(os.path.join(t, "summary.json")) as f:
                self.assertEqual(len(json.load(f)["w7_cells"]), 15)

    def test_client_bound_flag_above_85_percent(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, client_cpu="360.00%")
            meta, _, _, _ = report.load(t)
            c = _w7(report_w7.collect(t, meta), "tideskv", "R32")
            self.assertAlmostEqual(c["client_cpu_pct"], 90.0)
            self.assertTrue(c["client_bound"])
            md = report_w7.render(meta, [c], {}, {}, {})
            self.assertIn("| 90% client-bound? |", md)

    def test_missing_phase_is_err_and_exit_one(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t)
            os.remove(os.path.join(t, "kvrocks-blob", "large", "M32.status"))
            self.assertEqual(report.main([t]), 1)
            with open(os.path.join(t, "report.md")) as f:
                md = f.read()
            self.assertIn("| Kvrocks+BlobDB | M32 | ERR |", md)
            self.assertIn("- Kvrocks+BlobDB M32: no status", md)

    def test_failed_server_is_not_run(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, servers=("tideskv", "kvrocks"))
            os.makedirs(os.path.join(t, "kvrocks-blob"))
            with open(os.path.join(t, "kvrocks-blob", "run.failed"), "w") as f:
                f.write("load failed or the load check did not pass; see kvrocks-blob/load/\n")
            self.assertEqual(report.main([t]), 1)
            with open(os.path.join(t, "report.md")) as f:
                md = f.read()
            self.assertIn("- Kvrocks+BlobDB R1: not run: load failed", md)
            self.assertIn("## Failed runs", md)
            self.assertIn("| Kvrocks+BlobDB | not run |", md)

    def test_no_sweep_and_skipped_fio(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, phases=("R1", "R4", "R32", "M32"), fio_status="skipped: cannot pull xridge/fio")
            shutil.rmtree(os.path.join(t, "fio"))
            self.assertEqual(report.main([t]), 0)
            with open(os.path.join(t, "report.md")) as f:
                md = f.read()
            self.assertNotIn("workers 16) |", md)
            self.assertIn("fio did not run: skipped: cannot pull xridge/fio.", md)

    def test_garbage_measurement_files_leave_dashes(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, servers=("tideskv",), phases=("R1",))
            d = os.path.join(t, "tideskv", "large")
            for ext, text in (("io1", "garbage\n"), ("diskstats", "T x\n1 2\n"), ("client.jsonl", "{bad")):
                with open(os.path.join(d, "R1." + ext), "w") as f:
                    f.write(text)
            os.remove(os.path.join(d, "R1.io0"))
            meta, _, _, _ = report.load(t)
            c = _w7(report_w7.collect(t, meta), "tideskv", "R1")
            self.assertIsNone(c["error"])
            self.assertIsNone(c["read_mbs"])
            self.assertIsNone(c["aqu_sz"])
            self.assertIsNone(c["client_cpu_pct"])
            self.assertEqual(report.main([t]), 1)  # the other servers did not run

    def test_write_amp_counts_writeback_after_the_window(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, servers=("tideskv",), phases=("M32",))
            d = os.path.join(t, "tideskv", "large")
            with open(os.path.join(W7, "M32.io1")) as f:
                text = f.read()
            # another 45015 x 256 KiB written back once the host synced
            with open(os.path.join(d, "M32.io1s"), "w") as f:
                f.write(text.replace("wbytes=24100824320", "wbytes=35901236480"))
            meta, _, _, _ = report.load(t)
            c = _w7(report_w7.collect(t, meta), "tideskv", "M32")
            self.assertAlmostEqual(c["write_amp"], 3.0)
            self.assertAlmostEqual(c["write_mbs"], 23600.82432 / 30.002)  # MB/s stays the window's

    def test_no_server_io_leaves_queue_blank(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, servers=("tideskv",), phases=("R1",))
            shutil.copy(os.path.join(W7, "M32.io0"), os.path.join(t, "tideskv", "large", "R1.io1"))
            meta, _, _, _ = report.load(t)
            c = _w7(report_w7.collect(t, meta), "tideskv", "R1")
            self.assertEqual((c["read_mbs"], c["reads_per_get"]), (0.0, 0.0))
            self.assertEqual((c["device"], c["aqu_sz"], c["r_await_ms"]), (None, None, None))

    def _m32(self, t, server="tideskv"):
        meta, _, _, _ = report.load(t)
        return _w7(report_w7.collect(t, meta), server, "M32")

    def _edit_memtier(self, t, fn, server="tideskv", phase="M32"):
        p = os.path.join(t, server, "large", phase + ".json")
        with open(p) as f:
            d = json.load(f)
        fn(d["ALL STATS"])
        with open(p, "w") as f:
            json.dump(d, f)

    def test_valid_replies_pass(self):
        m = report_w7.parse_memtier_w7(os.path.join(W7, "memtier-m32.json"))
        self.assertEqual((m["set_rx"], m["get_rx"]), (5 * 45015, 262155 * 404973))
        self.assertIsNone(report_w7.reply_errors(m))

    def test_set_error_replies_are_err(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, servers=("tideskv",), phases=("M32",))
            # one SET answered "-ERR ... No space left on device\r\n" instead of +OK
            self._edit_memtier(t, lambda s: s["Sets"]["Time-Serie"]["0"].update(
                {"Bytes RX": s["Sets"]["Time-Serie"]["0"]["Bytes RX"] + 40}))
            c = self._m32(t)
            self.assertTrue(c["error"].startswith("errors: SET replies are not all +OK (225115 bytes for 45015 SETs)"))
            self.assertIsNone(c["get_ops"])  # its numbers are not reported
            self.assertEqual(report.main([t]), 1)
            with open(os.path.join(t, "report.md")) as f:
                md = f.read()
            self.assertIn("| TidesKV | M32 | ERR |", md)
            self.assertNotIn("| TidesKV | M32 | nvme0n1", md)

    def test_get_misses_and_short_replies_are_err(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, servers=("tideskv",), phases=("M32",))
            self._edit_memtier(t, lambda s: s["Gets"].update({"Misses/sec": 100.0}))
            self.assertIn("3000 GET misses", self._m32(t)["error"])
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, servers=("tideskv",), phases=("M32",))
            self._edit_memtier(t, lambda s: s["Gets"]["Time-Serie"]["0"].update({"Bytes RX": 0}))
            self.assertIn("GET replies average", self._m32(t)["error"])

    def test_missing_byte_counts_are_err(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, servers=("tideskv",), phases=("M32",))
            self._edit_memtier(t, lambda s: (s["Sets"].pop("Time-Serie"), s["Gets"].pop("Time-Serie")))
            err = self._m32(t)["error"]
            self.assertIn("no SET byte counts", err)
            self.assertIn("no GET byte counts", err)

    def test_diskstats_only_between_markers(self):
        with open(os.path.join(W7, "M32.diskstats")) as f:
            text = f.read()
        # the first sample is before the timed run started: the window is 2000..31000 ms
        samples = report_w7.parse_diskstats("M start 1500\n" + text + "M end 31500\nT 40000\n"
                                            " 259 0 nvme0n1 99999 0 0 99999 0 0 0 0 0 0 999999 0 0 0 0 0 0\n")
        self.assertEqual([t for t, _ in samples], [2000, 31000])
        aqu, r_await = report_w7.device_queue(samples, "259:0")
        self.assertAlmostEqual(aqu, (121000 - 61000) / 29000.0)
        self.assertAlmostEqual(r_await, (25500 - 13000) / 5000.0)

    def test_marker_between_a_sample_and_its_lines(self):
        samples = report_w7.parse_diskstats("T 1000\nM start 1001\n 259 0 nvme0n1 1 0 0 1 0 0 0 0 0 0 1 0 0 0 0 0 0\n"
                                            "T 2000\n 259 0 nvme0n1 2 0 0 3 0 0 0 0 0 0 5 0 0 0 0 0 0\n")
        self.assertEqual([t for t, _ in samples], [2000])  # the 1000 sample precedes the marker

    def test_eviction_warning_keeps_numbers_and_notes_it(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, servers=("tideskv",), phases=("R32",))
            with open(os.path.join(t, "tideskv", "large", "R32.status"), "w") as f:
                f.write("ok\nwarning: the data dir could not be dropped from the page cache\n")
            meta, _, _, _ = report.load(t)
            c = _w7(report_w7.collect(t, meta), "tideskv", "R32")
            self.assertIsNone(c["error"])
            self.assertEqual(c["warnings"], ["warning: the data dir could not be dropped from the page cache"])
            md = report_w7.render(meta, [c], {}, {}, {})
            self.assertIn("- TidesKV R32: warning: the data dir could not be dropped from the page cache", md)

    def test_cache_not_dropped_flag_at_full_size_only(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, servers=("tideskv",), phases=("R1", "R4", "R32"))
            for p in ("R1", "R4"):  # no reads from disk at all
                shutil.copy(os.path.join(W7, "M32.io0"), os.path.join(t, "tideskv", "large", p + ".io1"))
            meta, _, _, _ = report.load(t)
            cells = report_w7.collect(t, meta)
            self.assertEqual(_w7(cells, "tideskv", "R4")["warnings"],
                             ["cache not dropped? 0.00 disk reads per GET, expected about 0.75"])
            self.assertEqual(_w7(cells, "tideskv", "R1")["warnings"], [])  # R1 is expected to be cached
            self.assertEqual(_w7(cells, "tideskv", "R32")["warnings"], [])  # 1.25 reads per GET
            _set_meta(t, size="large-smoke")  # 128 MiB fits in memory: no flag
            meta, _, _, _ = report.load(t)
            self.assertEqual(_w7(report_w7.collect(t, meta), "tideskv", "R4")["warnings"], [])

    def _disk(self, t, server, after, logical=32 << 30):
        with open(os.path.join(t, server, "disk.json"), "w") as f:
            json.dump({"after_load_bytes": after, "end_bytes": after, "load_seconds": 120, "logical_bytes": logical}, f)

    def test_compressed_values_are_noted(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, phases=("R1",))
            self._disk(t, "tideskv", 12 << 30)  # 32 GiB loaded as 12 GiB: lz4 on memtier's random data
            self._disk(t, "kvrocks", int(0.81 * (32 << 30)))  # above 0.8: no note
            self.assertAlmostEqual(report_w7.stored_ratio(t, "tideskv"), 0.375)
            self.assertIsNone(report_w7.stored_ratio(t, "nonexistent"))
            self.assertEqual(report.main([t]), 0)  # a note, not an ERR
            with open(os.path.join(t, "report.md")) as f:
                md = f.read()
            self.assertIn("Errors:\n- TidesKV: values compressed (2.7×): working-set ratios are understated", md)
            self.assertNotIn("Kvrocks: values compressed", md)

    def test_cache_check_scales_with_compression(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, servers=("tideskv", "kvrocks"), phases=("R4", "R32"))
            # 0.30 disk reads per GET in R4 on both
            for s in ("tideskv", "kvrocks"):
                with open(os.path.join(t, s, "large", "R4.io1"), "w") as f:
                    f.write("259:0 rbytes=%d wbytes=500000000\n" % (1000000000 + int(0.3 * 404973 * 262144)))
            self._disk(t, "tideskv", 12 << 30)  # stored 0.375 of logical: 0.30 is what a cold start gives
            meta, _, _, _ = report.load(t)
            cells = report_w7.collect(t, meta)
            self.assertAlmostEqual(_w7(cells, "tideskv", "R4")["reads_per_get"], 0.3, places=3)
            self.assertEqual(_w7(cells, "tideskv", "R4")["warnings"], [])
            self.assertEqual(_w7(cells, "kvrocks", "R4")["warnings"],  # stored 1.06x: 0.30 is too few
                             ["cache not dropped? 0.30 disk reads per GET, expected about 0.75"])
            self._disk(t, "tideskv", 4 << 30)  # stored 0.125: threshold 0.0625, still not flagged
            self.assertEqual(_w7(report_w7.collect(t, meta), "tideskv", "R4")["warnings"], [])
            self._disk(t, "tideskv", 30 << 30)  # stored 0.94: threshold 0.47, 0.30 is flagged
            self.assertEqual(_w7(report_w7.collect(t, meta), "tideskv", "R4")["warnings"],
                             ["cache not dropped? 0.30 disk reads per GET, expected about 0.70"])

    def test_fio_queue_depths_come_from_the_files(self):
        with tempfile.TemporaryDirectory() as t:
            make_w7_run(t, servers=("tideskv",), phases=("R1",))
            shutil.copy(os.path.join(W7, "fio-qd8.json"), os.path.join(t, "fio", "qd64.json"))
            with open(os.path.join(t, "fio", "qd3.json"), "w") as f:
                f.write("{broken")
            self.assertEqual(sorted(report_w7.collect_fio(t)), [1, 3, 8, 64])
            md = report_w7.render({}, [], report_w7.collect_fio(t), {}, {})
            self.assertIn("| 64 | 4949.9 |", md)
            self.assertIn("| 3 | unreadable |", md)

    def test_medium_run_is_not_large(self):
        with tempfile.TemporaryDirectory() as t:
            make_run(t)
            meta, _, _, _ = report.load(t)
            self.assertFalse(report_w7.is_large(meta, t))
            self.assertEqual(report.main([t]), 0)
            with open(os.path.join(t, "report.md")) as f:
                self.assertNotIn("W7", f.read())


if __name__ == "__main__":
    unittest.main()
