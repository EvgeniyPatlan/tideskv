# TidesKV vs Apache Kvrocks: quick run, 16 Nov 2026

> **This is a quick run.** Every case ran once, on data small enough to fit in memory. It shows behaviour and
> correctness, not disk-bound performance, and the suite draws no verdicts from it. Treat the ratios as early signals.
> Full-size runs with three repetitions are needed before any claim.

TidesKV 0.1.0 at commit 5db560b against `apache/kvrocks` 2.17.0 (pinned digest), with memtier_benchmark 2.1.4, on an
i9-12900HK with NVMe under Linux 6.8. The full generated report is [report.md](report.md), and the machine-readable
results are in [summary.json](summary.json). The method is in [bench/compare/README.md](../../../bench/compare/README.md).

## Summary

Ratios above 1 favour TidesKV. Each TidesKV figure is compared with the best Kvrocks configuration for that metric.

| Case | Headline metric | TidesKV | Kvrocks | Ratio | Reading |
|---|---|---|---|---|---|
| U1 skewed reads, cache | ops/s, Gaussian keys | 291,096 | 197,297 | 1.48 | TidesKV ahead |
| U2 every key expires | disk left at end | 1.14 GiB | 449 MiB | 0.38 | Kvrocks ahead |
| U3 small in-place updates | ops/s | 274,932 | 195,217 | 1.41 | TidesKV ahead |
| U4 hashes as records | ops/s, mixed H* commands | 207,734 | 155,372 | 1.34 | TidesKV ahead |
| U5 1 / 4 / 16 KB values | ops/s at 4 KB | 194,039 | 129,592 | 1.50 | TidesKV ahead |
| U6 durable writes (fsync) | ops/s, 64 connections | 4,338 | 2,239 | 1.94 | not usable: disk fsync rate unstable |
| U7 kill and restart | acknowledged writes lost | 0 | 0 | 1.00 | even |
| U8 import footprint | disk after load | 1.50 GiB | 1.46 GiB | 0.97 | even |
| F1 Redis 7.2 compatibility | cases identical to Redis, of 171 | 143 | 121 | – | TidesKV ahead; it refuses more commands |

## Where TidesKV did better

- Throughput 1.3–1.5× higher on reads, small updates, hashes and 4–16 KB values.
- Bulk import 2.5× faster with memtier's load (266k against 107k ops/s).
- Back to normal latency sooner after most restarts, for example 228 ms against 1,086 ms after one SIGKILL.
- Matched Redis reply for reply in every case for strings, expiry, hashes, lists, sets, sorted sets and transactions.
- Smaller on disk after U3's aging: 361 MiB against 634 MiB.

## Where Kvrocks did better, or TidesKV falls short

- Space after expiry: TidesKV kept 1.14 GiB when every key had expired, against 449 MiB. TidesKV does not yet reclaim
  expired data. Kvrocks also kept a 1.3–1.5 GiB WAL archive, reported separately.
- Peak disk during TTL churn: 1.89 GiB against 0.93–1.20 GiB.
- First reply after a restart: Kvrocks answered sooner, about 80–110 ms against 90–190 ms.
- TidesKV refuses some server and introspection commands Redis accepts, such as `SLOWLOG` and `LATENCY`.
- Kvrocks has Streams, Bitmaps, HyperLogLog, Geo, Pub/Sub, JSON, Search and Bloom. TidesKV has none of these yet.
- Kvrocks has a long release history and 26 named production users. TidesKV is pre-1.0 and its on-disk format may change.

## Correctness

- Every cell passed its reply checks on both servers. No failed cells.
- U7 killed each server with SIGTERM and with SIGKILL at three points under strict durability. Neither lost or
  corrupted an acknowledged write.

## What these numbers do not show yet

- **Disk-bound performance.** The data fitted in memory on both sides, so U1's "bigger than RAM" question is not answered.
- **Fair latency comparison.** Every run here pushed both servers to their maximum. Latency is only compared at equal
  offered load, which runs in full mode.
- **Durable-write speed (U6).** The disk's own fsync rate fell from 1,621/s to about 230/s during the run, and most
  cells were flagged unstable.
- **U5 disk sizes for TidesKV.** TidesKV read exactly 448 MiB at all three value sizes, which looks like whole
  preallocated segments. Do not quote those figures until that is checked.
- **Repeatability.** One repetition only. The full suite runs three and reports the median and range.
