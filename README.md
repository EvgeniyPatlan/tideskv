# TidesKV

TidesKV is a Redis-compatible key-value server written in C++20. It stores data on disk, using the [TidesDB](https://github.com/tidesdb/tidesdb) storage engine.

It speaks RESP2 and RESP3 and follows Redis 7.2 semantics for the commands it supports. It also offers master–replica replication and a cluster mode that the [Apache Kvrocks](https://kvrocks.apache.org/) controller can manage.

The aim is a production alternative to Kvrocks: a dataset larger than RAM, durable writes, and existing Redis clients unchanged.

> **Status:** pre-1.0 (`0.1.0`). Milestones M0–M7b are done: the data types, transactions, Lua, replication, and cluster mode with slot migration. M8 remains, covering performance tuning, packaging and docs, and the v1.0 release gate. Interfaces and the on-disk format may still change.

## Features

**Data types**
- Strings, hashes, lists (including blocking pops and moves), sets and sorted sets.
- TTLs, with lazy expiry plus a background reaper.

**Keyspace**
- `SCAN`, `KEYS`, `RENAME`, `RANDOMKEY`, `DBSIZE`, `FLUSHDB` and `FLUSHALL`.
- `SELECT` across 16 databases.

**Transactions and scripting**
- `MULTI`, `EXEC`, `DISCARD` and `WATCH`.
- Lua 5.1 scripting with `EVAL`, `EVALSHA`, `SCRIPT` and the read-only variants, matching Redis's sandbox and replies.

**Protocol and connections**
- RESP2 and RESP3 (`HELLO`).
- `AUTH`, `CLIENT`, `CONFIG GET/SET`, `INFO`, `COMMAND` and `DEBUG`.

**Durability**
- Three sync modes:
  - `none`: the OS flushes.
  - `interval`: one fsync every `sync-interval-us`.
  - `full`: every acknowledged write is fsynced.

**Replication**
- `REPLICAOF`, full sync, and partial resync from a backlog (PSYNC2-style).
- `WAIT`, `min-replicas-to-write`, and failover with replica promotion.

**Cluster mode** (`cluster-enabled yes`)
- Redis Cluster routing: `MOVED`, `ASK`, `CROSSSLOT`, `CLUSTERDOWN` and `TRYAGAIN`.
- `CLUSTER` views, and topology pushed through Kvrocks's `CLUSTERX` protocol, so [kvrocks-controller](https://github.com/apache/kvrocks-controller) 1.3.0 can manage a TidesKV cluster.
- Online slot migration (`CLUSTERX MIGRATE`), with a per-slot write fence and no lost acknowledged writes.

**Not supported yet** (planned for v1.x): Streams, Bitmaps, HyperLogLog, Geo, Pub/Sub, JSON, Search, TLS and ACLs. `MOVE`, `COPY` and `SWAPDB` are also missing. On-disk compatibility with Redis RDB or Kvrocks is a non-goal.

Run `COMMAND LIST` against a server for the exact command set.

For a guided demo and a tour of how TidesKV works, see the [tutorial](docs/tutorial.md).

## Quick start

### Build

You need Linux (x86_64 or arm64), CMake ≥ 3.24, Ninja, and GCC 13 or Clang 18. LZ4 and Zstd development headers are needed for compression. On Ubuntu 24.04:

```bash
sudo apt-get install -y cmake ninja-build build-essential git liblz4-dev libzstd-dev
cmake --preset release
cmake --build --preset release -j
```

The binary is `build/release/src/tideskv-server`. CMake fetches its dependencies, TidesDB, asio, spdlog and GoogleTest, at pinned versions. To build without compression libraries, add `-DTIDESKV_COMPRESSION=OFF`.

### Run

```bash
build/release/src/tideskv-server                       # defaults: 127.0.0.1:6666, data in ./data
build/release/src/tideskv-server tideskv.conf          # Redis-style "name value" config file
build/release/src/tideskv-server --port 6380 --dir /var/lib/tideskv --sync-mode full
```

Settings are applied in this order of precedence: config file < environment (`TIDESKV_REQUIREPASS`) < command-line `--name value` pairs.

Connect with any Redis client:

```bash
redis-cli -p 6666 SET greeting hello
redis-cli -p 6666 GET greeting
```

Exit codes: 0 on a clean shutdown, 1 for a configuration error, 2 when storage fails to open, 3 when it cannot listen.

## Configuration

These are the most used settings. `CONFIG GET *` lists all of them with their current values.

| Setting | Default | Meaning |
|---|---|---|
| `bind` / `port` | `127.0.0.1` / `6666` | Listen address |
| `dir` | `./data` | Data directory |
| `workers` | `0` (one per CPU) | Worker threads |
| `sync-mode` | `interval` | `none`, `interval` or `full` |
| `sync-interval-us` | `1000000` | fsync period for `interval` |
| `block-cache-size` | `4gb` | TidesDB block cache |
| `memtable-size` | `64mb` | TidesDB memtable size |
| `l1-file-count-trigger` | `8` | Flush-tier tables per column family that start a merge |
| `l0-stall-threshold` | `64` | Write backpressure: throttle from half this backlog, block at it |
| `compression` | `lz4` | `no`, `lz4` or `zstd` |
| `requirepass` | empty | Password for `AUTH` |
| `maxclients` | `10000` | Connection limit |
| `lua-time-limit` | `5000` ms | When a long script starts answering `BUSY` |
| `replicaof` | empty | `<host> <port>` to start as a replica |
| `repl-backlog-size` | `1gb` | Backlog kept for partial resync |
| `min-replicas-to-write` | `0` | Refuse writes without enough healthy replicas |
| `cluster-enabled` | `no` | Cluster mode (startup only) |

The replication and migration settings keep their Redis and Kvrocks names. Examples are `masterauth`, `replica-read-only`, `repl-timeout` and `migrate-batch-rate-limit-mb`.

## Replication

```bash
# on the replica
redis-cli -p 6667 REPLICAOF 127.0.0.1 6666
redis-cli -p 6667 ROLE
```

A new or diverged replica gets a full sync, which is a logical snapshot stream. After that it follows the master's replication log, and it resumes from the backlog after short disconnects.

`WAIT numreplicas timeout` reports how many replicas have acknowledged the writes so far. `REPLICAOF NO ONE` promotes a replica.

## Cluster mode

Start each node with `cluster-enabled yes`, then let a controller push the topology. Use [kvrocks-controller](https://github.com/apache/kvrocks-controller) with etcd, or call `CLUSTERX SETNODEID` and `CLUSTERX SETNODES` yourself. Nodes don't gossip with each other; the controller is the source of truth.

Redis Cluster clients work unchanged: they follow `MOVED` and `ASK`, and use `READONLY` on replicas.

To move slots between masters, use `CLUSTERX MIGRATE <slot|from-to> <target-node-id>`; the controller's migrate API calls it. The source streams the slot's data to the target. It then briefly fences writes to those slots (`TRYAGAIN`) while it sends the final changes, and the controller assigns the slots to the target. The target cleans up a half-finished import if anything fails.

## Development

```bash
cmake --preset debug && cmake --build --preset debug -j
ctest --preset debug -j8                                # unit, contract, integration, multinode, crash
cmake --preset asan  && cmake --build --preset asan -j && ctest --preset asan -j8
cmake --preset tsan  && cmake --build --preset tsan -j && ctest --preset tsan -j8
scripts/coverage.sh                                     # llvm-cov line coverage, gate 80 %
scripts/compat.sh build/release/src/tideskv-server      # Go compatibility suites (needs Go ≥ 1.22)
scripts/docker-ci.sh                                    # every CI job in a clean Ubuntu 24.04 container
```

The tests are layered as follows:

| Layer | Contents |
|---|---|
| `tests/unit` | GoogleTest unit tests |
| `tests/contract` | Storage contract suite, run against the in-memory and TidesDB backends |
| `tests/integration` | A real server over TCP |
| `tests/multinode` | Replication and cluster scenarios: failover, migration under load, kill -9 |
| `tests/crash` | Kill -9 durability checks |
| `tests/fuzz` | libFuzzer targets for the RESP parser and the replication codecs |
| `tests/compat` | Go ports of the Apache Kvrocks integration suites, plus a differential test against Redis 7.2 |

CI runs debug, release, ASan/UBSan, TSan, coverage, clang-format, clang-tidy, fuzzing, crash tests and the compat suites. See `.github/workflows/ci.yml`.

### Source layout

| Directory | Contents |
|---|---|
| `src/net`, `src/resp` | Networking (asio) and the RESP2/3 parser and writer |
| `src/commands` | Command table, routing and the command implementations |
| `src/exec` | The engine: the single write path, transactions and per-slot write fences |
| `src/types`, `src/encoding` | Data types and their key/value encodings |
| `src/storage` | The `Storage` interface, with TidesDB and in-memory backends |
| `src/expire` | The background expiry reaper |
| `src/script` | Lua scripting |
| `src/repl` | Replication: the replication log, full sync and PSYNC |
| `src/cluster`, `src/migrate` | Topology, slot routing and slot migration |
| `src/server`, `src/app` | Configuration, sessions and `main` |

## Benchmarks

The comparison benchmark against Kvrocks is in progress under `bench/compare/`. Published numbers will follow it.

## License

TidesKV is released under the [MIT License](LICENSE).

Some test code is derived from Apache Kvrocks and is licensed under the Apache License 2.0. `third_party/` contains Lua, fpconv and sha1 under their own licenses. See [NOTICE](NOTICE) for the attributions.
