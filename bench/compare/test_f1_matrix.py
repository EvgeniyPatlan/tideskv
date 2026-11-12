"""Tests for f1_matrix.py against in-process fake RESP servers (stdlib only). Registered in ctest as bench_f1_matrix."""
import json
import os
import tempfile
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import f1_matrix  # noqa: E402
from uc_fake_resp import FakeResp  # noqa: E402
ROOT = os.path.dirname(os.path.dirname(HERE))
OK = b"+OK\r\n"
UNKNOWN = b"-ERR unknown command 'foo'\r\n"


def bulk(text):
    return b"$%d\r\n%s\r\n" % (len(text), text)


def arr(*items):
    return b"*%d\r\n" % len(items) + b"".join(b"$%d\r\n%s\r\n" % (len(i), i) for i in items)


class MatrixTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.servers = []

    def tearDown(self):
        for s in self.servers:
            s.stop()
        self.tmp.cleanup()

    def server(self, **handlers):
        s = FakeResp(handlers=handlers).start()
        self.servers.append(s)
        return s

    def write(self, name, text):
        path = os.path.join(self.tmp.name, name)
        with open(path, "w") as f:
            f.write(text)
        return path

    def run_matrix(self, cases, targets, ref, documented=""):
        out = os.path.join(self.tmp.name, "m.json")
        f1_matrix.run([("%s=127.0.0.1:%d" % (n, s.port)) for n, s in targets], "127.0.0.1:%d" % ref.port,
                      self.write("cases.txt", cases), self.write("doc.txt", documented), out)
        with open(out) as f:
            return json.load(f)

    @staticmethod
    def result(m, target, case=0):
        return m["cases"][case]["results"][target]

    def test_same(self):
        m = self.run_matrix("## strings | set get\nSET a 1\nGET a\n", [("t", self.server())], self.server())
        self.assertEqual(self.result(m, "t")["status"], "same")

    def test_differs_reports_first_command(self):
        t = self.server(GET=lambda c: b"$1\r\n2\r\n")
        m = self.run_matrix("## strings | c\nSET a 1\nGET a\nDBSIZE\n", [("t", t)], self.server())
        r = self.result(m, "t")
        self.assertEqual((r["status"], r["command"], r["ref"], r["got"]), ("differs", "GET a", '"1"', '"2"'))

    def test_unsupported_detected(self):
        ref = self.server(FOO=lambda c: OK)
        t = self.server(FOO=lambda c: UNKNOWN)
        m = self.run_matrix("## strings | c\nFOO x\n", [("t", t)], ref)
        r = self.result(m, "t")
        self.assertEqual((r["status"], r["command"]), ("unsupported", "FOO x"))

    def test_unknown_on_both_is_same_and_extra_is_not_a_difference(self):
        ref = self.server(FOO=lambda c: UNKNOWN)
        m = self.run_matrix("## json | c\nFOO x\n", [("none", self.server(FOO=lambda c: UNKNOWN)),
                                                    ("more", self.server(FOO=lambda c: OK))], ref)
        self.assertEqual((self.result(m, "none")["status"], self.result(m, "more")["status"]), ("same", "extra"))
        self.assertFalse(m["cases"][0]["ref_accepts"])

    def test_unordered_compares_sets(self):
        ref = self.server(SMEMBERS=lambda c: arr(b"a", b"b", b"c"))
        t = self.server(SMEMBERS=lambda c: arr(b"c", b"a", b"b"))
        cases = "## set | ordered\nSMEMBERS s\n## set | unordered\nSMEMBERS s #unordered\n"
        m = self.run_matrix(cases, [("t", t)], ref)
        self.assertEqual([self.result(m, "t", i)["status"] for i in (0, 1)], ["differs", "same"])
        t2 = self.server(SMEMBERS=lambda c: arr(b"c", b"a"))
        m = self.run_matrix("## set | u\nSMEMBERS s #unordered\n", [("t", t2)], ref)
        self.assertEqual(self.result(m, "t")["status"], "differs")

    def test_shape_and_fields_directives(self):
        ref = self.server(INFO=lambda c: bulk(b"role:master\r\nconnected_slaves:0\r\nv:1\r\n"), TIME=lambda c: arr(b"1", b"2"))
        t = self.server(INFO=lambda c: bulk(b"role:master\r\nconnected_slaves:0\r\nv:2\r\n"), TIME=lambda c: arr(b"9", b"8"))
        m = self.run_matrix("## server/info | a\nINFO x #fields=role,connected_slaves\n## server/info | b\nTIME #shape\n"
                            "## server/info | c\nINFO x\n", [("t", t)], ref)
        self.assertEqual([self.result(m, "t", i)["status"] for i in range(3)], ["same", "same", "differs"])

    def test_unknown_subcommand_on_redis_is_an_error_comparison(self):
        ref = self.server(SCRIPT=lambda c: b"-ERR unknown subcommand 'NOSUCH'. Try SCRIPT HELP.\r\n")
        t = self.server(SCRIPT=lambda c: b"-ERR Unknown SCRIPT subcommand\r\n")
        m = self.run_matrix("## scripting | c\nSCRIPT NOSUCH\n", [("t", t)], ref)
        self.assertEqual(self.result(m, "t")["status"], "same")

    def test_unsupported_only_where_redis_succeeds(self):
        ref = self.server(FOO=lambda c: b"-WRONGTYPE nope\r\n")
        t = self.server(FOO=lambda c: UNKNOWN)
        m = self.run_matrix("## strings | c\nFOO x\n", [("t", t)], ref)
        self.assertEqual(self.result(m, "t")["status"], "differs")

    def test_length_directive(self):
        ref = self.server(SPOP=lambda c: arr(b"a", b"b"))
        t = self.server(SPOP=lambda c: arr(b"x", b"c"))
        m = self.run_matrix("## set | l\nSPOP s 2 #length\n## set | n\nSPOP s 2\n", [("t", t)], ref)
        self.assertEqual([self.result(m, "t", i)["status"] for i in (0, 1)], ["same", "differs"])

    def test_documented_command_filter(self):
        t = self.server(GET=lambda c: b"$1\r\n2\r\n")
        doc = "## kvrocks\nstrings | c @DBSIZE | https://kvrocks.apache.org/docs/x | n\nstrings | d @get | https://kvrocks.apache.org/docs/x | n\n"
        m = self.run_matrix("## strings | c\nSET a 1\nGET a\n## strings | d\nSET a 1\nGET a\n", [("kvrocks", t)], self.server(), doc)
        self.assertEqual([bool(self.result(m, "kvrocks", i)["documented"]) for i in (0, 1)], [False, True])

    def test_error_text_is_not_a_difference_but_is_counted(self):
        ref = self.server(GET=lambda c: b"-ERR wrong number of arguments for 'get' command\r\n")
        t = self.server(GET=lambda c: b"-ERR bad args\r\n")
        m = self.run_matrix("## strings | c\nGET\n", [("t", t)], ref)
        r = self.result(m, "t")
        self.assertEqual((r["status"], r["error_text_differs"]), ("same", True))
        t = self.server(GET=lambda c: b"-WRONGTYPE x\r\n")
        m = self.run_matrix("## strings | c\nGET\n", [("t", t)], ref)
        self.assertEqual(self.result(m, "t")["status"], "differs")

    def test_documented_label_kvrocks_only(self):
        t = self.server(GET=lambda c: b"$1\r\n2\r\n")
        doc = "## kvrocks\nstrings | c | https://kvrocks.apache.org/docs/x | note\n"
        m = self.run_matrix("## strings | c\nSET a 1\nGET a\n", [("kvrocks", t), ("tideskv", t)], self.server(), doc)
        k, tk = self.result(m, "kvrocks"), self.result(m, "tideskv")
        self.assertEqual((k["status"], k["documented"]["url"]), ("differs", "https://kvrocks.apache.org/docs/x"))
        self.assertIsNone(tk["documented"])

    def test_flushall_between_cases(self):
        t, ref = self.server(), self.server()
        m = self.run_matrix("## strings | one\nSET a 1\n## strings | two\nDBSIZE\n", [("t", t)], ref)
        self.assertEqual(self.result(m, "t", 1)["status"], "same")
        self.assertEqual(ref.store, {})

    def test_merge_combines_targets(self):
        a = {"ref": "r", "targets": ["x"], "cases": [{"family": "strings", "case": "c", "ref_accepts": True,
                                                       "results": {"x": {"status": "same"}}}]}
        b = {"ref": "r", "targets": ["y"], "cases": [{"family": "strings", "case": "c", "ref_accepts": True,
                                                       "results": {"y": {"status": "differs"}}}]}
        m = f1_matrix.merge([a, b])
        self.assertEqual(m["targets"], ["x", "y"])
        self.assertEqual(set(m["cases"][0]["results"]), {"x", "y"})


class CaseFileTest(unittest.TestCase):
    def test_case_file_parses(self):
        cases = f1_matrix.parse_cases(os.path.join(HERE, "f1_cases.txt"))
        self.assertGreaterEqual(len(cases), 110)
        fams = {c["family"] for c in cases}
        self.assertTrue(fams <= set(f1_matrix.FAMILIES), fams - set(f1_matrix.FAMILIES))
        self.assertTrue(set(f1_matrix.FAMILIES) <= fams, set(f1_matrix.FAMILIES) - fams)
        names = [(c["family"], c["case"]) for c in cases]
        self.assertEqual(len(names), len(set(names)))
        self.assertTrue(all(c["commands"] for c in cases))

    def test_parser_rejects_bad_files(self):
        for text in ("SET a 1\n", "## nofamily | c\nGET a\n", "## strings | c\n", "## strings | c\nGET a\n## strings | c\nGET a\n"):
            with tempfile.NamedTemporaryFile("w", suffix=".txt") as f:
                f.write(text)
                f.flush()
                with self.assertRaises(ValueError, msg=text):
                    f1_matrix.parse_cases(f.name)

    def test_quoting_and_directives(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt") as f:
            f.write("# comment\n## json | c\nJSON.SET k $ '{\"a\": 1}'\nSMEMBERS s #unordered\n@sleep 0.2\n")
            f.flush()
            (c,) = f1_matrix.parse_cases(f.name)
        self.assertEqual(c["commands"][0]["argv"], [b"JSON.SET", b"k", b"$", b'{"a": 1}'])
        self.assertEqual(c["commands"][1]["mode"], "unordered")
        self.assertEqual(c["commands"][2]["sleep"], 0.2)

    def test_documented_file_entries_have_sources(self):
        entries = f1_matrix.parse_documented(os.path.join(HERE, "f1_documented.txt"))
        cases = {(c["family"], c["case"]) for c in f1_matrix.parse_cases(os.path.join(HERE, "f1_cases.txt"))}
        self.assertTrue(entries)
        for (target, fam, case), e in entries.items():
            self.assertIn((fam, case), cases)
            self.assertTrue(e["note"])
            if target == "kvrocks":
                self.assertRegex(e["url"], r"^https://(kvrocks\.apache\.org|github\.com/apache/kvrocks)/")
            else:
                self.assertTrue(os.path.exists(os.path.join(ROOT, e["url"].split("#")[0])), e["url"])


if __name__ == "__main__":
    unittest.main()
