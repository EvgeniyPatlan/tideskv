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


if __name__ == "__main__":
    unittest.main()
