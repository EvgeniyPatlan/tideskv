package diff

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"tideskv.local/compat/util"
)

// kvrocksQuirks overrides a case's Norm for commands (upper-case name) where Kvrocks 2.17
// deviates from Redis 7.2 and TidesKV follows Redis. Each entry says how Kvrocks deviates.
var kvrocksQuirks = map[string]Norm{}

// TestKvrocksCases replays the C1..C8 common-case streams against TidesKV and Kvrocks 2.17:
// every reply must match (under the case's Norm or a kvrocksQuirks override) and so must the
// final datasets (DumpState; Kvrocks has no DEBUG DIGEST).
func TestKvrocksCases(t *testing.T) {
	kv := os.Getenv("TIDESKV_KVROCKS_ADDR")
	if kv == "" {
		if os.Getenv("TIDESKV_DIFF_REQUIRED") != "" {
			t.Fatal("TIDESKV_DIFF_REQUIRED is set but TIDESKV_KVROCKS_ADDR is empty: no Kvrocks 2.17 to diff against")
		}
		t.Skip("set TIDESKV_KVROCKS_ADDR to a Kvrocks 2.17 server (scripts/compat.sh --kvrocks does this)")
	}
	checkKvrocksVersion(t, kv)
	ours := os.Getenv("TIDESKV_OURS_ADDR")
	if ours == "" {
		srv := util.StartServer(t, map[string]string{})
		defer srv.Close()
		ours = srv.HostPort()
	}
	for _, c := range Cases() {
		c := c
		t.Run(c.Name, func(t *testing.T) {
			for _, seed := range seeds(t) {
				t.Run(fmt.Sprintf("seed-%d", seed), func(t *testing.T) { runCase(t, c, seed, ours, kv) })
			}
		})
	}
}

func checkKvrocksVersion(t *testing.T, addr string) {
	c := mustDial(t, addr)
	defer c.Close()
	info, err := c.Do("INFO", "server")
	if err != nil || !strings.Contains(info.Str, "kvrocks_version:2.17.") {
		t.Fatalf("reference at %s must be Kvrocks 2.17.x (INFO server lacks kvrocks_version:2.17.)", addr)
	}
}

func runCase(t *testing.T, c Case, seed int64, ours, kv string) {
	a, b := mustDial(t, ours), mustDial(t, kv)
	defer a.Close()
	defer b.Close()
	for _, conn := range []*Conn{a, b} {
		if _, err := do(conn, "FLUSHALL"); err != nil {
			t.Fatal(err)
		}
	}
	g := NewGen(seed)
	var history []string
	for _, cmd := range c.Setup(g) {
		history = compareStep(t, a, b, cmd, NormNone, history)
	}
	time.Sleep(1100 * time.Millisecond) // the Setup's EX 1 keys expire on both
	for step := 0; step < ops(); step++ {
		cmd, norm := c.Next(g)
		if q, ok := kvrocksQuirks[strings.ToUpper(cmd[0])]; ok {
			norm = q
		}
		history = compareStep(t, a, b, cmd, norm, history)
	}
	for name, conn := range map[string]*Conn{"tideskv": a, "kvrocks": b} {
		if err := conn.AssertDrained(); err != nil {
			t.Fatalf("%s: %v\nlast commands:\n  %s", name, err, lastCommands(history))
		}
	}
	compareState(t, a, b, history)
}

// compareStep sends cmd to both servers, fails on a divergence, and returns the extended history.
// Step numbers count the Setup commands too.
func compareStep(t *testing.T, a, b *Conn, cmd []string, norm Norm, history []string) []string {
	t.Helper()
	history = append(history, strconv.Quote(strings.Join(cmd, " ")))
	ra, rawA, errA := a.DoRaw(cmd...)
	rb, rawB, errB := b.DoRaw(cmd...)
	if errA != nil || errB != nil {
		t.Fatalf("step %d: transport error\ntideskv: %v\nkvrocks: %v\nlast commands:\n  %s", len(history)-1,
			errA, errB, lastCommands(history))
	}
	if !sameReply(norm, ra, rb, rawA, rawB) {
		t.Fatalf("step %d diverged\ncommand: %s\ntideskv: %s (wire %q)\nkvrocks: %s (wire %q)\nlast commands:\n  %s",
			len(history)-1, history[len(history)-1], ra, rawA, rb, rawB, lastCommands(history))
	}
	return history
}

func compareState(t *testing.T, a, b *Conn, history []string) {
	t.Helper()
	sa, err := DumpState(a)
	if err != nil {
		t.Fatalf("tideskv: dump state: %v", err)
	}
	sb, err := DumpState(b)
	if err != nil {
		t.Fatalf("kvrocks: dump state: %v", err)
	}
	if d := DiffState(sa, sb); len(d) > 0 {
		t.Fatalf("every reply matched but the datasets differ:\n  %s\nlast commands:\n  %s",
			strings.Join(d, "\n  "), lastCommands(history))
	}
	t.Logf("%d keys identical on both", len(sa))
}
