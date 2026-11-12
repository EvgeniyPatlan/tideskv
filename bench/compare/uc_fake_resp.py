"""In-process fake RESP server for testing the use-case suite's Python tools (stdlib only).

    from uc_fake_resp import FakeResp
    srv = FakeResp(startup_delay=0.3, drop_last=3)
    srv.start()                       # returns at once; the server answers after startup_delay seconds
    ... talk to srv.host:srv.port ...
    srv.drop("k0:7")                  # forget one acked key
    srv.kill()                        # close every client socket and stop; the last drop_last acked SETs are lost
    srv.restart(startup_delay=0.1)    # same port, same (surviving) data
    srv.stop()                        # kill() without dropping anything

Commands: PING, SET key value, GET key, MGET key..., DBSIZE; anything else gets an error reply.
`store` is the dict (bytes -> bytes). `acked` is the keys of acknowledged SETs in order, `unacked` the keys of
SETs whose connection was closed before the reply (close_before_reply_key). Options (constructor):
  startup_delay        seconds after start()/restart() before the first connection is accepted; connects fail
                       until then (the port is reserved, nothing listens), as a restarting server's would
  drop_last            kill() removes the last k acked writes from `store`, as a crash that lost unsynced data
  close_before_reply_key  a SET of this key (str) is stored but its connection is closed without replying
"""
import socket
import socketserver
import threading
import time


class _Handler(socketserver.StreamRequestHandler):
    def handle(self):
        fake = self.server.fake
        fake._track(self.request)
        try:
            while True:
                cmd = self._read_command()
                if cmd is None:
                    return
                reply = fake._execute(cmd)
                if reply is None:  # close before replying
                    return
                self.wfile.write(reply)
                self.wfile.flush()
        except OSError:
            return
        finally:
            fake._untrack(self.request)

    def _read_command(self):
        line = self.rfile.readline()
        if not line:
            return None
        if not line.startswith(b"*"):
            return line.split()
        n = int(line[1:])
        args = []
        for _ in range(n):
            hdr = self.rfile.readline()
            size = int(hdr[1:])
            args.append(self.rfile.read(size + 2)[:-2])
        return args


class _Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class FakeResp:
    def __init__(self, host="127.0.0.1", startup_delay=0.0, drop_last=0, close_before_reply_key=None):
        self.host = host
        self.startup_delay = startup_delay
        self.drop_last = drop_last
        self.close_before_reply_key = close_before_reply_key.encode() if close_before_reply_key else None
        self.store = {}
        self.acked = []
        self.unacked = []
        self._lock = threading.Lock()
        self._socks = set()
        self._server = None
        self._thread = None
        self._stopping = threading.Event()
        with socket.socket() as s:  # reserve a port for the server's whole life
            s.bind((host, 0))
            self.port = s.getsockname()[1]

    def start(self):
        self._stopping.clear()
        self._thread = threading.Thread(target=self._serve, args=(self.startup_delay,), daemon=True)
        self._thread.start()
        return self

    def restart(self, startup_delay=None):
        self._join()
        if startup_delay is not None:
            self.startup_delay = startup_delay
        self.start()

    def _serve(self, delay):
        if self._stopping.wait(delay):
            return
        server = _Server((self.host, self.port), _Handler)
        server.fake = self
        self._server = server
        server.serve_forever(poll_interval=0.02)

    def kill(self, drop_last=None):
        """Stop serving, close every client socket, and drop the last k acked writes from the store."""
        k = self.drop_last if drop_last is None else drop_last
        self._join()
        with self._lock:
            for key in self.acked[len(self.acked) - k:] if k else []:
                self.store.pop(key, None)

    def drop(self, *keys):
        """Remove these keys (str) from the store, as if their acked writes had been lost."""
        with self._lock:
            for k in keys:
                self.store.pop(k.encode(), None)

    def stop(self):
        self.kill(drop_last=0)

    def _join(self):
        self._stopping.set()
        deadline = time.time() + 5
        while self._server is None and self._thread is not None and self._thread.is_alive() \
                and time.time() < deadline:
            time.sleep(0.01)  # still in its startup delay
        server, self._server = self._server, None
        if server is not None:
            server.shutdown()
            server.server_close()
        with self._lock:
            socks, self._socks = list(self._socks), set()
        for s in socks:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            s.close()
        if self._thread is not None:
            self._thread.join(timeout=5)
            self._thread = None

    def _track(self, sock):
        with self._lock:
            self._socks.add(sock)

    def _untrack(self, sock):
        with self._lock:
            self._socks.discard(sock)

    def _execute(self, cmd):
        op = cmd[0].upper()
        with self._lock:
            if op == b"PING":
                return b"+PONG\r\n"
            if op == b"SET" and len(cmd) >= 3:
                self.store[cmd[1]] = cmd[2]
                if cmd[1] == self.close_before_reply_key:
                    self.unacked.append(cmd[1])
                    return None
                self.acked.append(cmd[1])
                return b"+OK\r\n"
            if op == b"GET" and len(cmd) == 2:
                return self._bulk(self.store.get(cmd[1]))
            if op == b"MGET" and len(cmd) >= 2:
                return b"*%d\r\n" % (len(cmd) - 1) + b"".join(self._bulk(self.store.get(k)) for k in cmd[1:])
            if op == b"DBSIZE":
                return b":%d\r\n" % len(self.store)
        return b"-ERR unknown command\r\n"

    @staticmethod
    def _bulk(v):
        return b"$-1\r\n" if v is None else b"$%d\r\n%s\r\n" % (len(v), v)
