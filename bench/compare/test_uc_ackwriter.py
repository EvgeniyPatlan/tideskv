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

    def test_probe_times(self):
        t0 = int(time.time() * 1000)
        self.serve(startup_delay=0.5)
        self.fake.store[b"k:1"] = b"v"
        out = os.path.join(self.dir, "probe.json")
        r = run_tool("probe", "--t0", t0, "--host", "127.0.0.1", "--port", self.fake.port, "--until-p99", 1000, "--timeout", 20,
                     "--prefix", "k:", "--keymax", 1, "--out", out)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out) as f:
            p = json.load(f)
        self.assertGreaterEqual(p["pong_ms"], 500)
        self.assertLess(p["pong_ms"], 5000)
        self.assertGreaterEqual(p["recovered_ms"], p["pong_ms"] + 1000)  # a whole 1 s window after the PONG
        self.assertLessEqual(p["windows"][0]["p99_ms"], 1000)

    def test_probe_t0_is_the_clock_start(self):
        self.serve()
        out = os.path.join(self.dir, "probe.json")
        t0 = int(time.time() * 1000) - 3000  # the server "started" 3 s before the probe did
        r = run_tool("probe", "--host", "127.0.0.1", "--port", self.fake.port, "--until-p99", 1000, "--timeout", 20,
                     "--prefix", "k:", "--keymax", 1, "--t0", t0, "--out", out)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out) as f:
            self.assertGreaterEqual(json.load(f)["pong_ms"], 3000)

    def test_probe_baseline_and_log_keys(self):
        self.serve()
        out = os.path.join(self.dir, "base.json")
        r = run_tool("probe", "--host", "127.0.0.1", "--port", self.fake.port, "--baseline-secs", 1,
                     "--prefix", "k:", "--keymax", 5, "--out", out)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out) as f:
            b = json.load(f)
        self.assertGreater(b["p99_ms"], 0)
        self.assertGreater(b["requests"], 10)
        with open(self.log + ".0", "w") as f:
            f.write("a\nb\n")
        out2 = os.path.join(self.dir, "after.json")
        r = run_tool("probe", "--host", "127.0.0.1", "--port", self.fake.port, "--until-p99", 1000, "--timeout", 10,
                     "--log", self.log, "--out", out2)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_probe_timeout_reports_nulls(self):
        self.serve(startup_delay=30)
        out = os.path.join(self.dir, "probe.json")
        r = run_tool("probe", "--host", "127.0.0.1", "--port", self.fake.port, "--until-p99", 1, "--timeout", 1,
                     "--prefix", "k:", "--keymax", 1, "--out", out)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out) as f:
            p = json.load(f)
        self.assertIsNone(p["pong_ms"])
        self.assertIsNone(p["recovered_ms"])


if __name__ == "__main__":
    unittest.main()
