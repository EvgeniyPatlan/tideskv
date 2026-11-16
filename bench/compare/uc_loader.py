#!/usr/bin/env python3
"""Controlled-payload loader for the use-case suite (stdlib only; runs in the PY_IMAGE container).

Why not memtier: memtier's --random-data value is one buffer with one byte changed per request, the same sequence in
every client, so a set of values loaded in key order compresses to ~0.15 in 16 KiB blocks while a single value does not
compress at all. A block compressor (Kvrocks' RocksDB) then looks far better than on real data and a per-value one far
worse. This loader writes values of a fixed compressibility instead, as RocksDB's db_bench does (its
--compression_ratio 0.5): each value is ceil(size/2) random bytes followed by a copy of them, so one value compresses to
about half and values share nothing. A value depends only on (--seed, key index[, field]), so every server gets the
same bytes whatever --conns and --pipeline are.

  uc_loader.py --host H --port P --prefix PFX --keys N --value-size B --out JSON
               [--fields F] [--order seq|random] [--requests M] [--seed S] [--conns C] [--pipeline D]
    seq (default): SET PFX<i> for i = 1..N exactly once, split into contiguous ranges over C connections
                   (the key names of memtier's --key-pattern=P:P --key-minimum=1 --key-maximum=N).
    random:        M SETs (default N) of uniform random keys in 1..N; a different --seed gives every key a new value
                   (U3's aging overwrites).
    --fields F:    HSET PFX<i> f1 v1 .. fF vF instead (the field names of U4's memtier commands); field f of key i
                   holds value_for(seed, i * F + f - 1).
  Writes {"writes", "errors", "seconds", "ops_per_s", "keys", "value_size", "fields", "order", "seed", "conns",
  "pipeline"} to JSON. Exits 2 on an error reply or a lost or refused connection (the first error goes to stderr).
"""
import argparse
import json
import multiprocessing
import queue
import random
import socket
import sys
import time

MASK48 = (1 << 48) - 1


def value_for(seed, idx, size):
    """size bytes: ceil(size/2) random bytes (from seed and idx only) and their copy, cut to size."""
    half = (size + 1) // 2
    raw = random.Random((seed << 48) | (idx & MASK48)).randbytes(half)
    return (raw + raw)[:size]


def _bulk(b):
    return b"$%d\r\n%s\r\n" % (len(b), b)


def _command(a, idx, seed):
    key = a.prefix.encode() + str(idx).encode()
    if a.fields:
        args = [b"HSET", key]
        for f in range(1, a.fields + 1):
            args += [b"f%d" % f, value_for(seed, idx * a.fields + f - 1, a.value_size)]
    else:
        args = [b"SET", key, value_for(seed, idx, a.value_size)]
    return b"*%d\r\n" % len(args) + b"".join(_bulk(x) for x in args)


def _indexes(a, w):
    """The key indexes connection w writes, in order."""
    if a.order == "seq":
        lo = 1 + a.keys * w // a.conns
        hi = a.keys * (w + 1) // a.conns
        return range(lo, hi + 1)
    n = a.requests // a.conns + (1 if w < a.requests % a.conns else 0)
    rnd = random.Random((a.seed << 20) | w)
    return (rnd.randint(1, a.keys) for _ in range(n))


def _read_replies(rf, n):
    """Reads n simple replies; returns the first error line (str) or None."""
    err = None
    for _ in range(n):
        line = rf.readline()
        if not line:
            raise OSError("connection closed")
        if line[:1] == b"-" and err is None:
            err = line[1:].strip().decode(errors="replace")
        elif line[:1] == b"$":  # a bulk reply (not expected from SET/HSET); skip its body
            size = int(line[1:])
            if size >= 0:
                rf.read(size + 2)
    return err


def _worker(a, w, q):
    writes = 0
    try:
        sock = socket.create_connection((a.host, a.port), timeout=a.connect_timeout)
        sock.settimeout(300)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        rf = sock.makefile("rb")
        batch = []
        for idx in _indexes(a, w):
            batch.append(_command(a, idx, a.seed))
            if len(batch) == a.pipeline:
                sock.sendall(b"".join(batch))
                err = _read_replies(rf, len(batch))
                writes += len(batch)
                batch = []
                if err:
                    q.put((writes, err))
                    return
        if batch:
            sock.sendall(b"".join(batch))
            err = _read_replies(rf, len(batch))
            writes += len(batch)
            if err:
                q.put((writes, err))
                return
        sock.close()
        q.put((writes, None))
    except OSError as e:
        q.put((writes, "connection: %s" % e))
    except Exception as e:  # a reply it cannot parse, MemoryError, ...: report it, never leave run() waiting
        q.put((writes, "%s: %s" % (type(e).__name__, e)))


def _collect(q, procs):
    """One result per worker; a worker that died without reporting (a signal, a crash in the interpreter) counts as an
    error, so the loader never waits forever."""
    results = []
    while len(results) < len(procs):
        try:
            results.append(q.get(timeout=1))
        except queue.Empty:
            dead = sum(1 for p in procs if p.exitcode is not None)
            if dead > len(results) and q.empty():
                time.sleep(1)  # a result put just before exit may still be in flight
                while not q.empty():
                    results.append(q.get())
                lost = dead - len(results)
                results += [(0, "worker exited without a result")] * max(0, lost)
    return results


def run(a):
    q = multiprocessing.Queue()
    start = time.monotonic()
    procs = [multiprocessing.Process(target=_worker, args=(a, w, q)) for w in range(a.conns)]
    for p in procs:
        p.start()
    results = _collect(q, procs)
    for p in procs:
        p.join()
    secs = time.monotonic() - start
    writes = sum(r[0] for r in results)
    errors = [r[1] for r in results if r[1]]
    out = {"writes": writes, "errors": len(errors), "seconds": round(secs, 3),
           "ops_per_s": round(writes / secs) if secs > 0 else None, "keys": a.keys, "value_size": a.value_size,
           "fields": a.fields, "order": a.order, "seed": a.seed, "conns": a.conns, "pipeline": a.pipeline}
    with open(a.out, "w") as f:
        json.dump(out, f, indent=2)
    if errors:
        print("uc_loader: %s" % errors[0], file=sys.stderr)
        return 2
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--keys", type=int, required=True)
    ap.add_argument("--value-size", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--fields", type=int, default=0)
    ap.add_argument("--order", choices=("seq", "random"), default="seq")
    ap.add_argument("--requests", type=int)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--conns", type=int, default=8)
    ap.add_argument("--pipeline", type=int, default=32)
    ap.add_argument("--connect-timeout", type=float, default=30)
    a = ap.parse_args(argv)
    if a.keys < 1 or a.value_size < 1 or a.conns < 1 or a.pipeline < 1 or a.fields < 0:
        ap.error("--keys, --value-size, --conns and --pipeline must be positive, --fields not negative")
    if a.requests is None:
        a.requests = a.keys
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
