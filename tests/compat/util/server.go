// Package util starts tideskv-server for the compatibility suites ported from Apache Kvrocks.
package util

import (
	"context"
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
} // tideskv: replication knobs added in M6b

type KvrocksServer struct {
	t    testing.TB
	cmd  *exec.Cmd
	addr *net.TCPAddr
	dir  string
	log  *os.File
}

func freePort(t testing.TB) int {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port
}

// StartServer launches tideskv-server on a free port with a private data dir and waits for PING.
func StartServer(t testing.TB, configs map[string]string) *KvrocksServer {
	require.NotEmpty(t, *binPath, "pass -binPath=<tideskv-server>")
	dir := t.TempDir()
	port := freePort(t)
	args := []string{"--port", strconv.Itoa(port), "--dir", dir, "--loglevel", "warning", "--workers", "4"}
	for k, v := range configs {
		if !supportedConfigs[k] {
			t.Logf("tideskv: ignoring unsupported config %q", k)
			continue
		}
		args = append(args, "--"+k, v)
	}
	log, err := os.Create(filepath.Join(dir, "server.log"))
	require.NoError(t, err)
	cmd := exec.Command(*binPath, args...)
	cmd.Stdout, cmd.Stderr = log, log
	require.NoError(t, cmd.Start())
	s := &KvrocksServer{t: t, cmd: cmd, addr: &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1), Port: port}, dir: dir, log: log}
	s.waitReady()
	return s
}

func (s *KvrocksServer) waitReady() {
	c := redis.NewClient(&redis.Options{Addr: s.HostPort(), DialTimeout: 200 * time.Millisecond})
	defer c.Close()
	for i := 0; i < 100; i++ {
		if c.Ping(context.Background()).Err() == nil {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	out, _ := os.ReadFile(s.log.Name())
	require.FailNow(s.t, fmt.Sprintf("tideskv-server did not become ready; log:\n%s", out))
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
	require.NoError(s.t, s.cmd.Process.Signal(syscall.SIGTERM))
	done := make(chan error, 1)
	go func() { done <- s.cmd.Wait() }()
	select {
	case err := <-done:
		require.NoError(s.t, err, "tideskv-server exited with an error")
	case <-time.After(10 * time.Second):
		_ = s.cmd.Process.Kill()
		require.Fail(s.t, "tideskv-server did not stop within 10s")
	}
	_ = s.log.Close()
}
