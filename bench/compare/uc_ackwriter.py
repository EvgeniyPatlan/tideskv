#!/usr/bin/env python3
"""Acknowledged-write checker for the use-case suite's U7 (stdlib only; runs in the PY_IMAGE container).

  write  --host H --port P --conns N --prefix PFX --log FILE [--command SET]
      N threads, each doing `SET PFX<thread>:<seq> <seq>` (seq from 0) in a loop at pipeline 1. After each +OK the
      thread appends `key\\n` to FILE.<thread> and flushes. A thread stops when its connection is lost (a write
      whose reply never came is not logged) or when the server answers with an error. Prints `acked <total>` and
      exits 0, or exits 2 if any thread stopped on an error reply or could not connect. --command is a test hook.
  verify --host H --port P --log FILE --out JSON
      Reads every FILE.*, MGETs the keys in batches of 1000 and writes {"acked", "lost", "lost_sample" (first 10)}:
      a key counts as lost when MGET returns nil. Retries while the server is not reachable or answers an error
      (up to 60 s).
  probe  --host H --port P --out JSON (--until-p99 MS --timeout S | --baseline-secs S)
         (--log FILE | --prefix PFX --keymax N) [--t0 EPOCH_MS]
      One connection. Recovery mode (--until-p99): PING until PONG (pong_ms), then GET random keys in 1 s windows
      until a window's p99 <= MS; recovered_ms is the end of that window. Both are measured from t0 (default: the
      probe's own start), so pass the host's epoch ms taken just before starting the server. Within --timeout
      seconds of t0, else pong_ms / recovered_ms are null. Baseline mode (--baseline-secs S): GETs for S seconds,
      no PING wait, writes {"p99_ms", "requests", "windows"}. Keys come from the writer's logs (--log: acked keys)
      or are PFX<1..keymax> (the preload).
"""
import argparse
import glob
import json
import math
import os
import random
import socket
import sys
import threading
import time

BATCH = 1000
WINDOW_S = 1.0
LOG_KEYS_PER_FILE = 20000


class Conn:
    """A blocking RESP connection: command() returns the decoded reply, raises OSError on connection loss."""

    def __init__(self, host, port, timeout=30):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.rf = self.sock.makefile("rb")

    def close(self):
        try:
            self.rf.close()
            self.sock.close()
        except OSError:
            pass

    def command(self, *args):
        out = [b"*%d\r\n" % len(args)]
        for a in args:
            a = a if isinstance(a, bytes) else str(a).encode()
            out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        self.sock.sendall(b"".join(out))
        return self._read()

    def _read(self):
        line = self.rf.readline()
        if not line.endswith(b"\r\n"):
            raise ConnectionError("connection closed")
        kind, rest = line[:1], line[1:-2]
        if kind == b"+":
            return rest.decode()
        if kind == b"-":
            return RuntimeError(rest.decode())  # returned, not raised: an error reply is not a lost connection
        if kind == b":":
            return int(rest)
        if kind == b"$":
            n = int(rest)
            if n < 0:
                return None
            data = self.rf.read(n + 2)
            if len(data) != n + 2:
                raise ConnectionError("connection closed")
            return data[:-2]
        if kind == b"*":
            return [self._read() for _ in range(int(rest))]
        raise ConnectionError("bad reply %r" % line)


def connect_retry(host, port, until):
    """A Conn, retrying refused connects and name lookups until the monotonic time UNTIL; None on timeout."""
    while True:
        try:
            return Conn(host, port)
        except OSError:
            if time.monotonic() >= until:
                return None
            time.sleep(0.01)


# ---- write ----

def _writer(host, port, prefix, log_path, command, idx, result):
    conn = connect_retry(host, port, time.monotonic() + 10)
    if conn is None:
        result[idx] = (0, "could not connect")
        return
    acked, error, seq = 0, None, 0
    try:
        with open("%s.%d" % (log_path, idx), "w") as log:
            while True:
                key = "%s%d:%d" % (prefix, idx, seq)
                reply = conn.command(command, key, seq)
                if isinstance(reply, RuntimeError):
                    error = str(reply)
                    break
                log.write(key + "\n")
                log.flush()
                acked += 1
                seq += 1
    except OSError:
        pass  # the server went away: the normal end
    conn.close()
    result[idx] = (acked, error)


def cmd_write(a):
    result = [None] * a.conns
    threads = [threading.Thread(target=_writer, args=(a.host, a.port, a.prefix, a.log, a.command, i, result))
               for i in range(a.conns)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    errors = [r[1] for r in result if r and r[1]]
    for e in errors:
        print("writer stopped: %s" % e, file=sys.stderr)
    print("acked %d" % sum(r[0] for r in result if r))
    return 2 if errors else 0


# ---- verify ----

def _log_keys(log_path):
    for path in sorted(glob.glob(log_path + ".*")):
        with open(path) as f:
            for line in f:
                key = line.strip()
                if key:
                    yield key


def _mget(conn_box, a, keys):
    """MGET KEYS, reconnecting and retrying for up to 60 s while the server is down or still loading."""
    deadline = time.monotonic() + 60
    while True:
        try:
            if conn_box[0] is None:
                conn_box[0] = connect_retry(a.host, a.port, deadline)
                if conn_box[0] is None:
                    raise ConnectionError("cannot connect to %s:%d" % (a.host, a.port))
            reply = conn_box[0].command("MGET", *keys)
            if not isinstance(reply, RuntimeError):
                return reply
            err = str(reply)
        except OSError as e:
            conn_box[0] = None
            err = str(e)
        if time.monotonic() >= deadline:
            raise ConnectionError("MGET kept failing: %s" % err)
        time.sleep(1)


def cmd_verify(a):
    acked, lost, sample, batch, box = 0, 0, [], [], [None]

    def check():
        nonlocal lost
        for key, val in zip(batch, _mget(box, a, batch)):
            if val is None:
                lost += 1
                if len(sample) < 10:
                    sample.append(key)

    for key in _log_keys(a.log):
        acked += 1
        batch.append(key)
        if len(batch) == BATCH:
            check()
            batch = []
    if batch:
        check()
    with open(a.out, "w") as f:
        json.dump({"acked": acked, "lost": lost, "lost_sample": sample}, f)
    print("acked %d lost %d" % (acked, lost))
    return 0


# ---- probe ----

def _p99(lat):
    s = sorted(lat)
    return s[max(0, math.ceil(0.99 * len(s)) - 1)] if s else None


def _probe_keys(a):
    if a.log:
        keys = []
        for path in sorted(glob.glob(a.log + ".*")):
            with open(path) as f:
                keys += [k.strip() for _, k in zip(range(LOG_KEYS_PER_FILE), f) if k.strip()]
        if not keys:
            raise SystemExit("probe: no keys in %s.*" % a.log)
        return lambda: random.choice(keys)
    if not a.prefix or not a.keymax:
        raise SystemExit("probe: give --log, or --prefix and --keymax")
    return lambda: "%s%d" % (a.prefix, random.randint(1, a.keymax))


def _window(conn, pick, seconds):
    """GETs for SECONDS on CONN; returns (latencies in ms, error or None)."""
    lat, end = [], time.perf_counter() + seconds
    try:
        while time.perf_counter() < end:
            t = time.perf_counter()
            reply = conn.command("GET", pick())
            lat.append((time.perf_counter() - t) * 1000)
            if isinstance(reply, RuntimeError):
                return lat, str(reply)
    except OSError as e:
        return lat, str(e)
    return lat, None


def _wait_pong(a, t0, deadline):
    """(conn, pong_ms) once the server answers PING, or (None, None) at the deadline (monotonic clock)."""
    conn = None
    while time.monotonic() < deadline:
        conn = conn or connect_retry(a.host, a.port, deadline)
        if conn is None:
            break
        try:
            if conn.command("PING") == "PONG":
                return conn, (time.time() - t0) * 1000
        except OSError:
            conn.close()
            conn = None
        time.sleep(0.01)
    return None, None


def _win_record(t0, start, lat, err):
    return {"start_ms": round((start - t0) * 1000), "requests": len(lat),
            "p99_ms": round(_p99(lat), 3) if lat else None, "error": err}


def probe_recovery(a, pick, t0):
    deadline = time.monotonic() + max(0.0, t0 + a.timeout - time.time())
    res = {"pong_ms": None, "recovered_ms": None, "until_p99_ms": a.until_p99, "windows": []}
    conn, pong = _wait_pong(a, t0, deadline)
    if conn is None:
        return res
    res["pong_ms"] = round(pong)
    while time.monotonic() < deadline:
        start = time.time()
        lat, err = _window(conn, pick, min(WINDOW_S, max(0.0, deadline - time.monotonic())))
        w = _win_record(t0, start, lat, err)
        res["windows"].append(w)
        if err:
            conn.close()
            conn = connect_retry(a.host, a.port, deadline)
            if conn is None:
                break
        elif lat and time.monotonic() < deadline and w["p99_ms"] <= a.until_p99:
            res["recovered_ms"] = round((time.time() - t0) * 1000)
            break
    return res


def probe_baseline(a, pick):
    conn = connect_retry(a.host, a.port, time.monotonic() + 30)
    if conn is None:
        raise SystemExit("probe: cannot connect to %s:%d" % (a.host, a.port))
    all_lat, windows, t0 = [], [], time.time()
    while time.time() - t0 < a.baseline_secs:
        start = time.time()
        lat, err = _window(conn, pick, min(WINDOW_S, a.baseline_secs - (start - t0)))
        if err:
            raise SystemExit("probe: baseline read failed: %s" % err)
        all_lat += lat
        windows.append(_win_record(t0, start, lat, None))
    return {"p99_ms": round(_p99(all_lat), 3) if all_lat else None, "requests": len(all_lat), "windows": windows}


def cmd_probe(a):
    if (a.until_p99 is None) == (a.baseline_secs is None):
        raise SystemExit("probe: give --until-p99 (with --timeout) or --baseline-secs")
    pick = _probe_keys(a)
    t0 = a.t0 / 1000.0 if a.t0 else time.time()
    res = probe_baseline(a, pick) if a.baseline_secs is not None else probe_recovery(a, pick, t0)
    with open(a.out, "w") as f:
        json.dump(res, f)
    print(json.dumps({k: v for k, v in res.items() if k != "windows"}))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="mode", required=True)
    for name in ("write", "verify", "probe"):
        p = sub.add_parser(name)
        p.add_argument("--host", required=True)
        p.add_argument("--port", type=int, required=True)
    w = sub.choices["write"]
    w.add_argument("--conns", type=int, required=True)
    w.add_argument("--prefix", required=True)
    w.add_argument("--log", required=True)
    w.add_argument("--command", default="SET", help=argparse.SUPPRESS)
    v = sub.choices["verify"]
    v.add_argument("--log", required=True)
    v.add_argument("--out", required=True)
    p = sub.choices["probe"]
    p.add_argument("--out", required=True)
    p.add_argument("--until-p99", type=float)
    p.add_argument("--timeout", type=float, default=300)
    p.add_argument("--baseline-secs", type=float)
    p.add_argument("--log")
    p.add_argument("--prefix")
    p.add_argument("--keymax", type=int)
    p.add_argument("--t0", type=int, help="epoch ms the server was started (default: now)")
    a = ap.parse_args(argv)
    return {"write": cmd_write, "verify": cmd_verify, "probe": cmd_probe}[a.mode](a)


if __name__ == "__main__":
    sys.exit(main())
