"""Tests for uc_lsm.py (run: python3 -m unittest bench/compare/test_uc_lsm.py)."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "uc_lsm.py")


def creation(n, cf, comp, size):
    e = {"time_micros": 1, "cf_name": cf, "job": 3, "event": "table_file_creation", "file_number": n,
         "file_size": size, "table_properties": {"data_size": size, "compression": comp, "num_entries": 10}}
    return "2026/11/13-18:37:52.718278 14 EVENT_LOG_v1 " + json.dumps(e) + "\n"


class LsmTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.db = self._tmp.name

    def tearDown(self):
        self._tmp.cleanup()

    def sst(self, n, size):
        with open(os.path.join(self.db, "%06d.sst" % n), "wb") as f:
            f.write(b"x" * size)

    def run_tool(self):
        p = subprocess.run([sys.executable, TOOL, self.db], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        return json.loads(p.stdout)

    def test_live_files_by_compression(self):
        with open(os.path.join(self.db, "LOG.old.1"), "w") as f:
            f.write(creation(5, "metadata", "NoCompression", 300))  # live, from the rotated log
            f.write(creation(6, "metadata", "LZ4", 999))            # deleted since: not on disk
            f.write("2026/11/13 some other line\n")
        with open(os.path.join(self.db, "LOG"), "w") as f:
            f.write(creation(7, "metadata", "LZ4", 100))
            f.write(creation(8, "default", "LZ4", 50))
            f.write("2026/11/13 EVENT_LOG_v1 {broken json\n")
        self.sst(5, 300)
        self.sst(7, 100)
        self.sst(8, 50)
        self.sst(9, 20)  # no creation event: unknown
        r = self.run_tool()
        self.assertEqual(r["sst_files"], 4)
        self.assertEqual(r["sst_bytes"], 470)
        self.assertEqual(r["by_compression"], {"NoCompression": 300, "LZ4": 150, "unknown": 20})
        self.assertEqual(r["by_cf"]["metadata"], {"NoCompression": 300, "LZ4": 100})
        self.assertAlmostEqual(r["uncompressed_share"], round(300 / 450, 4))
        self.assertAlmostEqual(r["unknown_share"], round(20 / 470, 4))

    def test_empty_dir(self):
        r = self.run_tool()
        self.assertEqual((r["sst_files"], r["sst_bytes"], r["uncompressed_share"]), (0, 0, None))

    def test_missing_dir_exits_1(self):
        p = subprocess.run([sys.executable, TOOL, os.path.join(self.db, "nope")], capture_output=True, text=True)
        self.assertEqual(p.returncode, 1)


if __name__ == "__main__":
    unittest.main()
