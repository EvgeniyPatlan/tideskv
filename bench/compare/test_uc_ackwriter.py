"""Tests for uc_ackwriter.py against the in-process fake RESP server (run: python3 -m unittest bench/compare/test_uc_ackwriter.py)."""
import glob
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from uc_fake_resp import FakeResp  # noqa: E402

TOOL = os.path.join(HERE, "uc_ackwriter.py")


def run_tool(*args, timeout=60):
    return subprocess.run([sys.executable, TOOL, *map(str, args)], capture_output=True, text=True, timeout=timeout)


class AckWriterTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = self._tmp.name
        self.log = os.path.join(self.dir, "ack")
        self.fake = None

    def tearDown(self):
        if self.fake:
            self.fake.stop()
        self._tmp.cleanup()

    def serve(self, **kw):
        self.fake = FakeResp(**kw)
        self.fake.start()
        return self.fake

    def write_for(self, secs, conns=4, then_kill=True):
        """Run the writer, kill the fake after secs, return the writer's CompletedProcess."""
        box = []
        t = threading.Thread(target=lambda: box.append(run_tool(
            "write", "--host", "127.0.0.1", "--port", self.fake.port, "--conns", conns, "--prefix", "k", "--log", self.log)))
        t.start()
        time.sleep(secs)
        if then_kill:
            self.fake.kill(drop_last=0)
        t.join(30)
        self.assertFalse(t.is_alive(), "the writer did not exit after the connection was lost")
        return box[0]

    def logged(self):
        keys = []
        for f in sorted(glob.glob(self.log + ".*")):
            with open(f) as fh:
                keys += fh.read().split()
        return keys

    def dropped_and_logged(self, k):
        """The last k acked keys of the fake that the writer also logged (the kill can cut off the last replies)."""
        return {key.decode() for key in self.fake.acked[-k:]} & set(self.logged())

    def verify(self):
        out = os.path.join(self.dir, "verify.json")
        r = run_tool("verify", "--host", "127.0.0.1", "--port", self.fake.port, "--log", self.log, "--out", out)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out) as f:
            return json.load(f)

    def test_all_acked_survive(self):
        self.serve()
        w = self.write_for(0.6)
        self.assertEqual(w.returncode, 0, w.stderr)
        self.fake.restart(0)
        keys = self.logged()
        self.assertGreater(len(keys), 50)
        self.assertEqual(int(w.stdout.strip().split()[-1]), len(keys))
        v = self.verify()
        self.assertEqual((v["acked"], v["lost"], v["lost_sample"]), (len(keys), 0, []))

    def test_dropped_acked_write_detected(self):
        self.serve()
        self.assertEqual(self.write_for(0.5, then_kill=True).returncode, 0)
        dropped = set(self.logged()[-3:])  # three keys the client logged as acked
        self.fake.stop()
        self.fake.drop(*dropped)
        self.fake.restart(0)
        v = self.verify()
        self.assertEqual(v["lost"], 3)
        self.assertEqual(set(v["lost_sample"]), dropped)

    def test_lost_sample_is_capped_at_ten(self):
        self.serve()
        self.write_for(0.5)
        self.fake.kill(drop_last=25)
        dropped = self.dropped_and_logged(25)
        self.fake.restart(0)
        v = self.verify()
        self.assertEqual((v["lost"], len(v["lost_sample"])), (len(dropped), 10))
        self.assertGreater(len(dropped), 10)

    def test_unacked_not_counted(self):
        self.serve(close_before_reply_key="k0:3")
        w = self.write_for(2, conns=1, then_kill=False)
        self.assertEqual(w.returncode, 0, w.stderr)
        self.assertEqual(self.logged(), ["k0:0", "k0:1", "k0:2"])
        self.assertEqual(w.stdout.strip().split()[-1], "3")
        self.assertIn(b"k0:3", self.fake.unacked)
        v = self.verify()  # the server did store it, but it was never acknowledged
        self.assertEqual((v["acked"], v["lost"]), (3, 0))

    def test_writer_error_reply_is_not_an_ack(self):
        self.serve()
        r = run_tool("write", "--host", "127.0.0.1", "--port", self.fake.port, "--conns", 1, "--prefix", "k",
                     "--log", self.log, "--command", "BOGUS", timeout=30)
        self.assertEqual(r.returncode, 2)
        self.assertEqual(self.logged(), [])

    def probe(self, *args, timeout=60):
        out = os.path.join(self.dir, "probe-%d.json" % len(os.listdir(self.dir)))
        r = run_tool("probe", "--host", "127.0.0.1", "--port", self.fake.port, "--prefix", "k:", "--keymax", 1,
                     "--out", out, *args, timeout=timeout)
        data = None
        if os.path.exists(out):
            with open(out) as f:
                data = json.load(f)
        return r, data

    def test_probe_times(self):
        t0 = int(time.time() * 1000)
        self.serve(startup_delay=0.5)
        self.fake.store[b"k:1"] = b"v"
        r, p = self.probe("--t0", t0, "--until-p99", 1000, "--timeout", 20)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertGreaterEqual(p["pong_ms"], 500)
        self.assertLess(p["pong_ms"], 700)  # polled every few ms, not once a second
        # the start of the first of 3 consecutive 100 ms windows under the threshold: soon after the PONG
        self.assertGreaterEqual(p["recovered_ms"], p["pong_ms"])
        self.assertLess(p["recovered_ms"], p["pong_ms"] + 500)
        self.assertGreater(p["hits"], 0)
        self.assertEqual(p["restart_misses"], 0)
        self.assertLessEqual(p["windows"][0]["p99_ms"], 1000)

    def test_probe_recovery_needs_three_passing_windows(self):
        self.serve()
        self.fake.store[b"k:1"] = b"v"
        r, p = self.probe("--until-p99", 0.0000001, "--timeout", 1)  # nothing passes
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIsNotNone(p["pong_ms"])
        self.assertIsNone(p["recovered_ms"])
        self.assertGreater(len(p["windows"]), 3)

    def test_probe_waits_for_the_t0_file(self):
        self.serve()
        self.fake.store[b"k:1"] = b"v"
        t0file = os.path.join(self.dir, "t0")
        out = os.path.join(self.dir, "p.json")
        proc = subprocess.Popen([sys.executable, TOOL, "probe", "--host", "127.0.0.1", "--port", str(self.fake.port),
                                 "--prefix", "k:", "--keymax", "1", "--until-p99", "1000", "--timeout", "20",
                                 "--t0-file", t0file, "--out", out], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(1)
        self.assertIsNone(proc.poll(), "the probe must wait for the t0 file")
        tmp = t0file + ".tmp"
        with open(tmp, "w") as f:
            f.write(str(int(time.time() * 1000) - 2000))  # the server "started" 2 s ago
        os.rename(tmp, t0file)
        proc.communicate(timeout=30)
        self.assertEqual(proc.returncode, 0)
        with open(out) as f:
            self.assertGreaterEqual(json.load(f)["pong_ms"], 2000)

    def test_probe_baseline_counts_hits(self):
        self.serve()
        self.fake.store[b"k:1"] = b"v"
        r, b = self.probe("--baseline-secs", 1)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertGreater(b["p99_ms"], 0)
        self.assertEqual(b["hits"], b["requests"])
        self.assertGreater(b["requests"], 10)
        self.assertGreaterEqual(len(b["windows"]), 9)  # 100 ms windows

    def test_probe_reports_restart_misses_without_failing(self):
        self.serve()
        self.fake.store[b"k:1"] = b"v"  # k:2 is missing
        r, p = self.probe("--keymax", 2, "--until-p99", 1000, "--timeout", 5)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertGreater(p["restart_misses"], 0)
        self.assertEqual(p["restart_misses"], p["requests"] - p["hits"])

    def test_probe_zero_hits_fails(self):
        self.serve()  # the keys the probe reads do not exist
        r, b = self.probe("--baseline-secs", 1)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("hits", r.stderr)
        r, p = self.probe("--until-p99", 1000, "--timeout", 5)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(p["hits"], 0)

    def test_probe_timeout_reports_nulls(self):
        self.serve(startup_delay=30)
        r, p = self.probe("--until-p99", 1, "--timeout", 1)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIsNone(p["pong_ms"])
        self.assertIsNone(p["recovered_ms"])

    def test_corrupt_value_is_not_lost(self):
        self.serve()
        self.write_for(0.5)
        self.fake.restart(0)
        bad = self.logged()[0]
        self.fake.store[bad.encode()] = b"bogus"
        v = self.verify()
        self.assertEqual((v["lost"], v["corrupt"], v["corrupt_sample"]), (0, 1, [bad]))

    def test_partial_last_log_line_is_skipped(self):
        self.serve()
        with open(self.log + ".0", "w") as f:
            f.write("k0:0\nk0:1\nk0:2")  # the writer was killed mid-line
        self.fake.store[b"k0:0"] = b"0"
        self.fake.store[b"k0:1"] = b"1"
        v = self.verify()
        self.assertEqual((v["acked"], v["lost"]), (2, 0))

    def test_earlier_point_keys_do_not_mask_later_losses(self):
        """Each point has its own prefix on the same data: point 2's dropped keys stay lost although point 1 wrote
        keys of the same thread/seq shape (a constant prefix would let them mask it)."""
        self.serve()
        for point in ("p1", "p2"):
            self.log = os.path.join(self.dir, point)
            box = []
            t = threading.Thread(target=lambda: box.append(run_tool(
                "write", "--host", "127.0.0.1", "--port", self.fake.port, "--conns", 2, "--prefix", point + "-",
                "--log", self.log)))
            t.start()
            time.sleep(0.4)
            self.fake.kill(drop_last=0)
            t.join(30)
            self.fake.restart(0)
        self.fake.drop(*self.logged()[:3])  # point 2 loses three acked keys
        v = self.verify()
        self.assertEqual(v["lost"], 3)
        self.assertEqual(v["corrupt"], 0)


if __name__ == "__main__":
    unittest.main()
