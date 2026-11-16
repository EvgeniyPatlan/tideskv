"""Tests for uc_loader.py against the in-process fake RESP server (run: python3 -m unittest bench/compare/test_uc_loader.py)."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from uc_fake_resp import FakeResp  # noqa: E402
import uc_loader  # noqa: E402

TOOL = os.path.join(HERE, "uc_loader.py")


def run_tool(*args, timeout=120):
    return subprocess.run([sys.executable, TOOL, *map(str, args)], capture_output=True, text=True, timeout=timeout)


class ValueTest(unittest.TestCase):
    def test_size_and_determinism(self):
        for size in (100, 200, 1024, 4096, 16384):
            v = uc_loader.value_for(7, 42, size)
            self.assertEqual(len(v), size)
            self.assertEqual(v, uc_loader.value_for(7, 42, size))
        self.assertNotEqual(uc_loader.value_for(7, 42, 1024), uc_loader.value_for(7, 43, 1024))
        self.assertNotEqual(uc_loader.value_for(7, 42, 1024), uc_loader.value_for(8, 42, 1024))

    def test_one_value_compresses_to_half(self):
        for size in (1024, 4096, 16384):
            r = len(zlib.compress(uc_loader.value_for(1, 5, size), 9)) / size
            self.assertTrue(0.45 <= r <= 0.56, f"{size}: {r:.3f}")

    def test_no_redundancy_across_values(self):
        # memtier's --random-data compresses to ~0.15 in 16 KiB blocks of values; ours must stay at ~0.5
        block = b"".join(uc_loader.value_for(1, i, 1024) for i in range(1, 17))
        r = len(zlib.compress(block, 9)) / len(block)
        self.assertTrue(0.45 <= r <= 0.56, f"{r:.3f}")


class LoaderTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.out = os.path.join(self._tmp.name, "load.json")
        self.fake = None

    def tearDown(self):
        if self.fake:
            self.fake.stop()
        self._tmp.cleanup()

    def serve(self, **kw):
        self.fake = FakeResp(**kw)
        self.fake.start()
        return self.fake

    def load(self, *extra):
        return run_tool("--host", "127.0.0.1", "--port", self.fake.port, "--out", self.out, *extra)

    def test_sequential_load_writes_every_key_once(self):
        self.serve()
        p = self.load("--prefix", "u1:", "--keys", 1000, "--value-size", 1024, "--conns", 3, "--pipeline", 16)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(set(self.fake.store), {b"u1:%d" % i for i in range(1, 1001)})
        self.assertEqual(len(self.fake.acked), 1000)
        self.assertEqual(self.fake.store[b"u1:77"], uc_loader.value_for(1, 77, 1024))
        with open(self.out) as f:
            res = json.load(f)
        self.assertEqual((res["writes"], res["errors"], res["value_size"]), (1000, 0, 1024))
        self.assertGreater(res["seconds"], 0)

    def test_same_bytes_on_every_run(self):
        self.serve()
        self.load("--prefix", "k:", "--keys", 200, "--value-size", 100, "--conns", 2)
        first = dict(self.fake.store)
        self.fake.store.clear()
        self.load("--prefix", "k:", "--keys", 200, "--value-size", 100, "--conns", 5, "--pipeline", 7)
        self.assertEqual(first, self.fake.store)

    def test_random_overwrites(self):
        self.serve()
        p = self.load("--prefix", "u3:", "--keys", 100, "--value-size", 200, "--order", "random",
                      "--requests", 500, "--seed", 2, "--conns", 4)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(len(self.fake.acked), 500)
        self.assertTrue(set(self.fake.store) <= {b"u3:%d" % i for i in range(1, 101)})
        k = next(iter(self.fake.store))
        self.assertEqual(self.fake.store[k], uc_loader.value_for(2, int(k.split(b":")[1]), 200))

    def test_hset_fields(self):
        seen = {}

        def hset(cmd):
            seen[cmd[1]] = cmd[2:]
            return b":%d\r\n" % ((len(cmd) - 2) // 2)
        self.serve(handlers={"HSET": hset})
        p = self.load("--prefix", "u4:", "--keys", 50, "--value-size", 50, "--fields", 20)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(len(seen), 50)
        args = seen[b"u4:3"]
        self.assertEqual(args[0::2], [b"f%d" % f for f in range(1, 21)])
        self.assertEqual(args[1], uc_loader.value_for(1, 3 * 20 + 0, 50))
        self.assertTrue(all(len(v) == 50 for v in args[1::2]))

    def test_error_reply_exits_2(self):
        self.serve(handlers={"SET": lambda cmd: b"-ERR disk full\r\n"})
        p = self.load("--prefix", "k:", "--keys", 10, "--value-size", 100)
        self.assertEqual(p.returncode, 2)
        self.assertIn("disk full", p.stderr)

    def test_malformed_reply_exits_2_without_hanging(self):
        # a worker that dies on a reply it cannot parse must not leave the parent waiting forever
        self.serve(handlers={"SET": lambda cmd: b"$x\r\n"})
        p = run_tool("--host", "127.0.0.1", "--port", self.fake.port, "--out", self.out, "--prefix", "k:", "--keys", 10,
                     "--value-size", 10, timeout=30)
        self.assertEqual(p.returncode, 2)

    def test_no_server_exits_2(self):
        self.serve()
        port = self.fake.port
        self.fake.stop()
        self.fake = None
        p = run_tool("--host", "127.0.0.1", "--port", port, "--out", self.out, "--prefix", "k:", "--keys", 5,
                     "--value-size", 10, "--connect-timeout", 1)
        self.assertEqual(p.returncode, 2)


if __name__ == "__main__":
    unittest.main()
