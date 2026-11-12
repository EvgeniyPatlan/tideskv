"""Tests for uc_verdict.py (run: python3 -m unittest bench/compare/test_uc_verdict.py)."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import uc_verdict as uv  # noqa: E402


class VerdictTest(unittest.TestCase):
    def test_clear_win(self):
        t = uv.summarize([110.0, 108.0, 112.0], [])
        k = uv.summarize([100.0, 99.0, 101.0], [])
        v = uv.verdict(t, k, higher_is_better=True, quick=False)
        self.assertEqual(v.label, "TidesKV better")
        self.assertAlmostEqual(v.ratio, 1.10)

    def test_inside_spread_is_tie(self):
        t = uv.summarize([105.0, 90.0, 120.0], [])
        k = uv.summarize([100.0, 100.0, 100.0], [])
        v = uv.verdict(t, k, higher_is_better=True, quick=False)
        self.assertEqual(v.label, "tie")
        self.assertAlmostEqual(v.band, 30.0 / 105.0)

    def test_lower_is_better_orients(self):
        t = uv.summarize([1.0, 1.0, 1.0], [])
        k = uv.summarize([2.0, 2.0, 2.0], [])
        v = uv.verdict(t, k, higher_is_better=False, quick=False)
        self.assertEqual(v.label, "TidesKV better")
        self.assertAlmostEqual(v.ratio, 2.0)

    def test_errors_block_verdict(self):
        t = uv.summarize([10.0, 10.0, 10.0], [])
        k = uv.summarize([9.0, None, 9.0], ["rep2: server OOM"])
        v = uv.verdict(t, k, higher_is_better=True, quick=False)
        self.assertEqual(v.label, "no verdict")
        self.assertIsNone(v.ratio)
        self.assertIn("K", v.note)
        self.assertIn("rep2: server OOM", v.note)
        self.assertIsNone(uv.ratio(t, k, higher_is_better=True))

    def test_zero_metrics(self):
        z = uv.summarize([0.0, 0.0, 0.0], [])
        self.assertEqual(uv.verdict(z, z, higher_is_better=False, quick=False).label, "tie")
        self.assertEqual(uv.ratio(z, z, higher_is_better=False), 1.0)
        five = uv.summarize([5.0, 5.0, 5.0], [])
        v = uv.verdict(z, five, higher_is_better=False, quick=False)
        self.assertEqual(v.label, "TidesKV better")
        self.assertEqual(v.ratio, float("inf"))

    def test_zero_side_higher_is_better_direction(self):
        z = uv.summarize([0.0, 0.0, 0.0], [])
        five = uv.summarize([5.0, 5.0, 5.0], [])
        # TidesKV at 0 ops/s loses to Kvrocks at 5 ops/s.
        v = uv.verdict(z, five, higher_is_better=True, quick=False)
        self.assertEqual(v.label, "Kvrocks better")
        self.assertEqual(v.ratio, 0.0)
        # TidesKV at 5 ops/s beats Kvrocks at 0 ops/s.
        v = uv.verdict(five, z, higher_is_better=True, quick=False)
        self.assertEqual(v.label, "TidesKV better")
        self.assertEqual(v.ratio, float("inf"))

    def test_zero_median_with_spread_is_tie(self):
        t = uv.summarize([0.0, 0.0, 10.0], [])
        k = uv.summarize([5.0, 5.0, 5.0], [])
        v = uv.verdict(t, k, higher_is_better=False, quick=False)
        self.assertEqual(v.label, "tie")
        self.assertEqual(v.note, "zero median with spread")
        self.assertEqual(v.band, float("inf"))

    def test_zero_median_with_spread_higher_is_better_is_tie(self):
        t = uv.summarize([0.0, 0.0, 100.0], [])
        k = uv.summarize([50.0, 50.0, 50.0], [])
        v = uv.verdict(t, k, higher_is_better=True, quick=False)
        self.assertEqual(v.label, "tie")
        self.assertEqual(v.note, "zero median with spread")

    def test_zero_median_without_spread_still_wins(self):
        z = uv.summarize([0.0, 0.0, 0.0], [])
        five = uv.summarize([5.0, 5.0, 5.0], [])
        v = uv.verdict(z, five, higher_is_better=False, quick=False)
        self.assertEqual(v.label, "TidesKV better")
        self.assertEqual(v.ratio, float("inf"))

    def test_band_edge_is_tie(self):
        # Medians 105 vs 100 give ratio 1.05; the band is the 0.05 floor, so ratio == 1 + band is a tie.
        t = uv.summarize([105.0, 105.0, 105.0], [])
        k = uv.summarize([100.0, 100.0, 100.0], [])
        v = uv.verdict(t, k, higher_is_better=True, quick=False)
        self.assertEqual(v.band, 0.05)
        self.assertEqual(v.ratio, 1 + v.band)
        self.assertEqual(v.label, "tie")

    def test_quick_no_verdict(self):
        t = uv.summarize([110.0, 108.0, 112.0], [])
        k = uv.summarize([100.0, 99.0, 101.0], [])
        v = uv.verdict(t, k, higher_is_better=True, quick=True)
        self.assertEqual(v.label, "no verdict")
        self.assertEqual(v.note, "quick run")
        self.assertAlmostEqual(v.ratio, 1.10)

    def test_single_repetition_no_verdict(self):
        t = uv.summarize([110.0], [])
        k = uv.summarize([100.0, 99.0, 101.0], [])
        v = uv.verdict(t, k, higher_is_better=True, quick=False)
        self.assertEqual(v.label, "no verdict")
        self.assertEqual(v.note, "single repetition")

    def test_summarize_all_none(self):
        s = uv.summarize([None, None], ["a", "b"])
        self.assertEqual((s.median, s.lo, s.hi, s.n), (0.0, 0.0, 0.0, 0))
        self.assertEqual(s.errors, ("a", "b", "no data"))

    def test_summarize_none_without_error_text_blocks_verdict(self):
        s = uv.summarize([1.0, None, 3.0], [])
        self.assertEqual(s.errors, ("rep 2 failed",))
        self.assertEqual((s.median, s.lo, s.hi, s.n), (2.0, 1.0, 3.0, 2))
        t = uv.summarize([2.0, 2.0, 2.0], [])
        v = uv.verdict(t, s, higher_is_better=True, quick=False)
        self.assertEqual(v.label, "no verdict")
        self.assertIsNone(v.ratio)

    def test_best_kvrocks_picks_best(self):
        configs = {
            "a": uv.summarize([10.0, 10.0], []),
            "b": uv.summarize([30.0, 30.0], []),
            "c": uv.summarize([99.0, 99.0], ["boom"]),
        }
        self.assertEqual(uv.best_kvrocks(configs, higher_is_better=True), "b")
        self.assertEqual(uv.best_kvrocks(configs, higher_is_better=False), "a")

    def test_best_kvrocks_excludes_empty_configs(self):
        configs = {
            "empty": uv.summarize([None, None], ["x"]),
            "ok": uv.summarize([7.0, 7.0], []),
        }
        self.assertEqual(uv.best_kvrocks(configs, higher_is_better=True), "ok")

    def test_best_kvrocks_raises_when_none_remain(self):
        configs = {
            "a": uv.summarize([1.0, 1.0], ["boom"]),
            "b": uv.summarize([None], []),
        }
        with self.assertRaises(ValueError):
            uv.best_kvrocks(configs, higher_is_better=True)

    def test_mixed(self):
        win = uv.Verdict("TidesKV better", 1.2, 0.05, "")
        loss = uv.Verdict("Kvrocks better", 0.8, 0.05, "")
        tie = uv.Verdict("tie", 1.0, 0.05, "")
        self.assertEqual(uv.case_label([win, loss, tie]), "mixed")
        self.assertEqual(uv.case_label([win, tie]), "TidesKV better")
        self.assertEqual(uv.case_label([tie, win]), "tie")

    def test_case_label_empty_is_no_verdict(self):
        self.assertEqual(uv.case_label([]), "no verdict")

    def test_under_target_rate(self):
        self.assertTrue(uv.flag_under_target(94999, 100000))
        self.assertFalse(uv.flag_under_target(95000, 100000))

    def test_under_target_rejects_non_positive_target(self):
        with self.assertRaises(ValueError):
            uv.flag_under_target(10, 0)
        with self.assertRaises(ValueError):
            uv.flag_under_target(10, -5)


if __name__ == "__main__":
    unittest.main()
