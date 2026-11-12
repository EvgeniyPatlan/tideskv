# TidesKV vs Kvrocks

TidesKV `abc1234`, `apache/kvrocks:2.17.0`, `redislabs/memtier_benchmark:2.1.4`; size `quick`; warm-up 10s, timed 30s; server cpus `0-3`, memory `8g`; host test; t.

A ratio above 1 means TidesKV is ahead on throughput.

## Relaxed durability (TidesKV `sync-mode none` vs Kvrocks `rocksdb.write_options.sync no`)

| W | TidesKV ops/s | Kvrocks ops/s | ratio | TidesKV p50/p99/p999 ms | Kvrocks p50/p99/p999 ms |
|---|---|---|---|---|---|
| W1 | 138764 | 138764 | 1.00 | 0.015 / 0.039 / 0.135 | 0.015 / 0.039 / 0.135 |
| W2 | 138764 | 138764 | 1.00 | 0.015 / 0.039 / 0.135 | 0.015 / 0.039 / 0.135 |
| W3 | 139936 | 139936 | 1.00 | 0.015 / 0.031 / 0.119 | 0.015 / 0.031 / 0.119 |
| W4 | 139936 | 139936 | 1.00 | 0.015 / 0.031 / 0.119 | 0.015 / 0.031 / 0.119 |
| W5 | 139936 | 139936 | 1.00 | 0.015 / 0.031 / 0.119 | 0.015 / 0.031 / 0.119 |
| W6 | 138764 | 138764 | 1.00 | 0.015 / 0.039 / 0.135 | 0.015 / 0.039 / 0.135 |

| W | TidesKV CPU µs/op | Kvrocks CPU µs/op | TidesKV peak mem | Kvrocks peak mem | TidesKV settle s | Kvrocks settle s | TidesKV disk B/op | Kvrocks disk B/op | TidesKV compaction B/op |
|---|---|---|---|---|---|---|---|---|---|
| W1 | 14.4 | 14.4 | 2.00 GiB | 2.00 GiB | – | – |  |  |  |
| W2 | 14.4 | 14.4 | 2.00 GiB | 2.00 GiB | – | – |  |  |  |
| W3 | 14.3 | 14.3 | 2.00 GiB | 2.00 GiB | – | – |  |  |  |
| W4 | 14.3 | 14.3 | 2.00 GiB | 2.00 GiB | – | – |  |  |  |
| W5 | 14.3 | 14.3 | 2.00 GiB | 2.00 GiB | – | – |  |  |  |
| W6 | 14.4 | 14.4 | 2.00 GiB | 2.00 GiB | – | – |  |  |  |

W6 delete traffic: TidesKV 138764.0, Kvrocks 138764.0 DEL/s.

## Strict durability (TidesKV `sync-mode full` vs Kvrocks `rocksdb.write_options.sync yes`)

| W | TidesKV ops/s | Kvrocks ops/s | ratio | TidesKV p50/p99/p999 ms | Kvrocks p50/p99/p999 ms |
|---|---|---|---|---|---|
| W1 | 138764 | 138764 | 1.00 | 0.015 / 0.039 / 0.135 | 0.015 / 0.039 / 0.135 |
| W2 | 138764 | 138764 | 1.00 | 0.015 / 0.039 / 0.135 | 0.015 / 0.039 / 0.135 |
| W3 | 139936 | 139936 | 1.00 | 0.015 / 0.031 / 0.119 | 0.015 / 0.031 / 0.119 |
| W4 | 139936 | 139936 | 1.00 | 0.015 / 0.031 / 0.119 | 0.015 / 0.031 / 0.119 |
| W5 | 139936 | 139936 | 1.00 | 0.015 / 0.031 / 0.119 | 0.015 / 0.031 / 0.119 |
| W6 | 138764 | 138764 | 1.00 | 0.015 / 0.039 / 0.135 | 0.015 / 0.039 / 0.135 |

| W | TidesKV CPU µs/op | Kvrocks CPU µs/op | TidesKV peak mem | Kvrocks peak mem | TidesKV settle s | Kvrocks settle s | TidesKV disk B/op | Kvrocks disk B/op | TidesKV compaction B/op |
|---|---|---|---|---|---|---|---|---|---|
| W1 | 14.4 | 14.4 | 2.00 GiB | 2.00 GiB | – | – |  |  |  |
| W2 | 14.4 | 14.4 | 2.00 GiB | 2.00 GiB | – | – |  |  |  |
| W3 | 14.3 | 14.3 | 2.00 GiB | 2.00 GiB | – | – |  |  |  |
| W4 | 14.3 | 14.3 | 2.00 GiB | 2.00 GiB | – | – |  |  |  |
| W5 | 14.3 | 14.3 | 2.00 GiB | 2.00 GiB | – | – |  |  |  |
| W6 | 14.4 | 14.4 | 2.00 GiB | 2.00 GiB | – | – |  |  |  |

W6 delete traffic: TidesKV 138764.0, Kvrocks 138764.0 DEL/s.

## Disk and load

| Server | logical | after load | end | space amp (end/logical) | load time | settle after load | settle after restart |
|---|---|---|---|---|---|---|---|
| TidesKV | 1.00 GiB | 2.00 GiB | 3.00 GiB | 3.00 | 60s | – | – |
| Kvrocks | 1.00 GiB | 2.00 GiB | 3.00 GiB | 3.00 | 60s | – | – |

## Fairness

Settings read back from each running server with CONFIG GET.

### TidesKV

```
workers 4
```

### Kvrocks

```
workers 4
```

