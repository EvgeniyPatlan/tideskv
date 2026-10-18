# TidesKV vs Kvrocks comparison benchmark

`run.sh` benchmarks TidesKV and Kvrocks 2.17.0 under identical Docker limits and writes
`build/bench/<UTC timestamp>/report.md`.

```bash
bench/compare/run.sh --smoke   # start both servers at both durability levels, check parity, ~5 min
bench/compare/run.sh --quick   # small dataset, 10 s warm-up, 30 s runs, ~10-15 min
bench/compare/run.sh           # full: ~24 GB dataset, 60 s warm-up, 180 s runs, ~3 h
```

Options:
- `--relaxed-mode interval`: TidesKV's relaxed level uses `sync-mode interval` instead of `none`.
- `--dirty`: build TidesKV from the working tree instead of HEAD.
- `--out DIR`: write the run somewhere other than `build/bench/`.

Environment:
- `SERVER_CPUS` (default `0-3`), `SERVER_MEM` (`8g`) and `CLIENT_CPUS` (`8-11`) set the limits.
- `WARMUP` and `DURATION` are in seconds.
- `THREADS` and `CLIENTS` are memtier's settings.

The TidesKV image is tagged by the content of `src`, `cmake`, `CMakeLists.txt`, `CMakePresets.json`,
`third_party` and the Dockerfile, and is built only when that tag is missing. A commit that changes
only bench scripts reuses the image.

## What is measured

| W | Traffic | Dataset |
|---|---|---|
| W1 | SET/GET 1:1, 100 B values, random keys | strings |
| W2 | SET/GET 1:10 | strings |
| W3 | HSET/HGET on 10k-field hashes | hashes |
| W4 | ZADD / ZRANGEBYSCORE LIMIT 10 | zsets |
| W5 | LPUSH/LPOP | lists |
| W6 | W1 traffic plus a second client deleting 1k-field hashes at a fixed rate | strings + bighashes |

Every workload runs at two durability levels:

| Level | TidesKV | Kvrocks |
|---|---|---|
| relaxed | `sync-mode none` | `rocksdb.write_options.sync no` |
| strict | `sync-mode full` | `rocksdb.write_options.sync yes` |

## Fairness rules

- One server runs at a time, on the same cores (`SERVER_CPUS`), with the same memory cap (`SERVER_MEM`, which also caps its page cache), on the same disk.
- memtier runs on separate cores (`CLIENT_CPUS`).
- Both servers use 4 workers, a 2 GB block cache, a 64 MB memtable and LZ4 compression.
- The report's Fairness section shows the values read back from each running server. The run fails if any differs from the plan.
- The data is loaded once per server at the relaxed level. The workloads run W1..W6 at relaxed, then again after a restart at strict. W5 and W6 remove data, so the strict pass sees what the relaxed pass left. Both servers go through the same sequence.
- No host ports are published. All containers are named `tideskv-bench-*` and are removed on exit, including Ctrl-C.

## Reading the report

- The ratio is TidesKV ops/s divided by Kvrocks ops/s; above 1 means TidesKV is ahead.
- Latencies are memtier's totals, in milliseconds.
- CPU µs/op is the server container's CPU time per request.
- `ERR` cells name their cause under the table.

## Scaling up (S4)

For the S4 sign-off, run on a host where `wl_sizes full` in `workloads.sh` can be raised to 3× host RAM, and set `SERVER_MEM` to the host's RAM minus a margin.
