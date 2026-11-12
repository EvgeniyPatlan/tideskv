import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uc_cells  # noqa: E402
import uc_metrics  # noqa: E402
import uc_report  # noqa: E402
import uc_report_text  # noqa: E402


def memtier(ops, p99=1.0, p999=2.0, sets=100, gets=900, set_rx=None, misses=0.0, rate=0, series=None, clients=10,
            threads=2):
    sec = lambda n: {"Count": n, "Ops/sec": ops * n / (sets + gets), "Hits/sec": 0.0, "Misses/sec": 0.0}
    s_sec = sec(sets)
    s_sec["Time-Serie"] = {"0": {"Bytes RX": 5 * sets if set_rx is None else set_rx}}
    g_sec = sec(gets)
    g_sec["Hits/sec"], g_sec["Misses/sec"] = ops * gets / (sets + gets) * (1 - misses), ops * gets / (sets + gets) * misses
    totals = {"Count": sets + gets, "Ops/sec": ops, "Percentile Latencies": {"p50.00": 0.5, "p99.00": p99, "p99.90": p999},
              "Time-Serie": series or {}}
    return {"configuration": {"rate_limit": rate, "clients": clients, "threads": threads, "ratio": "1:9", "data_size": 1024,
                              "key_pattern": "R:R", "key_maximum": 1000, "pipeline": 1, "test_time": 30, "expiry_range": "0:0"},
            "ALL STATS": {"Sets": s_sec, "Gets": g_sec, "Totals": totals}}


DOC = {"url": "https://kvrocks.apache.org/x", "note": "lex ignores scores"}


def dif(cmd, status="differs", rk="bulk", gk="bulk", doc=None, ref='"1"', got='"2"'):
    return {"command": cmd, "status": status, "ref": ref, "got": got, "ref_kind": rk, "got_kind": gk, "documented": doc}


def _res(status="same", diffs=(), accepts=True):
    flagged = [d for d in diffs if d["status"] in ("differs", "unsupported")]
    return {"status": status, "diffs": list(diffs), "accepts": accepts, "error_text": [],
            "documented": [d["documented"] for d in flagged] if flagged and all(d["documented"] for d in flagged) else None,
            "undocumented": [d["command"] for d in flagged if not d["documented"]]}


def case(family, name, tides, kv, ref_accepts=True):
    return {"family": family, "case": name, "ref_accepts": ref_accepts, "results": {"tideskv": tides, "kvrocks": kv}}


F1_MATRIX = {
    "ref": "redis 7.2.5 at r:6379", "targets": ["tideskv", "kvrocks"],
    "cases": [
        case("strings", "c1", _res(), _res()),
        case("strings", "c2", _res(), _res("differs", [dif("GET a")])),
        case("strings", "touch", _res("unsupported", [dif("TOUCH a", "unsupported", "integer", "error ERR")]), _res()),
        case("hash", "wrong type", _res(), _res("differs", [dif("HGET s f", rk="error WRONGTYPE", gk="error ERR")])),
        case("list", "wrong type", _res(), _res("differs", [dif("LPUSH s a", rk="error WRONGTYPE", gk="error ERR")])),
        case("set", "dump", _res("unsupported", [dif("DUMP k", "unsupported", "nil-bulk", "error ERR")]), _res()),
        case("zset", "lex", _res(), _res("differs", [dif("ZRANGEBYLEX z - +", doc=DOC), dif("ZLEXCOUNT z - +", doc=DOC)])),
        case("zset", "partly", _res(), _res("differs", [dif("ZRANGEBYLEX z - +", doc=DOC), dif("ZRANGE z 0 -1")])),
        case("streams", "xadd", _res(), _res("unsupported", [dif("XADD s 1-1 a 1", "unsupported", "bulk", "error ERR", doc=DOC)], False)),
        case("geo", "geoadd", _res("unsupported", [dif("GEOADD g", "unsupported", "integer", "error ERR", doc=DOC)], False),
             _res("differs", [dif("GEODIST g a b")])),
        case("scripting", "config", _res("differs", [dif("CONFIG SET x 1", rk="status", gk="error ERR")]), _res()),
        case("json", "json set", _res(accepts=False), _res("extra", [dif("JSON.SET k $ 1", "extra")]), ref_accepts=False),
    ],
}


class Run:
    def __init__(self, test, mode="full", reps=2):
        self.dir = tempfile.mkdtemp()
        test.addCleanup(shutil.rmtree, self.dir)
        self.reps = reps
        self.write("meta.json", json.dumps({"mode": mode, "reps": reps, "client_cpus": "8-15", "server_mem": "8g"}))

    def path(self, *parts):
        p = os.path.join(self.dir, *parts)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p

    def write(self, rel, text):
        with open(self.path(*rel.split("/")), "w") as f:
            f.write(text)

    def cell(self, case, server, rep, phase, doc, status="ok", client_cpu=None):
        base = "%s/%s/rep%d/%s" % (case, server, rep, phase)
        self.write(base + ".json", json.dumps(doc))
        self.write(base + ".status", status)
        if client_cpu is not None:
            self.write(base + ".client.jsonl", json.dumps({"CPUPerc": "%.1f%%" % client_cpu}))

    def done(self, case, server, rep, text="ok"):
        self.write("%s/%s/rep%d/done" % (case, server, rep), text)

    def du(self, case, server, rep, name, total, wal):
        self.write("%s/%s/rep%d/%s" % (case, server, rep, name), "epoch_s,bytes,wal_archive_bytes\n1,%d,%d\n" % (total, wal))

    def read(self, rel):
        with open(self.path(*rel.split("/"))) as f:
            return f.read()

    def report(self):
        s = uc_report.build(self.dir)
        return s, uc_report.render(s)


def with_hashes(doc):
    for name in ("Hgets", "Hsets", "Hgetalls", "Hdels"):
        doc["ALL STATS"][name] = {"Count": 10, "Ops/sec": doc["ALL STATS"]["Totals"]["Ops/sec"] / 4, "Hits/sec": 0.0,
                                  "Misses/sec": 0.0, "Time-Serie": {"0": {"Bytes RX": 40}}}
    return doc


def u4(run, t_ops, k_ops, k_status="ok", k_doc=None):
    for rep in (1, 2):
        for server, ops in (("tideskv", t_ops), ("kvrocks", k_ops)):
            run.cell("U4", server, rep, "mix", k_doc if (server == "kvrocks" and k_doc) else with_hashes(memtier(ops + rep)),
                     status=k_status if server == "kvrocks" else "ok")
            run.done("U4", server, rep)


def entry(summary, case, key):
    return next(e for e in summary["cases"][case]["metrics"] if e["key"] == key)


class ReportTest(unittest.TestCase):
    def test_err_replies_make_err_cell(self):
        run = Run(self)
        u4(run, 1000, 500, k_doc=memtier(500, set_rx=120))  # -ERR replies are longer than +OK
        s, text = run.report()
        e = entry(s, "U4", "mix_ops")
        self.assertTrue(e["configs"]["kvrocks"]["errors"])
        self.assertEqual(e["verdict"]["label"], "no verdict")
        self.assertIn("ERR: rep1: errors:", text)
        self.assertTrue(s["failed"])

    def test_low_hit_rate_is_err_for_u1(self):
        run = Run(self)
        for rep in (1, 2):
            run.cell("U1", "tideskv", rep, "closed-gauss", memtier(1000, misses=0.5))
            run.cell("U1", "kvrocks", rep, "closed-gauss", memtier(900))
            run.done("U1", "tideskv", rep); run.done("U1", "kvrocks", rep)
        s, _ = run.report()
        self.assertIn("hit rate", " ".join(entry(s, "U1", "closed-gauss_ops")["configs"]["tideskv"]["errors"]))

    def test_client_bound_caveat(self):
        run = Run(self)
        for rep in (1, 2):
            run.cell("U4", "tideskv", rep, "mix", memtier(1000), client_cpu=7 * 100)  # 700% of 8 CPUs = 87.5%
            run.cell("U4", "kvrocks", rep, "mix", memtier(500), client_cpu=100)
            run.done("U4", "tideskv", rep); run.done("U4", "kvrocks", rep)
        s, text = run.report()
        e = entry(s, "U4", "mix_ops")
        self.assertEqual(e["verdict"]["label"], "no verdict")
        self.assertIn("client-bound", e["verdict"]["note"])
        self.assertIn("client-bound: memtier used 88%", text)

    def test_under_target_withholds_latency_only(self):
        run = Run(self)
        for rep in (1, 2):
            run.cell("U1", "tideskv", rep, "open50", memtier(4000, p99=1.0, rate=100, clients=10, threads=2))  # target 2000
            run.cell("U1", "kvrocks", rep, "open50", memtier(1800, p99=9.0, rate=100, clients=10, threads=2))  # 90% of 2000
            run.cell("U1", "kvrocks", rep, "open25", memtier(1000, p99=9.0, rate=50, clients=10, threads=2))
            run.cell("U1", "tideskv", rep, "open25", memtier(1000, p99=1.0, rate=50, clients=10, threads=2))
            run.done("U1", "tideskv", rep); run.done("U1", "kvrocks", rep)
        s, text = run.report()
        self.assertEqual(entry(s, "U1", "open25_p99")["verdict"]["label"], "TidesKV better")
        e = entry(s, "U1", "open50_p99")
        self.assertEqual(e["verdict"]["label"], "no verdict")
        self.assertIn("under target", e["verdict"]["note"])

    def test_summary_matrix_rows(self):
        run = Run(self)
        u4(run, 1000, 500)
        for rep in (1, 2):
            run.cell("U6", "tideskv", rep, "c64", memtier(10, sets=100, gets=0)); run.cell("U6", "kvrocks", rep, "c64", memtier(9, sets=100, gets=0))
            run.done("U6", "tideskv", rep); run.done("U6", "kvrocks", rep)
        with tempfile.TemporaryDirectory() as empty, mock.patch.object(uc_report, "HERE", empty):  # no f2_operations.md anywhere
            s, text = run.report()
        rows = [l for l in text.split("## Summary")[1].split("## Pros")[0].splitlines() if l.startswith("| U")]
        self.assertEqual([r.split(" | ")[0] for r in rows], ["| U4", "| U6"])
        self.assertIn("F1 not run", text)
        self.assertIn("F2 not run", text)

    def test_f2_defaults_to_the_file_next_to_the_tool(self):
        run = Run(self)
        u4(run, 1000, 500)
        _, text = run.report()
        self.assertIn("| Backup and restore |", text.split("## F2")[1])
        self.assertIn("pre-1.0", text.split("## F2")[1])

    def test_verdict_uses_best_kvrocks(self):
        run = Run(self)
        for rep in (1, 2):
            for sz, t, k, b in (("v4k", 2000, 1000, 1200), ("v16k", 2000, 800, 1500)):
                run.cell("U5", "tideskv", rep, sz, memtier(t)); run.cell("U5", "kvrocks", rep, sz, memtier(k))
                run.cell("U5", "kvrocks-blob", rep, sz, memtier(b))
            for srv in ("tideskv", "kvrocks", "kvrocks-blob"):
                run.done("U5", srv, rep)
        s, text = run.report()
        self.assertEqual(entry(s, "U5", "v16k_ops")["vs"], "kvrocks-blob")
        row = next(l for l in text.splitlines() if l.startswith("| U5"))
        self.assertIn("Kvrocks+BlobDB", row)

    def test_kvrocks_config_with_errors_is_shown(self):
        run = Run(self)
        for rep in (1, 2):
            for srv, ops in (("tideskv", 2000), ("kvrocks", 1000), ("kvrocks-blob", 1500)):
                run.cell("U5", srv, rep, "v4k", memtier(ops), status="memtier exited 1" if srv == "kvrocks-blob" else "ok")
                run.done("U5", srv, rep, "cell v4k failed" if srv == "kvrocks-blob" else "ok")
        s, text = run.report()
        e = entry(s, "U5", "v4k_ops")
        self.assertTrue(e["configs"]["kvrocks-blob"]["errors"])
        self.assertEqual(e["vs"], "kvrocks")
        self.assertIn("vs best valid Kvrocks config", text)
        self.assertIn("ERR: rep1: cell v4k failed", text)

    def test_quick_banner(self):
        run = Run(self, mode="quick", reps=1)
        u4(run, 1000, 500)
        s, text = run.report()
        self.assertIn("quick: not a disk result, no verdicts", text)
        self.assertEqual(entry(s, "U4", "mix_ops")["verdict"]["label"], "no verdict")

    def test_pros_cons_symmetry(self):
        run = Run(self)
        u4(run, 2000, 1000)
        _, text = run.report()
        section = text.split("## Pros and cons")[1].split("## Per-case")[0]
        item = "TidesKV: mix ops/s (U4)"
        self.assertEqual(section.count(item), 2)
        pros = section.split("**TidesKV pros**")[1].split("**TidesKV cons**")[0]
        cons = section.split("**Kvrocks cons**")[1].split("**Even")[0]
        self.assertIn(item, pros)
        self.assertIn(item, cons)

    def test_failed_cell_exit_1(self):
        run = Run(self)
        u4(run, 1000, 500)
        run.done("U4", "kvrocks", 2, "server did not start")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(uc_report.main(["uc_report.py", run.dir]), 1)
        self.assertTrue(os.path.exists(os.path.join(run.dir, "report.md")))
        with open(os.path.join(run.dir, "summary.json")) as f:
            json.load(f)
        ok = Run(self)
        u4(ok, 1000, 500)
        self.assertEqual(uc_report.main(["uc_report.py", ok.dir]), 0)

    def test_wal_archive_excluded(self):
        run = Run(self)
        for rep in (1, 2):
            for srv, total, wal in (("tideskv", 1000, 0), ("kvrocks", 5000, 3000)):
                run.du("U2", srv, rep, "du-end", total, wal)
                run.write("U2/%s/rep%d/du.csv" % (srv, rep), "epoch_s,bytes,wal_archive_bytes\n1,%d,%d\n" % (total, wal))
                run.write("U2/%s/rep%d/dbsize-end" % (srv, rep), "unverified")
                run.done("U2", srv, rep)
        s, text = run.report()
        e = entry(s, "U2", "du_end")
        self.assertEqual(e["configs"]["kvrocks"]["median"], 2000)
        self.assertEqual(entry(s, "U2", "wal_archive")["configs"]["kvrocks"]["median"], 3000)
        self.assertIn("unverified", text)

    def test_u2_live_estimate_labelled(self):
        run = Run(self)
        run.du("U2", "tideskv", 1, "du-end", 4096, 0)
        run.write("U2/tideskv/rep1/dbsize-end", "100"); run.write("U2/tideskv/rep1/value-size", "1024")
        self.assertEqual(uc_cells.u2_end_state(run.path("U2", "tideskv", "rep1"))["live_estimate"], 102400)

    def test_windows_over_10x(self):
        series = {str(i): {"Count": 10, "p99.90": 1.0} for i in range(60)}
        series["25"]["p99.90"] = 50.0
        over, n, wins = uc_cells.windows_over_10x(series)
        self.assertEqual((over, n), (1, 6))
        self.assertEqual(wins[2], 50.0)
        series["25"]["p99.90"] = 9.0
        self.assertEqual(uc_cells.windows_over_10x(series)[0], 0)

    def test_u7_point_metrics(self):
        run = Run(self, reps=1)
        for srv, lost in (("tideskv", 0), ("kvrocks", 3)):
            for pt in ("term-5", "kill-5"):
                d = "U7/%s/rep1/%s/" % (srv, pt)
                run.write(d + "verify.json", json.dumps({"acked": 10, "lost": lost, "corrupt": 0}))
                run.write(d + "probe.json", json.dumps({"pong_ms": 40, "recovered_ms": 100, "restart_misses": 0}))
                run.write(d + "restart.docker_run_ms", "150")
            run.done("U7", srv, 1)
        s, text = run.report()
        self.assertEqual(entry(s, "U7", "lost_all")["configs"]["kvrocks"]["median"], 6)
        self.assertIn("lost writes, kill-5", text)
        self.assertIn("process kill (SIGTERM/SIGKILL), not an OS crash or power loss", text)

    def test_u8_per_rep_layout_and_wal(self):
        run = Run(self, reps=1)
        for srv, total, wal in (("tideskv", 1000, 0), ("kvrocks", 9000, 8000)):
            doc = {"du_after_load": {"bytes": total, "wal_archive_bytes": wal}, "load_ops_per_s": 5, "settle_load_s": 5,
                   "settle_load_capped": False, "du_after_age": None, "kvrocks_after_compact": None}
            run.write("U8/%s/rep1/summary.json" % srv, json.dumps(doc))
            run.done("U8", srv, 1)
        s, _ = run.report()
        self.assertEqual(entry(s, "U8", "du_after_load")["configs"]["kvrocks"]["median"], 1000)

    def test_f1_f2_sections_when_present(self):
        run = Run(self)
        u4(run, 1000, 500)
        run.write("F1/matrix.json", json.dumps(F1_MATRIX))
        run.write("f2_operations.md", "| Topic | TidesKV |\n|---|---|\n| backup | x |")
        _, text = run.report()
        self.assertNotIn("F1 not run", text)
        self.assertIn("| backup | x |", text)
        f1 = text.split("## F1")[1].split("## F2")[0]
        self.assertIn("| strings | 3 | same 2 / differs 0 / unsupported 1 / documented 0 | same 2 / differs 1 / unsupported 0 / documented 0 |", f1)
        self.assertIn("| zset | 2 | same 2 / differs 0 / unsupported 0 / documented 0 | same 0 / differs 2 / unsupported 0 / documented 1 |", f1)
        self.assertIn("| Streams | `xadd` | yes | yes | no |", f1)
        self.assertIn("| JSON | `json set` | no | no | yes |", f1)
        self.assertIn("| strings | c2 | Kvrocks | differs | `GET a` | \"1\" | \"2\" | undocumented |", f1)
        self.assertIn("| zset | partly | Kvrocks | differs | `ZRANGE z 0 -1` | \"1\" | \"2\" | undocumented |", f1)
        self.assertIn("| zset | lex | Kvrocks | differs | `ZLEXCOUNT z - +` |", f1)  # every differing command is listed
        self.assertIn("documented: [lex ignores scores](https://kvrocks.apache.org/x)", f1)
        self.assertIn("not a difference (Redis 7.2 lacks it)", f1)
        self.assertIn("redis-databases 0", f1)

    def test_f1_error_text_appendix_is_collapsed(self):
        m = json.loads(json.dumps(F1_MATRIX))
        m["cases"][0]["results"]["kvrocks"]["error_text"] = [{"command": "GET", "ref": "-ERR a", "got": "-ERR b"}]
        run = Run(self)
        u4(run, 1000, 500)
        run.write("F1/matrix.json", json.dumps(m))
        _, text = run.report()
        d = text.split("<details>")[1].split("</details>")[0]
        self.assertIn("| strings | c1 | Kvrocks | `GET` | -ERR a | -ERR b |", d)

    def test_f1_pros_and_cons_in_full_and_quick_runs(self):
        for mode in ("full", "quick"):
            run = Run(self, mode=mode, reps=1 if mode == "quick" else 2)
            u4(run, 1000, 500)
            run.write("F1/matrix.json", json.dumps(F1_MATRIX))
            _, text = run.report()
            pros = text.split("## Pros and cons")[1].split("## Per-case")[0]
            part = lambda a, b: pros.split("**%s**" % a)[1].split("**%s**" % b)[0]
            tp, tc, kp, kc = part("TidesKV pros", "TidesKV cons"), part("TidesKV cons", "Kvrocks pros"), \
                part("Kvrocks pros", "Kvrocks cons"), pros.split("**Kvrocks cons**")[1].split("**Even")[0]
            self.assertIn("F1 hash: TidesKV matches Redis 7.2 on all 1 cases; Kvrocks has 1 undocumented case(s)", tp)
            self.assertIn("F1 set: Kvrocks matches Redis 7.2 on all 1 cases; TidesKV has 1 undocumented case(s)", kp)
            self.assertIn("F1 type: TidesKV has Streams, Kvrocks does not", tp)
            self.assertIn("F1 type: Kvrocks has JSON, TidesKV does not", kp)
            # one item per root cause: WRONGTYPE replaced by ERR in two families is one Kvrocks con
            wrong = [x for x in kc.splitlines() if "error WRONGTYPE replaced by error ERR" in x]
            self.assertEqual(len(wrong), 1)
            self.assertIn("hash, list", wrong[0])
            self.assertIn("HGET, LPUSH", wrong[0])
            # the TidesKV gaps that the other side does not have are cons too, grouped as one cause
            self.assertTrue(any("TidesKV refuses commands Redis 7.2 accepts" in x and "TOUCH, DUMP" in x for x in tc.splitlines()), tc)
            # a documented-missing type is no family pro for TidesKV, while Kvrocks' undocumented geo difference is a con
            self.assertNotIn("F1 geo:", tp)
            self.assertTrue(any("GEODIST" in x and "geo" in x for x in kc.splitlines()))
            self.assertTrue(any("CONFIG SET" in x for x in tc.splitlines()), tc)  # container commands keep their subcommand
            # documented-only differences are no con
            self.assertFalse(any("ZLEXCOUNT" in x for x in kc.splitlines()))
            self.assertTrue(any("ZRANGE" in x and "zset" in x for x in kc.splitlines()))  # the undocumented part of a partly documented case

    def test_documented_absence_is_no_family_pro(self):
        m = json.loads(json.dumps(F1_MATRIX))
        m["cases"].append(case("tracking", "client tracking", _res("unsupported", [dif("CLIENT TRACKING ON", "unsupported", doc=DOC)]),
                               _res("unsupported", [dif("CLIENT TRACKING ON", "unsupported")])))
        m["cases"][-1]["results"]["tideskv"], m["cases"][-1]["results"]["kvrocks"] = (
            _res("unsupported", [dif("CLIENT TRACKING ON", "unsupported")]),
            _res("unsupported", [dif("CLIENT TRACKING ON", "unsupported", doc=DOC)]))
        items = uc_report_text.f1_items(m)
        self.assertFalse([x for x in items["kvrocks_pros"] if "tracking" in x])  # Kvrocks documents the gap, it is no pro
        self.assertTrue([x for x in items["tideskv_cons"] if "CLIENT TRACKING" in x])  # TidesKV's undocumented gap stays a con

    def test_f1_items_are_symmetric(self):
        swap = {"tideskv": "kvrocks", "kvrocks": "tideskv"}
        flipped = json.loads(json.dumps(F1_MATRIX))
        for c in flipped["cases"]:
            c["results"] = {swap[k]: v for k, v in c["results"].items()}
        a, b = uc_report_text.f1_items(F1_MATRIX), uc_report_text.f1_items(flipped)
        for key in ("pros", "cons"):
            self.assertEqual(sorted(x.replace("TidesKV", "A").replace("Kvrocks", "B") for x in a["tideskv_" + key]),
                             sorted(x.replace("Kvrocks", "A").replace("TidesKV", "B") for x in b["kvrocks_" + key]))


if __name__ == "__main__":
    unittest.main()
