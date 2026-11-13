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
- `TIDESKV_EXTRA_CONF` (experiments) appends config lines, separated by `;`, to the TidesKV conf, for example
  `TIDESKV_EXTRA_CONF='btree-block-size 16kb;l0-stall-threshold 32'`. The lines come last, so they win. A value that
  changes a key the parity check reads back makes `run.sh`'s parity check fail and its cells show `ERR`; use it with
  `profile.sh` or accept that the run is not a comparison.
- `ALLOCATOR` (`profile.sh` only; `mimalloc`, the default, or `system`) sets `TIDESKV_ALLOCATOR` for the profiling
  server image. A value other than `mimalloc` is added to the image tag, so each allocator gets its own image.

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
- Both servers use 4 workers, a 2 GB block cache, a 256 MB memtable and LZ4 compression.
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

## Use-case suite

`usecases.sh` compares TidesKV with Kvrocks 2.17.0 on eight use cases (U1-U8) and two non-performance parts (F1, F2).
Where `run.sh` measures workloads, this suite asks questions a user would ask: does a cache bigger than RAM hold up,
does disk usage follow expiring data, what does a durable write cost, is an acknowledged write ever lost. It reuses
`run.sh`'s harness (`lib.sh`): the same pinned images, the same CPU, memory and file limits, the same parity read-back,
and the same page-cache and settle rules as under "Fairness rules" above. Its output is `<out>/report.md` plus
`summary.json`; nothing in `run.sh` or W1-W7 changes.

```bash
bench/compare/usecases.sh --quick --out build/bench/uc-quick   # every case, 1 repetition, small data: a check of the suite, not a result
bench/compare/usecases.sh --out build/bench/uc-full            # full: 3 repetitions
bench/compare/usecases.sh U6 U7 --out build/bench/uc-full      # only these cases (U1..U8, F1)
bench/compare/uc_report.py build/bench/uc-full                 # rebuild report.md and summary.json from the run directory
```

Options: `--quick`, `--dirty` (build TidesKV from the working tree), `--out DIR`, and any number of case names.
`SERVER_CPUS`, `SERVER_MEM`, `CLIENT_CPUS` and `QUIESCE_MAX` (default 600 s, quick 60 s) work as for `run.sh`.
`usecases.sh` runs `uc_report.py` at the end and exits with its status; `uc_report.py` exits 1 when any cell failed. The host needs Docker, `fio` is run from its pinned image, and Python
tools (U7's ack-writer and F1) run from the pinned `PY_IMAGE` in `lib.sh`.

Resume: running `usecases.sh` again with the same `--out` skips every repetition that has a `done` file and redoes the
rest from an empty data directory, so an interrupted run (Ctrl-C, a crashed host) never mixes partial files. It refuses
(exit 2, naming the fields) when the run directory's `meta.json` has another mode, TidesKV image (tag or id), Kvrocks
image digest, `SERVER_CPUS`, `SERVER_MEM` or `CLIENT_CPUS`, so one report never mixes two configurations; each rep dir
also gets the images it ran with (`images.done`, and `images.closed` for pass A). U1 and U3 run in two passes (see
"Method"); a repetition whose pass A is missing redoes pass A and then pass B for every server, and a redone U1 or U3
pass A makes U8 recompute that server's repetition. A repetition whose data directory cannot be emptied first is
recorded as failed, not run on top of old data (U5 checks the same between value sizes).

Before each case loads, the suite checks the free disk against that case's need. Only one server's data is on disk at a
time (each pass starts from an emptied data dir), so the need is the larger of two per-server needs: TidesKV
`logical x 1.4 + 4 GiB`, Kvrocks `logical x 1.6 + 16 GiB + 4 GiB`. memtier's random values do not compress: after U1's
load in the quick run TidesKV held 1.33x and Kvrocks 1.05x of the logical bytes (keys x (value + 12)), so 1.4 covers
TidesKV's data; Kvrocks' 1.6 is about 1.1 for its data plus 0.5 for the transient space of its full COMPACT in U1; 16 GiB
is Kvrocks' WAL archive at its stock `wal_size_limit_mb 16384`; 4 GiB is slack. Even U8 and F1 ask for 20 GiB (the
Kvrocks term). Full-size needs: U1 50.9 GiB (the largest; 54.6 GB), U5 44.7 GiB, U2 30.2 GiB, U4 28.9 GiB, U7 26.3 GiB
(its preload plus every point's acknowledged writes, since the data dir is kept), U3 26.3 GiB, U6 20.2 GiB, U8 and F1
20 GiB. The
fio files (8 GiB for the random-read baseline before U1 in full mode, 1 GiB for each U6 job) are deleted before any
server loads. A case that does not fit is skipped, and its repetitions say why.

### Time

Measured on the dev host (i9-12900HK, NVMe) in quick mode:

| Case | Wall time |
|---|---|
| U1 | 4 min |
| U2 | 45 min (3 servers x 15 min) |
| U3 | 11 min |
| U4 | 2 min |
| U5 | 9 min |
| U6 | 59 min (TidesKV 21 min, Kvrocks 38 min; the strict preload of 1M keys dominates) |
| U7 | 12 min |
| U8, F1 | under 1 min |
| Whole run | 2 h 23 min (22:19 to 00:42 UTC, including the fio baselines and image checks) |

A later quick check of U1, U3, U5, U7 and U8 alone, after the U7 load-point change, took: fio random-read baseline 35 s,
U1 4 min, U3 11 min, U5 9 min, U7 13 min (TidesKV 4.5 min, Kvrocks 8 min), U8 under 1 s; each U6 server adds about 1 min
for its two fio jobs at full-mode settings.

The quick run is a check of the suite, not a measurement: its data fits in the page cache.

A full run is estimated at about 46 h (roughly 2 days); the first full run measures it. The estimate is rough, +-20%,
and built from these components. "Derived" means computed from the full-mode constants in `uc_cases.sh`;
"extrapolated" means scaled from the quick run's timings.

| Case | Estimate | Basis |
|---|---|---|
| U1 | 2.6 h | extrapolated: per server and repetition about 25 min (two closed phases of 4 min, three open phases of 4 min, two loads of 20M keys at 1-2 min each from the quick run's 6.5-11.6 s per 2M keys, settles), 6 server-repetitions, plus 4 min for the fio random-read baseline |
| U2 | 13.5 h | derived: (3600 s churn + 1800 s idle) x 3 configurations x 3 repetitions; excludes load and settle time |
| U3 | 12 h | derived and extrapolated: 3600 s closed + 3 x 600 s open = 90 min per server and repetition, plus about 30 min of loads and aging, 6 server-repetitions |
| U4 | 1 h | extrapolated: about 10 min per server and repetition |
| U5 | 5 h | extrapolated: three value sizes, each loaded and run for 4 min, 9 server-repetitions |
| U6 | 3.1 h | extrapolated: the quick run's strict preload of 1M keys took 21 min (TidesKV) and 38 min (Kvrocks), times 3 repetitions, plus 4 sweep steps of 60 s and 1 min of fio per server and repetition |
| U7 | 9 h (5-11 h) | extrapolated: the quick U7 preload of 200k strict keys took 372 s (TidesKV) and 154 s (Kvrocks), so 2M keys take about 62 and 26 min per repetition (4.4 h in all); derived: 30 points per server and repetition need 1.9M acknowledged writes, 32 min at 1000/s (the quick run saw 430-3700/s), plus about 20 s per point for the baseline probe, restart and verify, about 4.2 h in all |
| U8, F1 | under 5 min | measured |
| Total | about 46 h | sum of the rows |

U1 can be longer: Kvrocks' COMPACT after the closed-loop phases may run up to the 7200 s cap per repetition (up to 6 h more over three
repetitions); the quick run's COMPACT is not representative. The run is resumable, so it can be done in pieces (for
example U6 and U7 alone, or one case per night). Do not start it on a host that runs anything else: server and client are
pinned to cores and the cases measure latency.

### Cases

| Case | What runs | Servers | Durability | Headline metrics |
|---|---|---|---|---|
| U1 Cache bigger than RAM | 20M keys x 1 KB (about 20 GB, 2.5x `SERVER_MEM`), GET:SET 10:1, Gaussian and uniform key patterns closed-loop; then open-loop at 25/50/75% of the lower server's maximum. Kvrocks gets a full COMPACT after the closed-loop phases (for U8). fio's random-read curve is measured before U1 | TidesKV, Kvrocks | relaxed | ops/s; p50/p99/p99.9 at equal offered load; Kvrocks cache hit rate |
| U2 TTL churn and space reclaim | SET with random TTLs 120-1200 s, 1:1 GET (negative lookups over a 1e9-key space) at a fixed rate for 60 min, then 30 min idle; `du` every 60 s | TidesKV, Kvrocks, Kvrocks+cron (`compaction-checker-cron * * * * *`) | relaxed | disk bytes over time and at the end; p99.9 at the fixed rate; end-of-run key count |
| U3 Aged store, small updates | 20M x 200 B, overwritten twice at random, then 4:1 SET:GET for 60 min closed-loop, then open-loop phases; background-debt INFO fields shown beside the numbers | TidesKV, Kvrocks | relaxed | p99/p99.9 per window; windows over 10x median; ops/s; disk write bytes per op (cgroup write bytes / requests of the closed run) |
| U4 Hashes as records | 5M hashes x 20 fields x 50 B; HGET/HSET/HGETALL/HDEL mix 10:6:3:1 | TidesKV, Kvrocks | relaxed | ops/s; p99 |
| U5 Mid-size values | 1, 4 and 16 KB values, GET:SET 4:1 | TidesKV, Kvrocks, Kvrocks+BlobDB (`min_blob_size 4096`) | relaxed | ops/s; p99; disk write bytes per op; on-disk size |
| U6 Durable writes | SET 100 B at 1, 8, 64 and 256 connections, pipeline 1; before each server's sweep fio measures the durable-append floor (4 KiB sequential write + fdatasync) and a reference (4 KiB random write + fsync) | TidesKV, Kvrocks | strict | writes/s; p99; ops/s over the durable-append floor (information) |
| U7 Restart and crash recovery | writer with an acknowledgement log (2 KiB values); SIGTERM and SIGKILL once 12710, 63550 and 114390 writes are acknowledged (about 10, 50 and 90% of the 256 MiB memtable), the same counts on both servers, 5 times each; restart, then verify every acknowledged key | TidesKV, Kvrocks | strict | acknowledged writes lost, corrupt, restart misses; time to PONG; time to recovered p99 (with each server's absolute threshold and the acked count at the signal) |
| U8 Bulk load and footprint | no server of its own: reads U1's load and U3's aging | TidesKV, Kvrocks | relaxed | load ops/s; time to quiet (censored at `QUIESCE_MAX`: "≥ cap"); disk after load, after aging, after COMPACT (Kvrocks only) |
| F1 Command coverage | a command matrix against Redis 7.2 | TidesKV, Kvrocks | relaxed | same / differs / unsupported / documented / extra per command and case |
| F2 Operations facts | a hand-written, sourced table (`f2_operations.md`) | | | facts with links |

Quick mode shrinks the data (U1 2M keys, U3 1M, U4 250k, U5 0.5 GiB per size, U7 200k) and the time (U2 10 min of
writes and 5 idle, U3 5 min, U6 15 s per step, U7 signals after 1271, 6355 and 11439 acknowledged writes, once). U6
keeps its 1M-key preload.

### Method

- **Repetitions.** Full mode runs every cell 3 times, interleaved per repetition (TidesKV, then each Kvrocks
  configuration, then again). Quick mode runs once. Tables give the median and the min-max range.
- **Equal resources.** Same as `run.sh`: server on `SERVER_CPUS` with `SERVER_MEM` (8g) and 65536 open files, client
  on `CLIENT_CPUS`, 4 workers, 2048 MB block cache, 256 MB memtable, LZ4. Every setting is read back from the running
  server before any cell is measured; a mismatch fails the cell. Settings added for one case (the cron and blob
  Kvrocks configs) are read back the same way, and Kvrocks 2.17 rejecting a
  setting fails the cell loudly.
- **Cache accounting (U1).** TidesDB charges its index and bloom blocks to its block cache, as Kvrocks does with
  `cache_index_and_filter_blocks yes` (its default), so both sides have 2048 MB of total read cache.
- **Two passes (U1, U3, full mode).** Pass A loads and runs the closed-loop phases for every server of a repetition.
  Pass B reloads and runs open-loop phases at 25, 50 and 75% of the lower of the two servers' closed-loop ops/s from that
  repetition, so both servers are measured at the same offered load and neither is saturated. Quick mode runs pass A only.
- **Durability.** U1-U5 and U8 are relaxed (TidesKV `sync-mode none`, Kvrocks `write_options.sync no`), which is not
  power-loss safe on either side. U6 and U7 are strict (`full` / `yes`: fsync before each acknowledged write). TidesKV's
  `interval` mode is not compared with Kvrocks `sync yes`.
- **Disk figures** are allocated bytes (`du --block-size=1`, what the filesystem holds, since TidesKV preallocates files)
  taken after the server went quiet, excluding Kvrocks' WAL archive (`wal_ttl_seconds` default 10800), which is
  reported on its own line. Apparent size is shown beside them. Older runs that recorded only apparent size fall back
  to it, and the report labels that.
- **Correctness.** memtier's byte counts check that every SET reply is `+OK` (U1, U2, U3, U5, U6) and every HSET and
  HDEL reply an integer (U4); GETs must hit 99% after a load (U1, U3, U5). U2's GETs (negative lookups by design) and
  U4's HGET and HGETALL replies are not checked. A failed check makes the cell ERR.
- **Data.** LZ4 is on for both servers. memtier's random values are not assumed to compress: the report prints the
  observed on-disk / logical ratio per server and case (U1, U2, U3, U5).
- **U3 and U5 disk writes per op** are the server cgroup's bytes written during the timed run over its requests. They
  are information only, with no verdict: only writes inside the window count, so a server that keeps the data in its
  memtable or defers flushes and compaction past the window looks cheaper than it is (in the quick U3 run TidesKV showed
  7 B/op and Kvrocks 420, with the whole data set inside TidesKV's memtable).
- **U4.** memtier picks each command by weight (HGET 10, HSET 6, HGETALL 3, HDEL 1), so HDEL runs without a paired
  HSET, the same on both servers. The report shows the weights, not memtier's unused `--ratio`.
- **U8 time to quiet** is censored at `QUIESCE_MAX`: a wait that reached it is shown as "≥ cap", not as an error. It
  gets a verdict only when the other side went quiet below the cap by more than the band; when both reached it, none.
- **Page cache.** When the data dir cannot be dropped from the page cache before a server start, the rep dir (or U7
  point dir) gets an `evict-failed` note and the report shows it as a caveat.
- **U2.** GETs are negative lookups over a 1e9-key space, so the live set follows the SET rate times the mean TTL.
  Neither Kvrocks config gets a manual COMPACT. Kvrocks' end-of-run key count needs `DBSIZE SCAN` and may read
  "unverified" when the scan does not finish.
- **U1 cache hit rate** is Kvrocks-only: TidesKV's `INFO` has no hit or miss counters.
- **U6.** The 1M-key strict preload is fsync-bound: in one quick run it took 21 minutes for TidesKV and 38 minutes for
  Kvrocks. The host disk's fsync rate is not steady. An earlier quick run saw it shift between about 1.5k and 6.8k per
  second; in the quick run above, fio measured 1720 writes/s at the start and 216 and 237 writes/s right before the two
  servers' sweeps. A single-repetition U6 result is therefore not trustworthy. Before each server's sweep fio runs two
  jobs at queue depth 1: the durable-append floor, 4 KiB sequential writes with an fdatasync after each (the pattern of
  a write-ahead log), and a reference, 4 KiB random writes with an fsync after each. Both are shown beside the results,
  and each cell's ops/s over its server's durable-append floor is shown as information. Strict mode is configured and
  read back on both servers, but per-ack fsync is not observed by the suite: a server above the floor batches its
  flushes (group commit), and neither server reports an fsync count. A U6 cell whose per-second ops have a coefficient of variation above 0.25 is flagged unstable and gets no
  verdict; one with too few seconds to judge (under 5 after dropping the partial last one) is flagged "stability unknown".
  The rule is applied to U6 cells only.
- **U7.** A process kill (SIGTERM or SIGKILL), not an OS crash or power loss. Every point signals after the same
  number of acknowledged writes on both servers (spec: "kill at the same load point for both"): the runner polls the
  writer's acknowledgement logs every 50 ms and sends the signal once the count reaches N. N is 10, 50 and 90% of the
  256 MiB memtable at 2112 bytes per entry (2 KiB values plus 64 B for the key and the engines' per-entry overhead):
  12710, 63550 and 114390, a tenth of that in quick mode. With values of a few bytes the 90% point would take millions
  of strict writes, so the values are 2 KiB on both servers. The fill counts the writer's writes only, not the preload
  or what a restart's WAL replay leaves in the memtable. A point that does not reach N within `UC_U7_ACK_TIMEOUT`
  (900 s, quick 300 s) is still signalled and verified but shown as ERR. The U7 table shows each point's acked count
  at the signal and each server's absolute recovery threshold (2x its own warm p99). Each point uses a unique key
  prefix and keeps the data dir. Before the restart the page cache of the data dir is dropped. After the restart every
  acknowledged key is read back: `lost` (missing), `corrupt` (wrong value) and `restart_misses` (preload keys not
  found by the probe, reported, not a failure). Timings are measured from the moment `docker run` returned
  (docker's own time is shown separately). Recovery is the start of the first 3 consecutive 100 ms windows whose p99 is
  within 2x the same server's warm pre-kill p99.
- **F1.** Redis 7.2.5 is the oracle. The same commands run on a flushed db against Redis and each server, and every
  command of a case is compared. Statuses: `same`; `differs`; `unsupported` (the server refuses where Redis succeeds);
  `extra` (a command Redis 7.2 does not have); `documented` (the difference is described by the project's own
  documentation, with a link: used only when every difference in a case is documented). Pros and cons are listed
  symmetrically by root cause. F1 is a single run, and at least one case (Kvrocks `EXPIRE b 0` then `GET`) was observed
  timing-dependent, so re-running may change a row. Kvrocks ran with the default `redis-databases` of 0, which is
  configurable.
- **F2.** Facts about operating each server, each with its source and no scores. TidesKV is pre-1.0. A fact that
  could not be checked from the source is marked "not verified".

### Verdict rule

Per metric, the ratio is oriented so that above 1 means TidesKV is better (throughput: TidesKV over Kvrocks; latency,
bytes, recovery time: Kvrocks over TidesKV). The band is the larger of each side's relative spread, (max - min) / median,
and 0.05. Inside the band the verdict is "tie"; outside it, "TidesKV better" or "Kvrocks better". Rules around it:

- No verdict ("no verdict") for quick runs, for single repetitions, and when either side has an error in any
  repetition (a failed repetition is not replaced by the survivors; the note says which).
- A zero median with spread is a tie; 0 against 0 is a tie and 0 against a positive value is a win for the zero side.
- The comparison is against the best valid Kvrocks configuration for that metric, so TidesKV is never compared with a
  weaker Kvrocks setting. Errored configurations stay visible in the table.
- A case row says "mixed" when TidesKV wins one metric and Kvrocks wins another; otherwise the row takes the label of
  its headline metric (the first listed), so a tie headline with a win for one side elsewhere stays "tie". The row's
  Reason lists every metric with a non-tie verdict, so such a win is never hidden. There is no weighting across metrics.
- Latency verdicts are given only at equal offered load: the open-loop cells of U1 and U3, and U2's churn, which runs at a
  fixed rate identical for both servers. Closed-loop latency is shown as information and never compared. A cell
  that reached under 95% of its target rate ("under target"), or whose memtier used over 85% of the client CPUs
  ("client-bound"), gets no latency verdict.

### Reading the report

`report.md` opens with a summary table (one row per case: the headline metric for each server, the verdict label and
a one-line meaning), then one section per case: what it models, the question, the setup read back from the servers,
per-metric tables with ratio, band and verdict, notes, then the F1 matrix and the F2 table, and a methodology section
at the end. Cells show `median [min-max]`. `ERR: ...` cells name why a cell has no number. A quick run opens with the
banner "quick: not a disk result, no verdicts". `summary.json` holds the same numbers for other tools.

F1 lists each server's pros and cons grouped by root cause, and F2 lists operating facts for both servers. U3's "windows
over 10x median p99.9" counts windows of 10 seconds (the max of ten one-second p99.9 points each) whose value is above 10x
the median window.

### Not covered in v1

- Replication, failover and slot migration under load (needs a multi-node harness).
- Memory sweeps from 1x to 10x RAM, relaxed-mode pipeline and connection sweeps, scans after deletes.
- Power-loss simulation: U7 kills the server process only.
- MULTI/Lua atomicity across a crash.
- Kvrocks features TidesKV lacks (streams, geo, JSON, search...) are listed in F1 and F2, not benchmarked.
- Any change to either server's code, or tuning beyond the settings in the report.

### For reviewers (Kvrocks and TidesKV maintainers)

Please review both servers' configurations: TidesKV's `tideskv.conf` and its sync modes (`none` for relaxed, `full`
for strict), and Kvrocks' `kvrocks.conf`, the cron and blob variants in `uc_cases.sh`, and the settings the report lists
as read back. If a setting is not what you would run in production, or a workload favours one side, say so: every
setting, command line and raw result is in the run directory, and a corrected configuration can be re-run and published
next to the original.
