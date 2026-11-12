#!/usr/bin/env python3
"""Statistics and verdict rule for the use-case suite (stdlib only).

summarize() reduces repetitions to median/lo/hi/n over the non-None values, keeping the error strings of failed
repetitions. ratio() is oriented so that >1 means TidesKV is better. verdict() labels one metric against one Kvrocks
config; case_label() reduces a row's verdicts to one label. flag_under_target() marks a fixed-rate cell that did not
reach its target.
"""
import math
import statistics
from dataclasses import dataclass

MIN_BAND = 0.05
UNDER_TARGET_FRACTION = 0.95
LABEL_TIDESKV = "TidesKV better"
LABEL_KVROCKS = "Kvrocks better"
LABEL_TIE = "tie"
LABEL_NO_VERDICT = "no verdict"


@dataclass(frozen=True)
class Summary:
    median: float
    lo: float
    hi: float
    n: int
    errors: tuple[str, ...]


@dataclass(frozen=True)
class Verdict:
    label: str
    ratio: float | None
    band: float
    note: str


def _all_errors(values: list[float | None], errors: list[str]) -> tuple[str, ...]:
    """Every None in values is covered by an error: given messages first, then 'rep <i> failed' (1-based)."""
    failed = [i for i, v in enumerate(values) if v is None]
    messages = list(errors) + [f"rep {i + 1} failed" for i in failed[len(errors):]]
    if not any(v is not None for v in values):
        messages.append("no data")
    return tuple(messages)


def summarize(values: list[float | None], errors: list[str]) -> Summary:
    """Summarize repetitions; None marks a failed repetition, and every failure is reflected in errors."""
    present = [v for v in values if v is not None]
    all_errors = _all_errors(values, errors)
    if not present:
        return Summary(median=0.0, lo=0.0, hi=0.0, n=0, errors=all_errors)
    return Summary(
        median=statistics.median(present),
        lo=min(present),
        hi=max(present),
        n=len(present),
        errors=all_errors,
    )


def ratio(t: Summary, k: Summary, higher_is_better: bool) -> float | None:
    """TidesKV-over-Kvrocks ratio, oriented so >1 means TidesKV is better; None if either side has errors."""
    if t.errors or k.errors:
        return None
    if higher_is_better:
        num, den = t.median, k.median
    else:
        num, den = k.median, t.median
    if den == 0:
        return 1.0 if num == 0 else math.inf
    return num / den


def _band(t: Summary, k: Summary) -> float:
    """Relative spread of either side with a positive median, floored at MIN_BAND; inf if a zero median has spread."""
    for s in (t, k):
        if s.median == 0 and s.hi > s.lo:
            return math.inf
    spreads = [(s.hi - s.lo) / s.median for s in (t, k) if s.median > 0]
    return max(spreads + [MIN_BAND])


def _error_note(t: Summary, k: Summary) -> str:
    parts = []
    if t.errors:
        parts.append("errors in T: " + "; ".join(t.errors))
    if k.errors:
        parts.append("errors in K: " + "; ".join(k.errors))
    return "; ".join(parts)


def verdict(t: Summary, k: Summary, higher_is_better: bool, quick: bool) -> Verdict:
    """Label one metric; the ratio and band are reported even when no verdict is given."""
    band = _band(t, k)
    r = ratio(t, k, higher_is_better)
    if t.errors or k.errors:
        return Verdict(LABEL_NO_VERDICT, r, band, _error_note(t, k))
    if quick:
        return Verdict(LABEL_NO_VERDICT, r, band, "quick run")
    if t.n < 2 or k.n < 2:
        return Verdict(LABEL_NO_VERDICT, r, band, "single repetition")
    if math.isinf(band):
        return Verdict(LABEL_TIE, r, band, "zero median with spread")
    if r > 1 + band:
        return Verdict(LABEL_TIDESKV, r, band, "")
    if r < 1 / (1 + band):
        return Verdict(LABEL_KVROCKS, r, band, "")
    return Verdict(LABEL_TIE, r, band, "")


def best_kvrocks(configs: dict[str, Summary], higher_is_better: bool) -> str:
    """Name of the Kvrocks config with the best median, ignoring configs that have errors."""
    valid = {name: s for name, s in configs.items() if not s.errors and s.n > 0}
    if not valid:
        raise ValueError("no Kvrocks config without errors")
    pick = max if higher_is_better else min
    return pick(valid, key=lambda name: valid[name].median)


def case_label(verdicts: list[Verdict]) -> str:
    """Row label: 'mixed' when TidesKV and Kvrocks both win some metric, else the headline (first) verdict's label."""
    if not verdicts:
        return LABEL_NO_VERDICT
    labels = {v.label for v in verdicts}
    if LABEL_TIDESKV in labels and LABEL_KVROCKS in labels:
        return "mixed"
    return verdicts[0].label


def flag_under_target(achieved: float, target: float) -> bool:
    """True when the achieved rate is below 95% of the target."""
    if target <= 0:
        raise ValueError(f"target must be positive, got {target}")
    return achieved / target < UNDER_TARGET_FRACTION
