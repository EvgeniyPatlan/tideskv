// Package util starts tideskv-server for the compatibility suites ported from Apache Kvrocks.
package util

import (
	"errors"
	"flag"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"syscall"
	"testing"
	"time"

	"github.com/redis/go-redis/v9"
	"github.com/stretchr/testify/require"
)

var binPath = flag.String("binPath", "", "path to the tideskv-server binary")

// KvrocksServerConfigs keeps the upstream type name so ported tests compile unchanged.
type KvrocksServerConfigs = map[string]string

// Config keys tideskv understands; anything else from an upstream test is ignored (logged).
var supportedConfigs = map[string]bool{
	"requirepass": true, "maxclients": true, "workers": true,
	"replica-announce-ip": true, "replica-announce-port": true, "masterauth": true, "repl-timeout": true,
	"repl-backlog-size": true, "replica-serve-stale-data": true, "min-replicas-to-write": true,
	"min-replicas-max-lag": true, "key-load-delay": true, "rdb-key-save-delay": true,
	"bind": true, "cluster-enabled": true, // tideskv: cluster knobs added in M7a
} // tideskv: replication knobs added in M6b

type KvrocksServer struct {
	t        testing.TB
	proc     *child
	addr     *net.TCPAddr
	dir      string
	log      *os.File
	args     []string // tideskv: everything but --port, kept for Start/Restart
	password string   // tideskv: requirepass, for the readiness probe
}

// child is one run of tideskv-server; err is valid once done is closed.
type child struct {
	cmd  *exec.Cmd
	done chan struct{}
	err  error
}

const (
	spawnAttempts = 5
	exitPortInUse = 3 // tideskv-server: the port is taken (as in tests/support/test_server.cc)
	readyTimeout  = 10 * time.Second
)

func freePort(t testing.TB) int {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port
}

// pickPort chooses a fresh port for StartServer (a variable so the harness test can hand out a taken one).
var pickPort = freePort

// StartServer launches tideskv-server on a free port with a private data dir and waits until it answers.
func StartServer(t testing.TB, configs map[string]string) *KvrocksServer {
	require.NotEmpty(t, *binPath, "pass -binPath=<tideskv-server>")
	dir := t.TempDir()
	args := []string{"--dir", dir, "--loglevel", "warning", "--workers", "4"}
	for k, v := range configs {
		if !supportedConfigs[k] {
			t.Logf("tideskv: ignoring unsupported config %q", k)
			continue
		}
		args = append(args, "--"+k, v)
	}
	s := &KvrocksServer{t: t, addr: &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1), Port: pickPort(t)}, dir: dir,
		args: args, password: configs["requirepass"]}
	s.start(true)
	return s
}

// Start runs the server again with the same arguments, dir and port (after Close), and waits until it answers.
func (s *KvrocksServer) Start() { s.start(false) }

// Restart stops the server cleanly, then starts it again.
func (s *KvrocksServer) Restart() {
	s.Close()
	s.Start()
}

// start spawns the server until it owns its port. freePort's probe is closed before the server binds, so a
// parallel test package may take a fresh port first: the server exits with code 3 and gets another port. A
// fixed port (Start after Close) may still be held for a moment by the predecessor: retried as is.
func (s *KvrocksServer) start(freshPort bool) {
	for attempt := 1; ; attempt++ {
		s.spawn()
		if s.waitReady() {
			return
		}
		if attempt == spawnAttempts || !s.exitedWithPortInUse() {
			_ = s.proc.cmd.Process.Kill()
			<-s.proc.done
			out, _ := os.ReadFile(s.log.Name())
			_ = s.log.Close()
			require.FailNow(s.t, fmt.Sprintf("tideskv-server did not become ready on port %d (%v); log:\n%s",
				s.addr.Port, s.proc.err, out))
		}
		_ = s.log.Close()
		if freshPort {
			s.addr.Port = pickPort(s.t)
		} else {
			time.Sleep(200 * time.Millisecond)
		}
	}
}

func (s *KvrocksServer) spawn() {
	log, err := os.OpenFile(filepath.Join(s.dir, "server.log"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	require.NoError(s.t, err)
	args := append([]string{"--port", strconv.Itoa(s.addr.Port)}, s.args...)
	cmd := exec.Command(*binPath, args...)
	cmd.Stdout, cmd.Stderr = log, log
	require.NoError(s.t, cmd.Start())
	p := &child{cmd: cmd, done: make(chan struct{})}
	go func() {
		p.err = cmd.Wait()
		close(p.done)
	}()
	s.proc, s.log = p, log
}

// waitReady reports whether the server answered as itself: INFO's process_id must be our child's, since a
// PING alone may reach another package's server on the same port. False once the child exits.
func (s *KvrocksServer) waitReady() bool {
	c := redis.NewClient(&redis.Options{Addr: s.HostPort(), Password: s.password, DialTimeout: 200 * time.Millisecond})
	defer c.Close()
	pid := strconv.Itoa(s.proc.cmd.Process.Pid)
	for deadline := time.Now().Add(readyTimeout); time.Now().Before(deadline); {
		select {
		case <-s.proc.done:
			return false
		default:
		}
		if FindInfoEntry(c, "process_id", "server") == pid {
			return true
		}
		time.Sleep(50 * time.Millisecond)
	}
	return false
}

func (s *KvrocksServer) exitedWithPortInUse() bool {
	select {
	case <-s.proc.done:
		var exitErr *exec.ExitError
		return errors.As(s.proc.err, &exitErr) && exitErr.ExitCode() == exitPortInUse
	default:
		return false
	}
}

func (s *KvrocksServer) HostPort() string { return s.addr.String() }

func (s *KvrocksServer) Host() string { return s.addr.IP.String() }

func (s *KvrocksServer) Port() uint64 { return uint64(s.addr.Port) }

func (s *KvrocksServer) NewClient() *redis.Client { return s.NewClientWithOption(&redis.Options{}) }

func (s *KvrocksServer) NewClientWithOption(o *redis.Options) *redis.Client {
	if o.Addr == "" {
		o.Addr = s.HostPort()
	}
	o.DialTimeout, o.ReadTimeout, o.WriteTimeout = 30*time.Second, 30*time.Second, 30*time.Second
	return redis.NewClient(o)
}

func (s *KvrocksServer) NewTCPClient() *TCPClient {
	c, err := net.Dial("tcp", s.HostPort())
	require.NoError(s.t, err)
	return newTCPClient(c)
}

// Close stops the server with SIGTERM and fails the test if it doesn't exit cleanly.
func (s *KvrocksServer) Close() {
	select {
	case <-s.proc.done: // already gone: its exit status is reported below
	default:
		require.NoError(s.t, s.proc.cmd.Process.Signal(syscall.SIGTERM))
	}
	select {
	case <-s.proc.done:
		require.NoError(s.t, s.proc.err, "tideskv-server exited with an error")
	case <-time.After(10 * time.Second):
		_ = s.proc.cmd.Process.Kill()
		require.Fail(s.t, "tideskv-server did not stop within 10s")
	}
	_ = s.log.Close()
}
