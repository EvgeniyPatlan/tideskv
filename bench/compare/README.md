# TidesKV vs Kvrocks comparison benchmark

`run.sh` benchmarks TidesKV and Kvrocks 2.17.0 under identical Docker limits and writes
`build/bench/<UTC timestamp>/report.md`.

```bash
bench/compare/run.sh --smoke   # start both servers at both durability levels, check parity, ~5 min
bench/compare/run.sh --quick   # small dataset, 10 s warm-up, 30 s runs, about 25 min
bench/compare/run.sh --medium  # ~8 GB dataset, full's timings, needs ~85 GB free, ~2.5 h
bench/compare/run.sh           # full: ~24 GB dataset, 60 s warm-up, 180 s runs, ~3 h
bench/compare/run.sh --large   # W7 only: 256 KiB values, 32 GiB per server, needs ~80 GiB free, ~1.5-2 h
bench/compare/run.sh --large --smoke  # W7 on 512 keys (128 MiB), 10 s warm-up, 30 s runs, ~15 min
```

Options:
- `--relaxed-mode interval`: TidesKV's relaxed level uses `sync-mode interval` instead of `none`.
- `--dirty`: build TidesKV from the working tree instead of HEAD.
- `--out DIR`: write the run somewhere other than `build/bench/`. The run refuses a `DIR/data` that is not empty.
- `--no-w7-sweep`: with `--large` (an error without it), skip the R32 pass at workers 16.

Environment:
- `SERVER_CPUS` (default `0-3`), `SERVER_MEM` (`8g`) and `CLIENT_CPUS` (`8-15`) set the limits.
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

## W7: large values on a miss-heavy cache (`--large`)

W7 follows the Percona post "Evaluating Kvrocks, part 2": 256 KiB values, a working set up to 32x the
block cache, uniform reads, 10% writes. It runs instead of W1-W6 and replaces their report.

- Data: 131,072 strings `L:<n>` of 262,144 random bytes (32 GiB logical), loaded once per server with SET.
- Servers, one after another: `tideskv` (defaults; its value log already separates values over 1 KiB),
  `kvrocks`, and `kvrocks-blob` (`rocksdb.enable_blob_files yes`, `rocksdb.min_blob_size 4096`). Each server's
  data is deleted before the next one loads, so only one server's data is on disk at a time. Free disk must be
  2x logical + 8 GiB + the fio file (80 GiB at full size).
- Compression is off on all three servers in W7 only (`compression no`, `rocksdb.compression no`; parity reads it
  back): memtier's `--random-data` compresses about 3x under lz4, which would shrink the 32x working set to about
  10x. The report notes a server whose after-load bytes are under 80% of logical ("values compressed").
- Both Kvrocks configs get `rocksdb.wal_ttl_seconds 0` and `rocksdb.wal_size_limit_mb 0` in W7 only (no replica
  needs the WAL archive, whose 3 h / 16 GiB default would keep M32's rewrites on disk); parity reads them back.
- Both servers get a 1 GiB block cache and `SERVER_MEM` defaults to `4g`, so 32x is miss-heavy even after the
  page cache. Workers stay at 4, relaxed durability.
- Phases, each after a restart and a page-cache drop of the data dir: R1, R4, R32 (reads only over 4,096, 16,384
  and 131,072 keys: 1x, 4x and 32x the block cache), R32 again at workers 16 for every server (reads in flight;
  skip with `--no-w7-sweep`), then M32 (90/10 GET/SET at 32x). The workers-16 pass runs before M32 so that it reads
  the same data as R32.
- memtier: 8 threads × 4 clients, pipeline 1, `--distinct-client-seed --randomize`. `THREADS`/`CLIENTS` do not
  apply; meta.json's `w7_client` says so.
- Before any server runs, fio (`FIO_IMAGE` in `lib.sh`, pinned by digest) reads 256 KiB blocks at random with
  O_DIRECT at queue depths 1-32, 30 s each, from an 8 GiB file under the run directory. If the image cannot be
  pulled, the step is skipped and `meta.json`'s `fio_status` says so. `FIO_RUNTIME`, `FIO_SIZE` and `FIO_QDS`
  override it; `--large --smoke` uses 5 s and 1 GiB.

The W7 report has, per server and phase: GET/s, SET/s, GET p50/p99/p99.9, SET p99, CPU µs/op, peak memory,
client CPU, and, from the server cgroup's io.stat over the timed run, device read and write MB/s, disk reads per
GET (bytes read ÷ GETs × 256 KiB), write amplification (M32: bytes written ÷ SETs × 256 KiB), plus aqu-sz and
r_await from `/proc/diskstats` sampled once a second for the busiest device between markers around the timed run
(host-wide counters, blank when the server did no I/O), and Kvrocks' data-block cache hit rate
(`block_cache_data_hit`/`_miss` from INFO, before and after the run; "– (index only)" for Kvrocks+BlobDB, whose
values bypass the block cache). A run whose memtier used over 85% of `CLIENT_CPUS` is flagged "client-bound?". The
fio curve follows the table.

- The harness syncs before the timed run, so the warm-up's dirty pages are not charged to it. Write amp counts the
  window plus its dirty pages (io.stat after a sync); "write amp incl. settle" also counts what the server wrote
  while it settled afterwards.
- Every reply is checked from memtier's byte counts: each SET must answer `+OK` (5 bytes), and GETs must hit with
  256 KiB values. Otherwise the cell is an ERR "errors: ...": memtier counts `-ERR` replies (a full disk) as done.
- A failed page-cache drop is a warning in the phase's status and under the table. At full size, an R4/R32 cell
  with under 0.5 × (stored ÷ logical bytes, at most 1) disk reads per GET is flagged "cache not dropped?" (about 0.75
  and 0.97 are expected for uncompressed data).

## Reading the report

- The ratio is TidesKV ops/s divided by Kvrocks ops/s; above 1 means TidesKV is ahead.
- Latencies are memtier's totals, in milliseconds.
- CPU µs/op is the server container's CPU time (its cgroup's `cpu.stat usage_usec`, read before and after the timed run) per request. Peak memory comes from `docker stats`.
- Disk B/op is the server cgroup's `io.stat` bytes written over the timed run per request (never `docker stats`, which counts block I/O several times here). TidesKV compaction B/op is the sum over column families of the `cf_*_compaction_bytes_written` growth in `INFO persistence` (saved as `<W>.info0`/`.info1`) per request; Kvrocks has no such counter. Runs from before these files existed show blank cells.
- Disk B/op counts only writes inside the timed window, so later writeback (notably relaxed Kvrocks, which flushes after the window) is excluded.
- Compaction bytes are counted when a compaction job commits, so over a 60-180 s window the values are noisy: a job that straddles the window edge lands wholly on one side.
- W6 has no post-window settle, so compaction pushed past the window is not counted.
- `ERR` cells name their cause under the table.
- The W6 line shows the achieved DEL rate against the target. A server below 90% of the target gets a note, because its reads ran against fewer deletes.

## Profiling

`profile.sh` runs TidesKV alone under perf while a workload's timed run is going:

```bash
bench/compare/profile.sh [--quick|--medium] [--strict] [--seconds N] --out DIR W1 [W3 ...]
```

- It builds two images from the committed tree (it refuses uncommitted changes under the image paths), tagged by content so reruns reuse them: `tideskv-prof:<hash>` is the server built with `-fno-omit-frame-pointer`, and `tideskv-perf:<hash>` is built `FROM` it with `linux-tools-generic`. perf must run in the second image so that it sees the same binaries at the same paths and can name TidesKV and TidesDB frames.
- It loads the data set once, profiles each named workload (W1..W6) at the relaxed level, and with `--strict` restarts the server in strict mode and profiles them again. The size defaults to `--medium`. `WARMUP` and `DURATION` default to 40 and 60 s here and can be overridden by env.
- perf samples for `--seconds` (default 30) starting 15 s into the timed run, at 499 Hz with frame-pointer call graphs. Keep `--seconds` at or below `DURATION - 15`.
- The data dir is removed at the end, as in `run.sh`.

Per workload and level (`<level>` is `relaxed` or `strict`), under `DIR`:

- `perf/<level>-<W>.data`: the raw perf data, kept for offline reading.
- `perf/<level>-<W>.flat.txt`: self time by library and symbol. `perf/<level>-<W>.tree.txt`: inclusive time with callers and callees. `perf/<level>-<W>.threads.txt`: self time by thread name (for example `compaction.` threads).
- `info-<level>-<W>-{before,after}.txt`: `INFO all` around the run. The usual memtier results are under `DIR/tideskv/<level>/`.

To read a `.data` file again, run `perf report` in the perf image, for example:

```bash
docker run --rm -v "$PWD/DIR/perf:/out" -w /out tideskv-perf:<hash> \
  perf report -i relaxed-W1.data --no-children --stdio --percent-limit 0.7 -g none --sort dso,sym
```

The perf sidecar shares the server's pid namespace and has profiling capabilities only (`PERFMON`, `SYS_PTRACE`, and `IPC_LOCK` for perf's locked buffers). It is pinned to CPUs 4-7 and is removed when perf finishes. The frame-pointer build is not the build `run.sh` measures, so do not compare throughput between the two.

## After Ctrl-C

An interrupted run leaves `build/bench/<ts>/data` holding files owned by root and uid 999. Delete them from a container:

```bash
docker run --rm --name tideskv-bench-clean-manual -v "$PWD/build/bench/<ts>/data:/d" --entrypoint find redis:7.2.5 /d -mindepth 1 -delete
```

## Scaling up (S4)

For the S4 sign-off, run on a host where `wl_sizes full` in `workloads.sh` can be raised to 3× host RAM, and set `SERVER_MEM` to the host's RAM minus a margin.
