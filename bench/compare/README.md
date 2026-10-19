# TidesKV vs Kvrocks comparison benchmark

`run.sh` benchmarks TidesKV and Kvrocks 2.17.0 under identical Docker limits and writes
`build/bench/<UTC timestamp>/report.md`.

```bash
bench/compare/run.sh --smoke   # start both servers at both durability levels, check parity, ~5 min
bench/compare/run.sh --quick   # small dataset, 10 s warm-up, 30 s runs, about 25 min
bench/compare/run.sh --medium  # ~8 GB dataset, full's timings, needs ~85 GB free, ~2.5 h
bench/compare/run.sh           # full: ~24 GB dataset, 60 s warm-up, 180 s runs, ~3 h
```

Options:
- `--relaxed-mode interval`: TidesKV's relaxed level uses `sync-mode interval` instead of `none`.
- `--dirty`: build TidesKV from the working tree instead of HEAD.
- `--out DIR`: write the run somewhere other than `build/bench/`. The run refuses a `DIR/data` that is not empty.

Environment:
- `SERVER_CPUS` (default `0-3`), `SERVER_MEM` (`8g`) and `CLIENT_CPUS` (`8-11`) set the limits.
  On this host (i9-12900HK) `0-3` is 2 physical cores with SMT, not 4 cores; meta.json records the core count.
- `WARMUP` and `DURATION` are in seconds (defaults 60 and 180, quick 10 and 30, medium as full).
- `THREADS` and `CLIENTS` are memtier's settings (defaults 4 and 50).
- `QUIESCE_MAX` caps each wait for the server to go quiet, in seconds (default 600, quick 60).

The TidesKV image is tagged by the content of `src`, `cmake`, `CMakeLists.txt`, `CMakePresets.json`,
`third_party` and the Dockerfile, and is built only when that tag is missing. A commit that changes
only bench scripts reuses the image.

## What is measured

| W | Traffic | Dataset |
|---|---|---|
| W1 | SET/GET 1:1, 100 B values, random keys | strings |
| W2 | SET/GET 1:10 | strings |
| W3 | HSET/HGET on 10k-field hashes | hashes |
| W4 | ZINCRBY / ZRANGEBYSCORE LIMIT 10 | zsets |
| W5 | LPUSH/LPOP | lists |
| W6 | W1 traffic plus a second memtier deleting 1k-field hashes at a fixed rate | strings + bighashes |

- W4's ZINCRBY picks members from twice the loaded range, so it adds new members: the zsets grow with each server's throughput.
- W6's delete rate (20 DEL/s quick, 100 DEL/s full) is spread over one connection per DEL/s, each rate-limited
  to 1 DEL/s, so the target holds while a DEL answers within 1 s. Keys are drawn at random with replacement,
  so DEL/s counts commands, including DELs of keys that were already deleted.

Every workload runs at two durability levels:

| Level | TidesKV | Kvrocks |
|---|---|---|
| relaxed | `sync-mode none` | `rocksdb.write_options.sync no` |
| strict | `sync-mode full` | `rocksdb.write_options.sync yes` |

## Fairness rules

- One server runs at a time, on the same cores (`SERVER_CPUS`), with the same memory cap (`SERVER_MEM`) and open-file limit (65536), on the same disk.
- Pages a removed container read stay in the host page cache, charged to its dead memory cgroup, and a new container can read them without being charged. So before every server start, the harness runs a host `sync` and drops the data dir's files from the page cache (`dd iflag=nocache count=0` per file). After every server stop it runs `sync` again.
- Before each workload's warm-up, after the load and after the strict restart, the harness waits until the server container uses under 5% of one core and writes at most 1 MiB over 5 s, capped at `QUIESCE_MAX`. The report shows each wait as "settle s"; a capped wait shows as `≥N (cap)`. One workload's background flush and compaction debt then shows up as settle time instead of slowing the next workload.
- memtier runs on separate cores (`CLIENT_CPUS`).
- Both servers use 4 workers, a 2 GB block cache, a 64 MB memtable and LZ4 compression.
- The report's Fairness section shows the values read back from each running server. The run fails if any differs from the plan.
- The data is loaded once per server at the relaxed level. The workloads run W1..W6 at relaxed, then again after a restart at strict. W5 and W6 remove data, so the strict pass sees what the relaxed pass left. Both servers go through the same sequence.
- Every memtier client uses its own seed (`--distinct-client-seed --randomize`) and random data, so no workload replays identical keys or values.
- No host ports are published. All containers are named `tideskv-bench-*` and are removed on exit, including Ctrl-C.

## Reading the report

- The ratio is TidesKV ops/s divided by Kvrocks ops/s; above 1 means TidesKV is ahead.
- Latencies are memtier's totals, in milliseconds.
- CPU µs/op is the server container's CPU time (its cgroup's `cpu.stat usage_usec`, read before and after the timed run) per request. Peak memory comes from `docker stats`.
- `ERR` cells name their cause under the table.
- The W6 line shows the achieved DEL rate against the target. A server below 90% of the target gets a note, because its reads ran against fewer deletes.

## After Ctrl-C

An interrupted run leaves `build/bench/<ts>/data` holding files owned by root and uid 999. Delete them from a container:

```bash
docker run --rm --name tideskv-bench-clean-manual -v "$PWD/build/bench/<ts>/data:/d" --entrypoint find redis:7.2.5 /d -mindepth 1 -delete
```

## Scaling up (S4)

For the S4 sign-off, run on a host where `wl_sizes full` in `workloads.sh` can be raised to 3× host RAM, and set `SERVER_MEM` to the host's RAM minus a margin.
